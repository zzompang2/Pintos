#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "vm/page.h"
#include "threads/synch.h"
#include "vm/frame.h"
#include "devices/block.h"

static void syscall_handler (struct intr_frame *);
int sys_read (int fd, const void *buffer, unsigned size);  
int sys_write (int fd, const void *buffer, unsigned size);
void sys_exit (int status);
void sys_seek (int fd, unsigned position);
int sys_mmap (int fd, void *addr);
void sys_munmap (int mapid);
bool sys_mkdir (const char *path);
bool sys_readdir (int fd, char *name);
bool sys_isdir (int fd);

int fd = 1;                              /* 각 file 에 fd 를 부여하기 위한 변수 */
struct lock fd_lock;                     /* lock for safely assigning fd */
struct semaphore read_sema, write_sema;  /* semaphore used for reader writer problem */
struct semaphore create_sema, remove_sema;
int reader_cnt;                          /* counter used for reader writer problem */
int mapid = 0;
struct lock mmap_lock;

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&fd_lock);
  sema_init (&read_sema, 1);
  sema_init (&write_sema, 1);
  sema_init (&create_sema, 1);
  sema_init (&remove_sema, 1);
  reader_cnt = 0;
  lock_init (&mmap_lock);
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{  
  switch (*(int *)(f->esp))
  {
  case SYS_HALT:
    shutdown_power_off ();
    break;
  case SYS_EXIT:
  {
    /* 커널 영역 침범하는지 확인 */
    if (is_kernel_vaddr(f->esp + 4))
      sys_exit (-1);
    int status = *(int *)(f->esp + 4);
    sys_exit (status);
    break;
  }
  case SYS_EXEC:
  {
    char *name = *(char **)(f->esp + 4);
    f->eax = process_execute (name);
    break;
  }
  case SYS_WAIT:
  {
    int pid = *(int *)(f->esp + 4);
    f->eax = process_wait (pid);
    break;
  }
  case SYS_CREATE:
  {
    /* 커널 영역 침범하는지 확인 */
    if (is_kernel_vaddr (f->esp + 4))
      sys_exit (-1);

    char *name = *(char **)(f->esp + 4);
    off_t initial_size = *(off_t *)(f->esp + 8);
    
    /* create 할 때 disk 에 비어있는 sector 를 찾으므로 sync 필요 */
    sema_down (&create_sema);
    f->eax = filesys_create (name, initial_size);
    sema_up (&create_sema);
    break;
  }
  case SYS_REMOVE:
  {
    char *filename = *(char **)(f->esp + 4);
    if (filename == NULL)
      f->eax = false;
    else
    {
      /* remove 를 하면 dir 에서 지워지기 때문에
         filesys_open 을 하기 전후로 remove_sema 를 이용한다. */
      sema_down (&remove_sema);
      f->eax = filesys_remove (filename);
      sema_up (&remove_sema);
    }
    break;
  }
  case SYS_OPEN:
  {
    /* 커널 영역 침범하는지 확인 */
    if (is_kernel_vaddr(f->esp + 4))
      sys_exit (-1);

    char *path = *(char **)(f->esp + 4);
    char filename[NAME_MAX + 1];

    if (path == NULL)
      f->eax = -1;
    else
    {
      sema_down (&remove_sema);
      struct inode *inode = filesys_open (path, filename);

      if (inode == NULL)
        f->eax = -1;
      else if (inode_is_dir (inode))
      {
        struct dir *dir = dir_open (inode);
        f->eax = add_dir_elem (filename, dir, 0);
      }
      else
      {
        struct file *file = file_open (inode);
        f->eax = add_file_elem (filename, file, 0);

        /* 현재 실행중인 파일이면 write 금지 */
        if (check_file_executing (filename))
          file_deny_write (file);
      }
      sema_up (&remove_sema);
    }
    break;
  }
  case SYS_FILESIZE:
  {
    struct list_elem *e;
    int fd = *(int *)(f->esp + 4);
  
    for (e = list_begin (&thread_current ()->file_list); 
        e != list_end (&thread_current ()->file_list);
        e = list_next (e))
    {
      struct file_elem *fe = list_entry (e, struct file_elem, elem);
      
      if (fe->fd == fd)
      {
        f->eax = file_length (fe->file_or_dir);
        return;
      }
    }
    sys_exit (-1);
    break;
  }
  case SYS_READ:
  {
    int fd = *(int *)(f->esp + 4);
    void *buffer = *(void **)(f->esp + 8);
    unsigned size = *(unsigned *)(f->esp + 12);

    /* 커널 영역 침범하는지 확인 */
    if (is_kernel_vaddr (buffer))
      sys_exit (-1);

    /* 만약 buffer 가 load 되지 않은 frame 이라면 미리 load 를 하고
       도중에 swap_out 되지 않도록 pin_lock 까지 걸어준다 */
    uint32_t buffer_ = pg_round_down (buffer);
    struct page *p = page_lookup ((void *)buffer_);
    if (p != NULL)
    {
      while (buffer_ <= buffer + size)
      {
        lock_acquire (&p->pin_lock);
        load_segment (p);
        buffer_ += PGSIZE;
        p = page_lookup ((void *)buffer_);
      }
    }

    sema_down (&read_sema);
    reader_cnt++;
    if (reader_cnt == 1)
      sema_down (&write_sema);
    sema_up (&read_sema);

    f->eax = sys_read (fd, buffer, size);
    
    sema_down (&read_sema);
    reader_cnt--;
    if (reader_cnt == 0)
      /* writer 에게 파일에 써도 된다고 표시 */
      sema_up (&write_sema);
    sema_up (&read_sema);
    
    buffer_ = pg_round_down (buffer);
    p = page_lookup ((void *)buffer_);
    if (p != NULL)
    {
      while (buffer_ <= buffer + size)
      {
        lock_release (&p->pin_lock);
        buffer_ += PGSIZE;
        p = page_lookup ((void *)buffer_);
      }
    }
    break;
  }
  case SYS_WRITE:
  {
    int fd = *(int *)(f->esp + 4);
    const void *buffer = *(void **)(f->esp + 8);
    unsigned size = *(unsigned *)(f->esp + 12);

    /* 커널 영역 침범하는지 확인 */
    if (is_kernel_vaddr (buffer))
      sys_exit (-1);
    
    /* write 도중 read 를 할 수 없도록 해준다 */
    sema_down (&write_sema);
    f->eax = sys_write (fd, buffer, size);
    sema_up (&write_sema);
    break;
  }
  case SYS_SEEK:
  {
    int fd = *(int *)(f->esp + 4);
    unsigned position = *(unsigned *)(f->esp + 8);

    /* pos 를 변경할 때 write/read 를 하면 안 되므로 sema 걸어준다. */
    sema_down (&write_sema);
    sys_seek (fd, position);
    sema_up (&write_sema);
    break;
  }
  case SYS_TELL:
  {
    int fd = *(int *)(f->esp + 4);

    /* pos 가 변경되면 안 되므로 sema 걸어준다. */
    sema_down (&write_sema);
    f->eax = sys_tell (fd);
    sema_up (&write_sema);
    break;
  }
  case SYS_CLOSE:
  {
    int fd = *(int *)(f->esp + 4);
    struct list_elem *e;

    for (e = list_begin (&thread_current ()->file_list); 
         e != list_end (&thread_current ()->file_list);
         e = list_next (e))
    {
      struct file_elem *fe = list_entry (e, struct file_elem, elem);
      
      if (fe->fd == fd)
      {
        /* thread 의 file list 에서 각 파일 제거 */
        list_remove (&fe->elem);
        if (!fe->is_dir)
          file_close (fe->file_or_dir);
        else
          dir_close (fe->file_or_dir);
        free (fe);
        break;
      }
    }
    break;
  }
  case SYS_MMAP:
  {
    int fd = *(int *)(f->esp + 4);
    void *addr = *(void **)(f->esp + 8);

    if (pg_round_down (addr) != addr)
      f->eax = -1;
    /* 커널 영역 침범하는지 확인 */
    else if (is_kernel_vaddr (addr))
      f->eax = -1;
    /* addr 가 0이거나 page 크기 단위가 아닌 경우 */
    else if (addr == 0 || (int) addr & 7 != 0)
      f->eax = -1;
    else
    {
      lock_acquire (&mmap_lock);
      f->eax = sys_mmap (fd, addr);
      lock_release (&mmap_lock);
    }
    break;
  }
  case SYS_MUNMAP:
  {
    int mapid = *(int *)(f->esp + 4);
		sys_munmap (mapid);
    break;
  }
  case SYS_CHDIR:
  {
    const char *path = *(char **)(f->esp + 4);
    f->eax = change_dir (path);
    break;
  }
  case SYS_MKDIR:
  {
    const char *path = *(char **)(f->esp + 4);
    f->eax = make_dir (path);
    break;
  }
  case SYS_READDIR:
  {
    int fd = *(int *)(f->esp + 4);
    char *name = *(char **)(f->esp + 8);
    f->eax = sys_readdir (fd, name);
    break;
  }
  case SYS_ISDIR:
  {
    int fd = *(int *)(f->esp + 4);
    f->eax = sys_isdir (fd);
    break;
  }
  case SYS_INUMBER:
  {
    int fd = *(int *)(f->esp + 4);
    f->eax = sys_inumber (fd);
    break;
  }
  default:
    printf ("Unknown system call...\n");
  }
}

struct file *
find_file_by_mapid (int mapid)
{
  struct list_elem *e;
  
  for (e = list_begin (&thread_current ()->file_list); 
       e != list_end (&thread_current ()->file_list);
       e = list_next (e))
  {
    struct file_elem *fe = list_entry (e, struct file_elem, elem);
    
    if (fe->mapid == mapid)
      return fe->file_or_dir;
  }
  return NULL;
}

struct file_elem *
find_file_elem (int fd)
{
  struct list_elem *e;

  for (e = list_begin (&thread_current ()->file_list); 
      e != list_end (&thread_current ()->file_list);
      e = list_next (e))
  {
    struct file_elem *fe = list_entry (e, struct file_elem, elem);
    
    if (fe->fd == fd)
      return fe;
  }
  return NULL;
}

void
sys_exit (int status)
{
  printf ("%s: exit(%d)\n", thread_current ()->name, status);
  if (thread_current ()->parent != NULL)
  {
    thread_current ()->info->exit_status = status;
    /* wait 중인 부모에게 죽음을 알리기 위해서 sema_up 한다. */
    sema_up (&thread_current ()->info->exit_sema);
  }
  thread_exit ();
}

/* file_list 에 file 의 정보를 담은 새로운 file_elem 을 추가한다. 
   mapid 가 0이 아닌 경우는 mmap 에서 오픈된 파일인 경우이다.
   할당된 fd 를 리턴한다. */
int
add_file_elem (char *filename, struct file *file, int mapid)
{
  if (file == NULL)
    return -1;
  
  /* file 정보를 담은 file_elem 을 file_list 에 추가 */
  struct file_elem *fe = malloc (sizeof (struct file_elem));
  fe->file_or_dir = file;
  memcpy (fe->filename, filename, 15);
  /* 안전하게 fd 를 부여하기 위한 synchronization */
  lock_acquire (&fd_lock);
  fe->fd = ++fd;
  fe->mapid = mapid;
  fe->is_dir = false;
  list_push_front (&thread_current ()->file_list, &fe->elem);
  lock_release (&fd_lock);
  return fd;
}

int
add_dir_elem (char *filename, struct dir *dir, int mapid)
{
  if (dir == NULL)
    return -1;
  
  /* file 정보를 담은 file_elem 을 file_list 에 추가 */
  struct file_elem *fe = malloc (sizeof (struct file_elem));
  fe->file_or_dir = dir;
  memcpy (fe->filename, filename, 15);
  /* 안전하게 fd 를 부여하기 위한 synchronization */
  lock_acquire (&fd_lock);
  fe->fd = ++fd;
  fe->mapid = mapid;
  fe->is_dir = true;
  list_push_front (&thread_current ()->file_list, &fe->elem);
  lock_release (&fd_lock);

  return fd;
}

int 
sys_read (int fd, const void *buffer, unsigned size)
{
  int result;

  /* stdin */
  if (fd == 0)  
  {
    for (int i=0; i<size; i++)
      if (input_getc () == '\0')
        return i;
  }
  /* 파일 읽기 실행 */
  else if (fd >= 2)
  {
    struct file_elem *fe = find_file_elem (fd);
    if (fe != NULL)
    {
      if (fe->is_dir)
        return -1;
      else
        return file_read (fe->file_or_dir, buffer, size);
    }
    else
      return -1;
  }
  /* 올바르지 않은 fd */
  return -1;
}

int
sys_write (int fd, const void *buffer, unsigned size)
{
   /* stdout */
  if (fd == 1)
  {
    putbuf (buffer, size);
    return size;
  }
  /* 파일 안에 쓰기 실행 */
  else if (fd >= 2)  
  {
    struct file_elem *fe = find_file_elem (fd);
    if (fe != NULL)
    {
      if (fe->is_dir)
        return -1;
      else
        return file_write (fe->file_or_dir, buffer, size);
    }
    return -1;
  }
   /* 올바르지 않은 fd */
  else
    return -1;
}

void 
sys_seek (int fd, unsigned position)
{
  struct file_elem *fe = find_file_elem (fd);
  if (fe != NULL)
    file_seek (fe->file_or_dir, position);
}

int
sys_tell (int fd)
{
  struct file_elem *fe = find_file_elem (fd);
  if (fe != NULL)
    return file_tell (fe->file_or_dir);
  return -1;
}

int
sys_mmap (int fd, void *addr)
{
  if (fd < 2)
    return -1;
  
  struct file_elem *fe = find_file_elem (fd);
  
  if (fe == NULL)
    return -1;
  
  int filesize = file_length (fe->file_or_dir);
  int zero_bytes;
  
  /* filesize 가 0인 경우 */
  if (filesize == 0)
    return -1;

  /* addr 가 stack 영역인 경우 */
  if (thread_current ()->stack_ofs < addr + filesize)
    return -1;
  
  /* file 이 addr 에 들어갔을 때 이미 할당되어 있는 영역에 침범되는 경우 
     -1을 리턴하고, 그렇지 않은 경우 zero_byte 를 계산한다. */
  void *addr_ = addr;
  for (zero_bytes = filesize; zero_bytes > 0; zero_bytes -= PGSIZE)
  {
    if(page_lookup (addr_))
      return -1;
    addr_ += PGSIZE;
  }
  zero_bytes = zero_bytes < 0 ? (-1) * zero_bytes : 0;

  /* reopen 을 해서 이미 열려있는 file 과 inode 를 공유한다. */
  struct file *file = file_reopen (fe->file_or_dir);

  /* 현재 실행중인 파일이면 write 금지 */
  if (check_file_executing (fe->filename))
    file_deny_write (file);

  /* file_list 와 upage_table 에 추가 */
  add_file_elem (fe->filename, file, ++mapid);
  save_upage (fe->filename, (off_t) 0, addr, filesize, zero_bytes, 
              true, mapid);
  return mapid;
}

/* mapid 를 가진 모든 page 를 (mapid 에 해당하는) 파일에 저장하고 free 한다. */
void
sys_munmap (int mapid)
{
  struct file *file = find_file_by_mapid (mapid);
  
  struct list_elem *e;
  for (e = list_begin (&thread_current ()->mmap_list);
       e != list_end (&thread_current ()->mmap_list);)
  {
    struct page *p = list_entry (e, struct page, mmap_elem);

    if (p->mapid != mapid)
    {
      e = list_next (&p->mmap_elem);
      continue;
    }

    uint32_t *pte = lookup_page (thread_current ()->pagedir, p->upage, false);
    
    /* load 하기 전의 page 인 경우 */
    if (pte == NULL)
      goto done;
    
    /* dirty bit 가 1인 경우 */
    if (file && *pte & 0x40)
    {
      sema_down (&write_sema);
      lock_acquire (&p->pin_lock);
      load_segment (p);

      file_write_at (file, p->upage, p->read_bytes, p->ofs);

      lock_release (&p->pin_lock);
      sema_up (&write_sema);
    }

    /* physical memory 정리 */
    void *vaddr = pagedir_get_page (thread_current ()->pagedir, p->upage);
    palloc_free_page (vaddr);
    remove_frame (p->upage);
    *pte &= ~1;

    /* mmap_list 와 upage_table 에서 제거하고 free */
    done:
    e = list_remove (&p->mmap_elem);
    hash_delete (&thread_current ()->upage_table, &p->hash_elem);
    free (p);
  }
}

int
sys_inumber (int fd)
{
  struct file_elem *fe = find_file_elem (fd);
  return fe != NULL ? file_get_inumber (fe->file_or_dir) : -1;
}

bool
sys_readdir (int fd, char *name)
{
  struct file_elem *fe = find_file_elem (fd);
  if (fe != NULL && fe->is_dir)
    return dir_readdir (fe->file_or_dir, name);
  return false;
}

bool
sys_isdir (int fd)
{
  struct file_elem *fe = find_file_elem (fd);
  return fe != NULL && fe->is_dir;
}