// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_minivsfs.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#define BS 4096u // block size
#define INODE_SIZE 128u
#define ROOT_INO 1u

uint64_t g_random_seed = 0; // This should be replaced by seed value from the CLI.

// below contains some basic structures you need for your project
// you are free to create more structures as you require

#pragma pack(push, 1)
typedef struct
{
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
    // CREATE YOUR SUPERBLOCK HERE
    // ADD ALL FIELDS AS PROVIDED BY THE SPECIFICATION

    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint32_t checksum; // crc32(superblock[0..4091])
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push, 1)
typedef struct
{
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
    // CREATE YOUR INODE HERE
    // IF CREATED CORRECTLY, THE STATIC_ASSERT ERROR SHOULD BE GONE

    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint64_t inode_crc; // low 4 bytes store crc32 of bytes [0..119]; high 4 bytes 0

} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t) == INODE_SIZE, "inode size mismatch");

#pragma pack(push, 1)
typedef struct
{
    uint32_t inode_no; 
    uint8_t type;      
    char name[58];     
    // CREATE YOUR DIRECTORY ENTRY STRUCTURE HERE
    // IF CREATED CORRECTLY, THE STATIC_ASSERT ERROR SHOULD BE GONE

    uint8_t checksum; // XOR of bytes 0..62
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t) == 64, "dirent size mismatch");

// ==========================DO NOT CHANGE THIS PORTION=========================
// These functions are there for your help. You should refer to the specifications to see how you can use them.
// ====================================CRC32====================================
uint32_t CRC32_TAB[256];
void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        CRC32_TAB[i] = c;
    }
}
uint32_t crc32(const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = CRC32_TAB[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
// ====================================CRC32====================================

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
static uint32_t superblock_crc_finalize(superblock_t *sb)
{
    sb->checksum = 0;
    uint32_t s = crc32((void *)sb, BS - 4);
    sb->checksum = s;
    return s;
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void inode_crc_finalize(inode_t *ino)
{
    uint8_t tmp[INODE_SIZE];
    memcpy(tmp, ino, INODE_SIZE);
    // zero crc area before computing
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c; // low 4 bytes carry the crc
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void dirent_checksum_finalize(dirent64_t *de)
{
    const uint8_t *p = (const uint8_t *)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++)
        x ^= p[i]; // covers ino(4) + type(1) + name(58)
    de->checksum = x;
}

static inline void bitmap_set(uint8_t *bm, uint64_t idx)
{
    
    bm[idx >> 3] |= (uint8_t)(1u << (idx & 7u));
}
static inline void bitmap_clear(uint8_t *bm, uint64_t idx)
{
    bm[idx >> 3] &= (uint8_t)~(1u << (idx & 7u));
}
static inline int bitmap_test(const uint8_t *bm, uint64_t idx)
{
    return (bm[idx >> 3] >> (idx & 7u)) & 1u;
}

void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --image out.img --size-kib <180..4096> --inodes <128..512>\n"
            "Example: %s --image out.img --size-kib 1024 --inodes 128\n",
            prog, prog);
}

int main(int argc, char **argv)
{
    crc32_init();
  
    const char *image_path = NULL;
    uint64_t size_kib = 0;
    uint64_t inode_count = 0;


    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--image") == 0)
        {
            if (i + 1 >= argc)
            {
                print_usage(argv[0]);
                return 1;
            }
            image_path = argv[i + 1];
            i++;
        }
        else if (strcmp(argv[i], "--size-kib") == 0)
        {
            if (i + 1 >= argc)
            {
                print_usage(argv[0]);
                return 1;
            }
            size_kib = (uint64_t)strtoull(argv[i + 1], NULL, 10);
            i++;
        }
        else if (strcmp(argv[i], "--inodes") == 0)
        {
            if (i + 1 >= argc)
            {
                print_usage(argv[0]);
                return 1;
            }
            inode_count = (uint64_t)strtoull(argv[i + 1], NULL, 10);
            i++;
        }
        else
        {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    
    if (!image_path || size_kib == 0 || inode_count == 0)
    {
        fprintf(stderr, "Missing required arguments.\n");
        print_usage(argv[0]);
        return 1;
    }

   
    if (size_kib < 180 || size_kib > 4096 || (size_kib % 4u) != 0)
    {
        fprintf(stderr, "Error: --size-kib must be between 180 and 4096 and a multiple of 4.\n");
        return 1;
    }
    if (inode_count < 128 || inode_count > 512)
    {
        fprintf(stderr, "Error: --inodes must be in range 128..512.\n");
        return 1;
    }

  
    uint64_t total_blocks = (size_kib * 1024ull) / BS; 
    uint64_t inodes_per_block = BS / INODE_SIZE;       
    uint64_t inode_table_blocks = (inode_count + inodes_per_block - 1) / inodes_per_block;
    uint64_t inode_bitmap_start = 1;  
    uint64_t inode_bitmap_blocks = 1; 
    uint64_t data_bitmap_start = 2;   
    uint64_t data_bitmap_blocks = 1;  
    uint64_t inode_table_start = 3;   
    uint64_t data_region_start = inode_table_start + inode_table_blocks;
    if (data_region_start >= total_blocks)
    {
        fprintf(stderr, "Error: Not enough space for data region with given parameters.\n");
        return 1;
    }
    uint64_t data_region_blocks = total_blocks - data_region_start;

  
    printf("Image: %s\n", image_path);
    printf("Size: %" PRIu64 " KiB -> %" PRIu64 " blocks (BS=%u)\n", size_kib, total_blocks, BS);
    printf("Inodes: %" PRIu64 ", inode table blocks: %" PRIu64 "\n", inode_count, inode_table_blocks);
    printf("inode bitmap at block %" PRIu64 ", data bitmap at block %" PRIu64 "\n",
           inode_bitmap_start, data_bitmap_start);
    printf("inode table starts at block %" PRIu64 " (%" PRIu64 " blocks)\n",
           inode_table_start, inode_table_blocks);
    printf("data region starts at block %" PRIu64 " (%" PRIu64 " blocks)\n",
           data_region_start, data_region_blocks);

   
    FILE *f = fopen(image_path, "wb");
    if (!f)
    {
        perror("fopen");
        return 1;
    }

    
    uint8_t *block = calloc(1, BS);
    if (!block)
    {
        perror("calloc");
        fclose(f);
        return 1;
    }

   
    memset(block, 0, BS);
    superblock_t *sb = (superblock_t *)block; 

    sb->magic = 0x4D565346u; 
    sb->version = 1u;
    sb->block_size = BS;
    sb->total_blocks = total_blocks;
    sb->inode_count = inode_count;
    sb->inode_bitmap_start = inode_bitmap_start;
    sb->inode_bitmap_blocks = inode_bitmap_blocks;
    sb->data_bitmap_start = data_bitmap_start;
    sb->data_bitmap_blocks = data_bitmap_blocks;
    sb->inode_table_start = inode_table_start;
    sb->inode_table_blocks = inode_table_blocks;
    sb->data_region_start = data_region_start;
    sb->data_region_blocks = data_region_blocks;
    sb->root_inode = ROOT_INO;
    sb->mtime_epoch = (uint64_t)time(NULL);
    sb->flags = 0;
   
    superblock_crc_finalize(sb); 

   
    if (fwrite(block, 1, BS, f) != BS)
    {
        perror("fwrite");
        free(block);
        fclose(f);
        return 1;
    }

   
    memset(block, 0, BS);
    
    bitmap_set(block, 0); 
    if (fwrite(block, 1, BS, f) != BS)
    {
        perror("fwrite");
        free(block);
        fclose(f);
        return 1;
    }

    
    memset(block, 0, BS);
    
    bitmap_set(block, 0); 
    if (fwrite(block, 1, BS, f) != BS)
    {
        perror("fwrite");
        free(block);
        fclose(f);
        return 1;
    }

    
   
    for (uint64_t blk = 0; blk < inode_table_blocks; ++blk)
    {
        memset(block, 0, BS);
        for (uint64_t slot = 0; slot < inodes_per_block; ++slot)
        {
            uint64_t ino_index = blk * inodes_per_block + slot; 
            if (ino_index >= inode_count)
                break;

            inode_t ino;
            memset(&ino, 0, sizeof(ino));

            if (ino_index == 0)
            {
                
                ino.mode = (uint16_t)0040000; 
                ino.links = 2;                
                ino.uid = 0;
                ino.gid = 0;
                ino.size_bytes = (uint64_t)BS; 
                time_t now = time(NULL);
                ino.atime = (uint64_t)now;
                ino.mtime = (uint64_t)now;
                ino.ctime = (uint64_t)now;
                for (int d = 0; d < 12; ++d)
                    ino.direct[d] = 0;
                ino.direct[0] = (uint32_t)data_region_start; 
                ino.reserved_0 = ino.reserved_1 = ino.reserved_2 = 0;
                ino.proj_id = 2;
                ino.uid16_gid16 = 0;
                ino.xattr_ptr = 0;
                
                inode_crc_finalize(&ino);
            }
            else
            {
                inode_crc_finalize(&ino); 
            }

         
            memcpy(block + slot * INODE_SIZE, &ino, INODE_SIZE);
        }
        
        if (fwrite(block, 1, BS, f) != BS)
        {
            perror("fwrite");
            free(block);
            fclose(f);
            return 1;
        }
    }

    
    for (uint64_t dblk = 0; dblk < data_region_blocks; ++dblk)
    {
        memset(block, 0, BS);
        if (dblk == 0)
        {
            
            dirent64_t *de = (dirent64_t *)block;
            
            memset(&de[0], 0, sizeof(dirent64_t));
            de[0].inode_no = (uint32_t)ROOT_INO;
            de[0].type = 2; 
            memset(de[0].name, 0, sizeof(de[0].name));
            de[0].name[0] = '.';
            dirent_checksum_finalize(&de[0]);

            // Entry 1: ".."
            memset(&de[1], 0, sizeof(dirent64_t));
            de[1].inode_no = (uint32_t)ROOT_INO;
            de[1].type = 2;
            memset(de[1].name, 0, sizeof(de[1].name));
            de[1].name[0] = '.';
            de[1].name[1] = '.';
            dirent_checksum_finalize(&de[1]);

            
        }
     
        if (fwrite(block, 1, BS, f) != BS)
        {
            perror("fwrite");
            free(block);
            fclose(f);
            return 1;
        }
    }

    
    fflush(f);
    free(block);
    fclose(f);

    printf("Successfully created MiniVSFS image '%s' with %" PRIu64 " blocks.\n", image_path, total_blocks);
    return 0;
    // WRITE YOUR DRIVER CODE HERE
    // PARSE YOUR CLI PARAMETERS
    // THEN CREATE YOUR FILE SYSTEM WITH A ROOT DIRECTORY
    // THEN SAVE THE DATA INSIDE THE OUTPUT IMAGE
}
