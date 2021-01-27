#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <stdio.h>
#include "lib/kernel/hash.h"
#include "threads/synch.h"

enum status
{
	UNLOAD = 0,
	IN_MEMORY,
	IN_DISK
};

struct page
{
	struct hash_elem hash_elem; 	/* Hash table element. */
	struct list_elem mmap_elem;		/* Mmap list element. */
	void *upage;                 	/* Virtual address. */
	char file_name[15];						/* Name of ELF file */
	int32_t ofs;									/* Offset in the file to start reading */
	uint32_t read_bytes;					/* Amount of bytes to be read */
	uint32_t zero_bytes; 					/* PGSIZE - read_bytes */
	bool writable;								/* indicate if segment is writable */
	enum status status;						/* the current status of the page */
	uint32_t swap_index;					/* the index in the swap table */
	struct lock pin_lock;					/* pin indicating that file is reading */
	int mapid;
};

unsigned page_hash (const struct hash_elem *elem);
bool page_less (const struct hash_elem *a_, const struct hash_elem *b_);
struct page *page_lookup (const void *address);
void stack_growth (void *esp);

#endif /* vm/page.h */