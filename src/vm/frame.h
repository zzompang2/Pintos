#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <stdio.h>
#include "lib/kernel/hash.h"

struct list frame_table;

struct frame
{
	struct list_elem elem; 				/* frame_table element. */
	void *upage;                	/* the user virtual address that frame maps to. */
};

void *lru_remove ();

#endif /* vm/frame.h */