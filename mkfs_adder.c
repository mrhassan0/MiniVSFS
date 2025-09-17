// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_adder.c -o mkfs_adder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12

#pragma pack(push,1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;
    uint64_t root_inode;
    uint64_t mtime_epoch;
    uint32_t flags;
    uint32_t checksum;
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[12];
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;
    // THIS FIELD SHOULD STAY AT THE END
    uint64_t inode_crc;
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;       
    uint8_t  type;           
    char     name[58];
    uint8_t  checksum;
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent64 size mismatch");

// ====================================CRC32====================================
uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}
// ====================================CRC32====================================

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t c = crc32(sb, BS);
    sb->checksum = c;
    return c;
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER INODE ELEMENTS HAVE BEEN FINALIZED
static uint32_t inode_crc_finalize(inode_t* in) {
    // low 4 bytes store crc32 of bytes [0..119]; high 4 bytes 0
    in->inode_crc = 0;
    uint32_t c = crc32(in, 120);
    in->inode_crc = (uint64_t)c; // high 4 bytes remain 0
    return c;
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];
    de->checksum = x;
}

int main(int argc, char** argv) {
    crc32_init();

    
    const char *in_path=NULL, *out_path=NULL, *file_path=NULL;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--input")==0 && i+1<argc){ in_path=argv[++i]; }
        else if(strcmp(argv[i],"--output")==0 && i+1<argc){ out_path=argv[++i]; }
        else if(strcmp(argv[i],"--file")==0 && i+1<argc){ file_path=argv[++i]; }
    }
    if(!in_path || !out_path || !file_path){
        fprintf(stderr,"Usage: %s --input in.img --output out.img --file <filename>\n", argv[0]);
        return 1;
    }

    
    FILE* fi = fopen(in_path,"rb");
    if(!fi){ perror("fopen input"); return 1; }
    fseek(fi,0,SEEK_END);
    long fsz = ftell(fi);
    fseek(fi,0,SEEK_SET);
    size_t img_bytes = (size_t)fsz;
    uint8_t* img = (uint8_t*)malloc(img_bytes);
    if(!img){ fclose(fi); fprintf(stderr,"OOM\n"); return 1; }
    if(fread(img,1,img_bytes,fi)!=img_bytes){ fclose(fi); free(img); fprintf(stderr,"read image failed\n"); return 1; }
    fclose(fi);

    superblock_t* sb = (superblock_t*)img;
    uint8_t* inode_bitmap = img + sb->inode_bitmap_start*BS;
    uint8_t* data_bitmap  = img + sb->data_bitmap_start*BS;
    uint8_t* inode_table  = img + sb->inode_table_start*BS;
    uint8_t* data_region  = img + sb->data_region_start*BS;

    
    FILE* fadd = fopen(file_path,"rb");
    if(!fadd){ perror("fopen file"); free(img); return 1; }
    fseek(fadd,0,SEEK_END);
    long fsz_in = ftell(fadd);
    fseek(fadd,0,SEEK_SET);

    char fname[59]={0};
    const char* bn = strrchr(file_path,'/');
    if(!bn) bn = file_path; else bn++;
    strncpy(fname,bn,58);
    fname[58]='\0';

    
    uint64_t free_ino_index=(uint64_t)-1;
    for(uint64_t i=0;i<sb->inode_count;i++){
        uint8_t b = inode_bitmap[i>>3u];
        if(((b>>(i&7u))&1u)==0u){ free_ino_index=i; break; }
    }
    if(free_ino_index==(uint64_t)-1){ fprintf(stderr,"Error: no free inode\n"); free(img); return 1; }
    uint32_t new_ino_no = (uint32_t)(free_ino_index + 1); 

    
    uint64_t need_blocks = (fsz_in<=0)?0:((uint64_t)(fsz_in-1)/BS + 1);
    if(need_blocks>DIRECT_MAX){ fprintf(stderr,"Warning: file too large for MiniVSFS (max 12 blocks)\n"); free(img); return 1; }

    uint32_t direct[DIRECT_MAX]={0};
    uint64_t got=0;
    for(uint64_t bi=0; bi<sb->data_region_blocks && got<need_blocks; ++bi){
        uint8_t b = data_bitmap[bi>>3u];
        if(((b>>(bi&7u))&1u)==0u){
            // allocate
            data_bitmap[bi>>3u] = b | (1u<<(bi&7u));
            direct[got++] = (uint32_t)(sb->data_region_start + bi);
        }
    }
    if(got<need_blocks){ fprintf(stderr,"Error: not enough data blocks\n"); free(img); return 1; }

   
    for(uint64_t i=0;i<need_blocks;i++){
        size_t to_read = (size_t)((i+1)*BS <= (uint64_t)fsz_in ? BS : (uint64_t)fsz_in - i*BS);
        if(to_read>0){
            if(fread(data_region + (direct[i]-sb->data_region_start)*BS, 1, to_read, fadd)!=to_read){
                fprintf(stderr,"Error: reading input file\n"); free(img); fclose(fadd); return 1;
            }
        }
        if(to_read<BS){
            memset(data_region + (direct[i]-sb->data_region_start)*BS + to_read, 0, BS - to_read);
        }
    }
    fclose(fadd);

   
    inode_bitmap[free_ino_index>>3u] |= (1u<<(free_ino_index&7u));

    
    inode_t ino; memset(&ino,0,sizeof(ino));
    ino.mode=0100000; 
    ino.links=1;
    ino.size_bytes=fsz_in;
    time_t now=time(NULL);
    ino.atime=ino.mtime=ino.ctime=now;
    for(int i=0;i<DIRECT_MAX;i++) ino.direct[i]=direct[i];
    ino.proj_id=2; // group id
    inode_crc_finalize(&ino);

    uint64_t inodes_per_block=BS/INODE_SIZE;
    uint64_t blk_offset=free_ino_index/inodes_per_block;
    uint64_t slot=free_ino_index%inodes_per_block;
    memcpy(inode_table+blk_offset*BS+slot*INODE_SIZE,&ino,sizeof(ino));

    
    inode_t root; memcpy(&root,inode_table,sizeof(root));
    uint32_t rootblk=root.direct[0];
    dirent64_t* dirents=(dirent64_t*)(img+rootblk*BS);
    size_t slots=BS/sizeof(dirent64_t);

   
    for (size_t i = 0; i < slots; i++) {
        if (dirents[i].inode_no != 0) {
            
            if (strncmp(dirents[i].name, fname, sizeof(dirents[i].name)) == 0) {
                fprintf(stderr, "Error: File '%s' already exists in the filesystem.\n", fname);
                free(img);
                return 1;
            }
        }
    }

    size_t free_slot=SIZE_MAX;
    for(size_t i=0;i<slots;i++){
        if(dirents[i].inode_no==0){ free_slot=i; break; }
    }
    if(free_slot==SIZE_MAX){
        fprintf(stderr,"Error: root dir full\n");
        free(img); return 1;
    }
    dirent64_t de; memset(&de,0,sizeof(de));
    de.inode_no=new_ino_no;
    de.type=1;
    strcpy(de.name,fname);
    dirent_checksum_finalize(&de);
    dirents[free_slot]=de;

    root.links+=1;
    inode_crc_finalize(&root);
    memcpy(inode_table,&root,sizeof(root));

    
    superblock_crc_finalize(sb);

    
    FILE* fo=fopen(out_path,"wb");
    fwrite(img,1,img_bytes,fo);
    fclose(fo);
    free(img);

    printf("Added '%s' (%ld bytes) as inode #%u using %llu block(s). Output: %s\n",
           fname, fsz_in, new_ino_no, (unsigned long long)need_blocks, out_path);

    return 0;
}

