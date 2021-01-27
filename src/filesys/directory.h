#ifndef FILESYS_DIRECTORY_H
#define FILESYS_DIRECTORY_H

#include <stdbool.h>
#include <stddef.h>
#include "devices/block.h"

/* Maximum length of a file name component.
   This is the traditional UNIX maximum length.
   After directories are implemented, this maximum length may be
   retained, but much longer full path names must be allowed. */
#define NAME_MAX 14
#define DIR_ENTRY_MAX 20

struct inode;

/* Opening and closing directories. */
bool dir_create (block_sector_t sector, size_t entry_cnt, block_sector_t dir_sector);
struct dir *dir_open (struct inode *);
struct dir *dir_open_root (void);
struct dir *dir_reopen (struct dir *);
void dir_close (struct dir *);
struct inode *dir_get_inode (struct dir *);

/* Reading and writing. */
bool dir_lookup (const struct dir *, const char *name, struct inode **);
bool dir_add (struct dir *, const char *name, block_sector_t);
bool dir_remove (struct dir *, const char *name);
bool dir_readdir (struct dir *, char name[NAME_MAX + 1]);

bool make_dir (const char *path);
bool change_dir (const char *path);
bool open_path_dir (struct dir *dir, const char *path, struct dir **dest_);
bool get_dir_name (char *path, char name[NAME_MAX + 1]);
bool dir_get_filename (struct inode *inode, char name[NAME_MAX + 1]);
int dir_get_sector (struct dir *dir);
int dir_get_dir_sector (struct dir *dir);

#endif /* filesys/directory.h */
