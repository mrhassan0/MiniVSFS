# MiniVSFS
MiniVSFS, based on VSFS, is fairly simple – a block-based file system structure with a
superblock, inode and data bitmaps, inode tables, and data blocks. Compared to
the regular VSFS, MiniVSFS cuts a few corners:
● Indirect pointer mechanism is not implemented
● Only supported directory is the root (/) directory
● Only one block each for the inode and data bitmap
● Limited size and inode counts


MKFS_BUILDER
It performs the following tasks in order:
1. Parse the command line inputs
2. Create the file system according to the provided specifications
3. Save the file system as a binary file with the name specified by the --image flag

   
MKFS_ADDER
It should performs the following tasks in order:
1. Parse the command line inputs
2. Open the input image as a binary file
3. Search the file in the present working directory, and add the file to the file
system.
4. Update the file system binary image
