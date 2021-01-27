#include <stdio.h>
#include "vm/frame.h"
#include "threads/pte.h"
#include "threads/thread.h"

struct lock frame_lock;

/* Initialize frame table. */
void
frame_init ()
{
	list_init (&frame_table);
	lock_init (&frame_lock);
}

void
insert_frame (struct list_elem *elem)
{
	lock_acquire (&frame_lock);
	list_push_back (&frame_table, elem);
	lock_release (&frame_lock);
}

void
remove_frame (void *upage)
{

	lock_acquire (&frame_lock);
	struct list_elem *e;
	struct frame *f;
	
	for (e = list_front (&frame_table); e != list_end (&frame_table);
			 e = list_next (e))
	{
		f = list_entry (e, struct frame, elem);
		if (f->upage == upage)
		{
			list_remove (&f->elem);
			free (f);
			break;
		}
	}
	lock_release (&frame_lock);
}

void *
lru_remove ()
{
	if (list_empty (&frame_table))
  	return NULL;

	struct frame *lru_frame;
	void *vaddr;

	lock_acquire (&frame_lock);
	lru_frame = list_entry (list_pop_front (&frame_table), struct frame, elem);
	lock_release (&frame_lock);

	/* free */
	void *upage = lru_frame->upage;
	free (lru_frame);

	return upage;
}

void *
reaccess_frame (void *upage)
{
	lock_acquire (&frame_lock);
	struct list_elem *e;
  for (e = list_begin (&frame_table); e != list_end (&frame_table);
       e = list_next (e))
  {
    struct frame *f = list_entry (e, struct frame, elem);
    if (f->upage == upage & PTE_ADDR)
    {
      list_remove (e);
      list_push_back (&frame_table, e);
      break;
    }
  }
	lock_release (&frame_lock);
}