#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"

/* A directory. */
struct dir 
  {
    struct inode *inode;                /* Backing store. */
    off_t pos;                          /* Current position. */
  };

/* A single directory entry. */
struct dir_entry 
  {
    block_sector_t inode_sector;        /* Sector number of header. */
    char name[NAME_MAX + 1];            /* Null terminated file name. */
    bool in_use;                        /* In use or free? */
  };

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (block_sector_t sector, size_t entry_cnt, block_sector_t dir_sector)
{
  return inode_create (sector, entry_cnt * sizeof (struct dir_entry), true, dir_sector);
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
  {
    dir->inode = inode;
    dir->pos = 0;
    return dir;
  }
  else
  {
    inode_close (inode);
    free (dir);
    return NULL;
  }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
  {
    inode_close (dir->inode);
    free (dir);
  }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);
  ASSERT (strlen (name) <= NAME_MAX);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e)
    if (e.in_use && !strcmp (name, e.name)) 
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        return true;
      }
  return false;
}

/** 
 * PATH 에서 가장 끝의 폴더 이름을 NAME 에 복사하고 PATH 에서 지운다.
 * @param path (in/out) 경로. 함수가 끝나면 오른쪽 끝 파일 이름이 제거됨
 * @param name (output) PATH 의 오른쪽 끝 파일 이름 
 */
bool
get_dir_name (char *path, char name[NAME_MAX + 1])
{
  ASSERT (strlen (path) > 0);

  int len = 0;
  char *last = path + strlen (path) - 1;  /* pointer to last character */

  /* 오른쪽 끝의 / 들을 모두 지우기 */
  while (*last == '/')
  {
    /* 문자열에 / 밖에 없는 경우 */
    if (path == last)
    {
      name[0] = '\0';
      return true;
    }
    last--;
  }
  *(last + 1) = '\0';

  while (*last != '/')
  {
    /* path 에 name 만 입력된 경우 */
    if (path == last)
    {
      last--;
      len++;
      break;
    }
    last--;
    len++;
  }
  if (len > NAME_MAX || len <= 0)
    return false;

  strlcpy (name, ++last, len + 1);
  *last = '\0';
  return true;
}

/**
 * DIR 에서 PATH 대로 directory 를 열고, DEST 로 리턴한다.
 * 오픈한 directory 의 섹터와 이름을 DIR_SECTOR_, NAME_ 에 리턴한다.
 * @return {bool} 
 * @param dir 현재 디렉토리
 * @param path 이동할 디렉토리의 path
 * @param dest_ (output) 이동한 dir 포인터
 */
bool
open_path_dir (struct dir *dir, const char *path, struct dir **dest_)
{
  ASSERT (path != NULL);

  struct dir *dest = NULL;          /* destination directory */
  bool absolute = path[0] == '/';   /* Is absolute or relative ? */
  char *name, *save_ptr;
  struct dir_entry e;
  struct inode *inode = NULL;

  if (*path == NULL)
  {
    /* 현재 dir 가 제거되어 있는 경우 */
    if (inode_is_removed (dir->inode))
      return false;
    dest = dir_reopen (dir);
    goto done;
  }

  /* 절대/상대 경로에 맞게 상위 폴더를 우선 오픈한다 */
  if (absolute)
    dest = dir_open_root ();
  else
  {
    /* 현재 dir 가 제거되어 있는 경우 */
    if (inode_is_removed (dir->inode))
      return false;
    dest = dir_reopen (dir);
  }

  for (name = strtok_r (path, "/", &save_ptr); name != NULL;
       name = strtok_r (NULL, "/", &save_ptr))
  {
    /* 현재 dir */
    if (strcmp (name, ".") == 0)
      continue;

    /* 상위 dir */
    if (strcmp (name, "..") == 0)
    {
      int dir_sector = inode_get_dir_sector (dest->inode);
      if (dir_sector < 0)
        /* root 에서 ".." 하는 경우 */
        continue;
      else
        inode = inode_open (inode_get_dir_sector (dest->inode));
    }
    /* dir 에 name 존재하지 않는 경우 */
    else if (!lookup (dest, name, &e, NULL))
    {
      dir_close (dest);
      return false;
    }
    /* dir 에 name 찾은 경우 */
    else
    {
      inode = inode_open (e.inode_sector);

      /* 존재하는 이름이 file 이거나 제거된 dir 인 경우 */
      if (!inode_is_dir (inode) || inode_is_removed (inode))
      {
        dir_close (dest);
        return false;
      }
    }

    /* 다음 폴더 오픈 */
    dir_close (dest);
    dest = dir_open (inode);
  }

done:
  *dest_ = dest;
  return true;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *path, struct inode **inode) 
{
  struct dir_entry e;
  int len;                  /* length of PATH */
  char *path_;              /* copy of PATH */
  char name[NAME_MAX + 1];  /* name of new dir */
  struct dir *dest = NULL;  /* destination directory */

  ASSERT (dir != NULL);
  ASSERT (path != NULL);

  /* Check PATH */
  if (*path == NULL)
    return false;

  *inode = NULL;

  /* strtok_r 함수는 원본 데이터를 수정하기 때문에, 복사본을 만들어 사용하자. */
  len = strlen (path);
  path_ = malloc (len + 1);
  strlcpy (path_, path, len + 1);
  
  /* 경로(path_)와 폴더 이름(name)을 나눈다 */
  if (!get_dir_name (path_, name))
    goto done;

  /* name 이 NULL, 즉 path == '/' 인 경우이다 */
  if (*name == '\0')
    *inode = inode_open (ROOT_DIR_SECTOR);

  /* name 이 . 또는 .. 인 경우, PATH 원래의 값으로 dir 을 오픈한다 */
  else if (strcmp (name, ".") == 0 || strcmp (name, "..") == 0)
  {
    if (!open_path_dir (dir, path, &dest))
      goto done;
    *inode = inode_reopen (dest->inode);
  }

  /* path 와 name 을 따로 오픈한다 */
  else
  {
    if (!open_path_dir (dir, path_, &dest))
      goto done;
    
    if (lookup (dest, name, &e, NULL))
      *inode = inode_open (e.inode_sector);
    else
      *inode = NULL;
  }
done:
  dir_close (dest);
  free (path_);
  return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name, block_sector_t inode_sector)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Check PATH */
  if (*name == '\0')
    return false;

  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.
     
     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

done:
  return success;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *path) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;
  int len;                    /* length of PATH */
  char *path_;                /* copy of PATH */
  char name[NAME_MAX + 1];    /* name of new dir */
  struct dir *dest = NULL;    /* destination directory */

  ASSERT (dir != NULL);
  ASSERT (path != NULL);

  /* Check PATH */
  if (*path == '\0')
    return false;

  /* strtok_r 함수는 원본 데이터를 수정하기 때문에, 복사본을 만들어 사용하자. */
  len = strlen (path);
  path_ = malloc (len + 1);
  strlcpy (path_, path, len + 1);
  
  /* 경로(path_)와 폴더 이름(name)을 나눈다 */
  if (!get_dir_name (path_, name))
    goto done;

  /* 폴더를 추가할 폴더를 오픈한다 */
  if (!open_path_dir (dir, path_, &dest))
    goto done;

  /* Find directory entry. */
  if (!lookup (dest, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;

  /* 지우려는 파일이 directory 인 경우, entry 가 하나라도 있으면 지우지 않는다 */
  if (inode_is_dir (inode))
  {
    struct dir_entry e_;
    for (int ofs_ = 0; inode_read_at (inode, &e_, sizeof e_, ofs_) == sizeof e_;
        ofs_ += sizeof e_)
      if (e_.in_use)
        goto done;
  }

  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dest->inode, &e, sizeof e, ofs) != sizeof e)
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

done:
  inode_close (inode);
  dir_close (dest);
  free (path_);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;

  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
  {
    dir->pos += sizeof e;
    if (e.in_use)
    {
      char tmp[NAME_MAX + 1];
      strlcpy (name, e.name, NAME_MAX + 1);
      return true;
    }
  }
  return false;
}

bool
make_dir (const char *path)
{
  ASSERT (path != NULL);

  if (*path == NULL)
    return false;

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
                  && dir_create (inode_sector, DIR_ENTRY_MAX, dir_get_sector (dest))
                  && dir_add (dest, name, inode_sector));
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);

  dir_close (dest);
  free (path_);
  return success;
}

bool
change_dir (const char *path)
{
  struct dir *cur = NULL;   /* current directory */
  struct dir *dest = NULL;  /* destination directory */

  ASSERT (path != NULL);

  /* Check PATH */
  if (*path == NULL)
    return false;

  /* PATH 폴더를 오픈한다 */
  cur = thread_current ()->cur_dir;
  if (cur == NULL)
    cur = thread_current ()->cur_dir = dir_open_root ();

  if (!open_path_dir (cur, path, &dest))
    return false;

  dir_close (cur);
  thread_current ()->cur_dir = dest;

  return true;
}

int
dir_get_sector (struct dir *dir)
{
  return inode_get_inumber (dir->inode);
}

int
dir_get_dir_sector (struct dir *dir)
{
  return inode_get_dir_sector (dir->inode);
}

bool
dir_get_filename (struct inode *inode, char name[NAME_MAX + 1])
{
  struct inode *dir_inode = NULL;
  struct dir_entry e;
  int ofs = 0;
  int sector = inode_get_inumber (inode);
  int dir_sector = inode_get_dir_sector (inode);
  bool success = false;

  if (dir_sector != -1)
  {
    dir_inode = inode_open (dir_sector);

    for (ofs = 0; inode_read_at (dir_inode, &e, sizeof e, ofs) == sizeof e;
         ofs += sizeof e)
      if (e.in_use && e.inode_sector == sector)
      {
        if (name != NULL)
          strlcpy (name, e.name, NAME_MAX + 1);
        success = true;
        break;
      }
  }
  inode_close (dir_inode);
  return success;
}