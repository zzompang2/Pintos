#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "filesys/cache.h"

/* Partition that contains the file system. */
struct block *fs_device;
struct semaphore open_sema;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  cache_init ();
  free_map_init ();
  sema_init (&open_sema, 1);

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  all_cache_flush ();
  read_write_thread_exit ();
  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *path, off_t initial_size) 
{
  block_sector_t inode_sector = 0;
  struct dir *dir = thread_current ()->cur_dir;
  if (dir == NULL)
    dir = thread_current ()->cur_dir = dir_open_root ();

  int len;                  /* length of PATH */
  char *path_;              /* copy of PATH */
  char name[NAME_MAX + 1];  /* name of new dir */
  struct dir *dest = NULL;  /* destination directory */

  len = strlen (path);
  path_ = malloc (len + 1);
  strlcpy (path_, path, len + 1);

  bool success = (dir != NULL
                  && get_dir_name (path_, name)
                  && open_path_dir (dir, path_, &dest)
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, false, dir_get_sector (dest))
                  && dir_add (dest, name, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);

  dir_close (dest);
  free (path_);
  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct inode *
filesys_open (const char *path, char *name)
{
  sema_down (&open_sema);
  struct dir *dir = thread_current ()->cur_dir;
  if (dir == NULL)
    dir = thread_current ()->cur_dir = dir_open_root ();

  struct inode *inode = NULL;

  if (dir != NULL && dir_lookup (dir, path, &inode))
    dir_get_filename (inode, name);

  sema_up (&open_sema);
  return inode;
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *path) 
{
  struct dir *dir = thread_current ()->cur_dir;
  if (dir == NULL)
    dir = thread_current ()->cur_dir = dir_open_root ();

  return dir != NULL && dir_remove (dir, path);
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, DIR_ENTRY_MAX, -1))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
