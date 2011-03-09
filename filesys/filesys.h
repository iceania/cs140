#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include "filesys/off_t.h"

/* Sectors of system file inodes. */
#define ZERO_SECTOR 0	        /* A dummy sector that all inodes point to
								   when the sector they point to has not been
								   written to yet */
#define FREE_MAP_SECTOR 1       /* Free map file inode sector. */
#define ROOT_DIR_SECTOR 2       /* Root directory file inode sector. */

/* Block device that contains the file system. */
struct block *fs_device;

void filesys_init (bool format);
void filesys_done (void);
bool filesys_create (const char *path, off_t initial_size);
struct file *filesys_open (const char *path);
bool filesys_remove (const char *path);

#endif /* filesys/filesys.h */
