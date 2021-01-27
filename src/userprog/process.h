#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "filesys/file.h"
#include "vm/page.h"

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);
bool load_segment (struct page *p);
bool install_page (void *upage, void *kpage, bool writable);
void save_upage (const char *file_name, off_t ofs, uint8_t *upage,
                 uint32_t read_bytes, uint32_t zero_bytes, 
                 bool writable, int mapid);
#endif /* userprog/process.h */
