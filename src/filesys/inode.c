#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "filesys/cache.h"
#include "userprog/process.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define INODE_ENTRY_NUM (BLOCK_SECTOR_SIZE / sizeof (block_sector_t))
#define FILESYS_PARTITION_MAX (8 * 1024 * 1024)
#define FILE_METADATA_MAX (BLOCK_SECTOR_SIZE + BLOCK_SECTOR_SIZE + BLOCK_SECTOR_SIZE * 128)
#define FILE_SIZE_MAX (FILESYS_PARTITION_MAX - FILE_METADATA_MAX)

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
{
  block_sector_t sector;              /* data sector. */
  block_sector_t dir_sector;          /* Sector of directory where file is located. */
  off_t length;                       /* File size in bytes. */
  int height;                         /* 0 = direct, 1 = indirect, 2 = double */
  int is_dir;                         /* Is directory or file? */
  char name[NAME_MAX + 1];            /* file name */
  unsigned magic;                     /* Magic number. */
  uint32_t unused[118];               /* Not used. */
};

struct sector_list
{
  block_sector_t list[INODE_ENTRY_NUM];   /* indirect block */
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
{
  struct list_elem elem;              /* Element in inode list. */
  block_sector_t sector;              /* Sector number of disk location. */
  block_sector_t dir_sector;          /* Sector number of dir where file is located.  */
  int open_cnt;                       /* Number of openers. */
  bool removed;                       /* True if deleted, false otherwise. */
  bool is_dir;                        /* Is directory or file? */
  int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
};

static bool inode_grow (block_sector_t sector, struct inode_disk *inode, off_t length);

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);

  struct inode_disk *disk_inode;
  struct sector_list *s_list = NULL;

  disk_inode = malloc (sizeof *disk_inode);
  cache_read (inode->sector, -1, disk_inode, BLOCK_SECTOR_SIZE, 0);

  int length = disk_inode->length;
  int sector = disk_inode->sector;
  int height = disk_inode->height;
  
  free (disk_inode);

  /* pos 가 파일 크기 초과한 경우 */
  if (pos >= length)
    return -1;

  /* inode 구조가 direct 가 아닌 경우 */
  if (height == 1)
  {
    s_list = malloc (sizeof *s_list);
    cache_read (sector, -1, s_list, BLOCK_SECTOR_SIZE, 0);

    int index = pos / BLOCK_SECTOR_SIZE;
    sector = s_list->list[index];
  }

  else if (height == 2)
  {
    s_list = malloc (sizeof *s_list);
    cache_read (sector, -1, s_list, BLOCK_SECTOR_SIZE, 0);

    int index = pos / BLOCK_SECTOR_SIZE / INODE_ENTRY_NUM;
    sector = s_list->list[index];

    cache_read (sector, -1, s_list, BLOCK_SECTOR_SIZE, 0);
    sector = s_list->list[(pos / BLOCK_SECTOR_SIZE) % INODE_ENTRY_NUM];
  }
  free (s_list);
  return sector;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, bool is_dir, block_sector_t dir_sector)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* filesys partition 을 8MB 까지로 제한 */
  if (length > FILE_SIZE_MAX)
    return false;

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);

  if (disk_inode == NULL)
    return false;

  /* 파일 데이터를 위해 필요한 sector 개수 */
  size_t sectors = bytes_to_sectors (length);

  /* disk_inode 초기화 */
  disk_inode->dir_sector = dir_sector;
  disk_inode->length = 0;
  disk_inode->height = -1;
  disk_inode->is_dir = is_dir;
  disk_inode->magic = INODE_MAGIC;

  success = inode_grow (sector, disk_inode, length);
  free (disk_inode);

  return success;
}

/**
 * 파일의 길이에 따라 현재 mapping을 유지할 지 확장할지 정하고 캐시에 inode
 * 정보를 업데이트 한다.
 * @return {bool} 길이 성장을 성공 했는지의 여부
 * @param sector inode_disk 가 저장된 sector number
 * @param inode inode_disk 포인터
 * @param length 증가시키려는 길이
 */
static bool
inode_grow (block_sector_t sector, struct inode_disk *inode, off_t length)
{
  /* 새로 만들어야 하는 섹터 개수 */
  int sector_grow = bytes_to_sectors (length) - bytes_to_sectors (inode->length);
  
  if (length < inode->length)
    return false;
  
  /* 새로 섹터를 만들 필요 없는 경우, length 값만 변경해준다 */
  if (sector_grow == 0)
  {
    inode->length = length;
    cache_write (sector, -1, inode, BLOCK_SECTOR_SIZE, 0);
    return true;
  }

  static char zeros[BLOCK_SECTOR_SIZE];

  /* 파일 사이즈가 0 이었을 경우, 우선 1 섹터를 할당 */
  if (inode->height == -1)
  {
    free_map_allocate (1, &inode->sector);
    cache_write (inode->sector, -1, zeros, BLOCK_SECTOR_SIZE, 0);
    inode->height = 0;
    inode->length = BLOCK_SECTOR_SIZE;
    sector_grow--;
  }

  /* direct inode 에서 indirect inode 로 변경해야 하는 경우 */
  if (inode->height == 0 && sector_grow > 0)
  {
    struct sector_list *s_list = calloc (1, sizeof *s_list);

    s_list->list[0] = inode->sector;       // 기존 있던 파일 한 섹터를 s_list 에 넣어놓기
    free_map_allocate (1, &inode->sector); // s_list 를 위한 섹터

    int child_left = INODE_ENTRY_NUM - 1; // s_list 에 남은 공간
    int min_left = child_left < sector_grow ? child_left : sector_grow;

    /* s_list 에 (넣을 수 있을 만큼) 섹터 인덱스를 넣고
       파일 데이터는 0 으로 초기화 */
    for (int i=0; i<min_left; i++)
    {
      free_map_allocate (1, &s_list->list[1 + i]);
      cache_write (s_list->list[1 + i], -1, zeros, BLOCK_SECTOR_SIZE, 0);
      inode->length += BLOCK_SECTOR_SIZE;
    }

    cache_write (inode->sector, -1, s_list, BLOCK_SECTOR_SIZE, 0);
    free (s_list);

    inode->height = 1;
    sector_grow -= min_left;
  }

  /* indirect inode 에서 새로운 섹터 추가하는 경우 */
  if (inode->height == 1 && sector_grow > 0)
  {
    struct sector_list *s_list = calloc (1, sizeof *s_list);
    cache_read (inode->sector, -1, s_list, BLOCK_SECTOR_SIZE, 0);

    int sectors = bytes_to_sectors (inode->length);
    int child_left = INODE_ENTRY_NUM - sectors; // s_list 에 남은 공간
    int min_left = child_left < sector_grow ? child_left : sector_grow;
    for (int i=0; i<min_left; i++)
    {
      free_map_allocate (1, &s_list->list[sectors + i]);
      cache_write (s_list->list[sectors + i], -1, zeros, BLOCK_SECTOR_SIZE, 0);
      inode->length += BLOCK_SECTOR_SIZE;
    }

    cache_write (inode->sector, -1, s_list, BLOCK_SECTOR_SIZE, 0);
    free (s_list);
    
    sector_grow -= min_left;
  }

  /* indirect inode 에서 doubly indirect inode 로 변경해야 하는 경우 */
  if (inode->height == 1 && sector_grow > 0)
  {
    struct sector_list *s_list_parent = calloc (1, sizeof *s_list_parent);
    struct sector_list *s_list_child;
  
    s_list_parent->list[0] = inode->sector;  // 기존 있던 sector_list 의 섹터를 s_list 에 넣어놓기
    free_map_allocate (1, &inode->sector);   // 부모 s_list 를 위한 섹터

    int child_idx = 1;
    while (sector_grow > 0)
    {
      s_list_child = calloc (1, sizeof *s_list_child);
      free_map_allocate (1, &s_list_parent->list[child_idx]);

      int child_left = INODE_ENTRY_NUM; // s_list 에 남은 공간
      int min_left = child_left < sector_grow ? child_left : sector_grow;

      /* s_list 에 (넣을 수 있을 만큼) 섹터 인덱스를 넣고
        파일 데이터는 0 으로 초기화 */
      for (int i=0; i<min_left; i++)
      {
        free_map_allocate (1, &s_list_child->list[i]);
        cache_write (s_list_child->list[i], -1, zeros, BLOCK_SECTOR_SIZE, 0);
        inode->length += BLOCK_SECTOR_SIZE;
      }

      cache_write (s_list_parent->list[child_idx], -1, s_list_child, BLOCK_SECTOR_SIZE, 0);
      free (s_list_child);

      sector_grow -= min_left;
      child_idx ++;
    }
    cache_write (inode->sector, -1, s_list_parent, BLOCK_SECTOR_SIZE, 0);
    free (s_list_parent);

    inode->height = 2;
  }

  /* doubly indirect inode 에서 새로운 섹터를 추가하는 경우 */
  if (inode->height == 2 && sector_grow > 0)
  {
    struct sector_list *s_list_parent = calloc (1, sizeof *s_list_parent);
    struct sector_list *s_list_child = calloc (1, sizeof *s_list_child);
  
    cache_read (inode->sector, -1, s_list_parent, BLOCK_SECTOR_SIZE, 0);

    int sectors = bytes_to_sectors (inode->length);
    int child_idx = (sectors - 1) / INODE_ENTRY_NUM;

    cache_read (s_list_parent->list[child_idx], -1, s_list_child, BLOCK_SECTOR_SIZE, 0);

    int child_sectors = (sectors-1) % INODE_ENTRY_NUM + 1;
    int child_left = INODE_ENTRY_NUM - child_sectors; // s_list 에 남은 공간
    int min_left = child_left < sector_grow ? child_left : sector_grow;

    for (int i=0; i<min_left; i++)
    {
      free_map_allocate (1, &s_list_child->list[child_sectors + i]);
      cache_write (s_list_child->list[child_sectors + i], -1, zeros, BLOCK_SECTOR_SIZE, 0);
      inode->length += BLOCK_SECTOR_SIZE;
    }
    cache_write (s_list_parent->list[child_idx], -1, s_list_child, BLOCK_SECTOR_SIZE, 0);
    free (s_list_child);
    
    sector_grow -= min_left;
    child_idx++;

    while (sector_grow > 0)
    {
      s_list_child = calloc (1, sizeof *s_list_child);
      free_map_allocate (1, &s_list_parent->list[child_idx]);

      child_left = INODE_ENTRY_NUM; // s_list 에 남은 공간
      min_left = child_left < sector_grow ? child_left : sector_grow;

      /* s_list 에 (넣을 수 있을 만큼) 섹터 인덱스를 넣고
        파일 데이터는 0 으로 초기화 */
      for (int i=0; i<min_left; i++)
      {
        free_map_allocate (1, &s_list_child->list[i]);
        cache_write (s_list_child->list[i], -1, zeros, BLOCK_SECTOR_SIZE, 0);
        inode->length += BLOCK_SECTOR_SIZE;
      }
      cache_write (s_list_parent->list[child_idx], -1, s_list_child, BLOCK_SECTOR_SIZE, 0);
      free (s_list_child);

      sector_grow -= min_left;
      child_idx++;
    }
    cache_write (inode->sector, -1, s_list_parent, BLOCK_SECTOR_SIZE, 0);
    free (s_list_parent);
  }

  inode->length = length;
  cache_write (sector, -1, inode, BLOCK_SECTOR_SIZE, 0);

  return true;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  if (sector < 0)
    return NULL;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e))
  {
    inode = list_entry (e, struct inode, elem);
    if (inode->sector == sector)
    {
      inode_reopen (inode);
      return inode;
    }
  }
  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;

  struct inode_disk *disk_inode = malloc (sizeof *disk_inode);
  cache_read (sector, -1, disk_inode, BLOCK_SECTOR_SIZE, 0);
  inode->dir_sector = disk_inode->dir_sector;
  inode->is_dir = disk_inode->is_dir;
  free (disk_inode);

  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
  {
    /* Remove from inode list and release lock. */
    list_remove (&inode->elem);

    /* Deallocate blocks if removed. */
    if (inode->removed)
    {
      struct inode_disk *disk_inode;
      disk_inode = malloc (sizeof *disk_inode);
      cache_read (inode->sector, -1, disk_inode, BLOCK_SECTOR_SIZE, 0);

      free_map_release (inode->sector, 1);

      if (disk_inode->height == 2)
      {
        struct sector_list *s_list_parent = calloc (1, sizeof *s_list_parent);
        struct sector_list *s_list_child;
        int sectors = bytes_to_sectors (disk_inode->length);
        
        cache_read (disk_inode->sector, -1, s_list_parent, BLOCK_SECTOR_SIZE, 0);
        
        /* s_list 에 (넣을 수 있을 만큼) 섹터 인덱스를 넣고
          파일 데이터는 0 으로 초기화 */
        for (int i=0; i<INODE_ENTRY_NUM && sectors>0; i++)
        {
          s_list_child = calloc (1, sizeof *s_list_child);
          cache_read (s_list_parent->list[i], -1, s_list_child, BLOCK_SECTOR_SIZE, 0);

          for (int j=0; j<INODE_ENTRY_NUM && sectors>0; j++)
          {
            free_map_release (s_list_child->list[j], 1);
            sectors--;
          }
          free_map_release (s_list_child->list[i], 1);
          free (s_list_child);
        }
        free (s_list_parent);
      }

      else if (disk_inode->height == 1)
      {
        struct sector_list *s_list = calloc (1, sizeof *s_list);
        int sectors = bytes_to_sectors (disk_inode->length);

        cache_read (disk_inode->sector, -1, s_list, BLOCK_SECTOR_SIZE, 0);

        for (int i=0; i<INODE_ENTRY_NUM && sectors>0; i++)
        {
          free_map_release (s_list->list[i], 1);
          sectors--;
        }
      }
      else if (disk_inode->height == 0)
        free_map_release (disk_inode->sector, 1);

      free (disk_inode);
    }
    free (inode);
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  while (size > 0)
  {
    /* Disk sector to read, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector (inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length (inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    /* 파일의 다음 sector (for read-ahead) */
    int next_sector_idx = byte_to_sector (inode, offset + BLOCK_SECTOR_SIZE);
    /* cache 에서 버퍼로 데이터 복사 */
    cache_read (sector_idx, next_sector_idx, buffer + bytes_read, chunk_size, sector_ofs);
    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
    return 0;

  struct inode_disk *disk_inode;
  disk_inode = malloc (sizeof *disk_inode);

  cache_read (inode->sector, -1, disk_inode, BLOCK_SECTOR_SIZE, 0);

  /* 쓰려는 곳이 EOF 이후인 경우 */
  if (offset + size > disk_inode->length)
    inode_grow (inode->sector, disk_inode, offset + size);

  free (disk_inode);

  while (size > 0)
  {
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector (inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length (inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually write into this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    /* 파일의 다음 sector (for read-ahead) */
    int next_sector_idx = byte_to_sector (inode, offset + BLOCK_SECTOR_SIZE);
    /* 버퍼에서 cache 로 데이터 복사 */
    cache_write (sector_idx, next_sector_idx, buffer + bytes_written, chunk_size, sector_ofs);

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  struct inode_disk *disk_inode;
  off_t result;

  disk_inode = malloc (sizeof *disk_inode);
  cache_read (inode->sector, -1, disk_inode, BLOCK_SECTOR_SIZE, 0);
  result = disk_inode->length;

  free (disk_inode);

  return result;
}

/* 인자로 주어진 inode가 directory를 위한 inode인지 알려주는 함수 */
bool
inode_is_dir (struct inode *inode)
{
  return inode->is_dir;
}

/* INODE 가 제거되었는지 여부를 반환 */
bool
inode_is_removed (struct inode *inode)
{
  return inode->removed;
}

/* INODE 의 DIR_SECTOR 를 반환 */
int
inode_get_dir_sector (struct inode *inode)
{
  return inode->dir_sector;
}