#include <stdio.h>
#include <bitmap.h>
#include "vm/swap.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "vm/page.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "filesys/file.h"

struct bitmap *swap_bitmap;
struct block *swap_block;
struct lock swap_lock;

void *read (void *aux, block_sector_t t, void *buffer);
void *write (void *aux, block_sector_t t, const void *buffer);

void
swap_init ()
{
	lock_init (&swap_lock);
	swap_bitmap = bitmap_create (1024 * 8);
	swap_block = block_get_role (BLOCK_SWAP);
}

bool
swap_out (void)
{
	lock_acquire (&swap_lock);

	/* kernel virtual address that ptte pointed to */
	void *lru_upage = lru_remove ();
	if (lru_upage == NULL)
		return  false;

	struct page *p = page_lookup (lru_upage);
	uint32_t *pte = lookup_page (thread_current ()->pagedir, lru_upage, false);
	void *vaddr = pagedir_get_page (thread_current ()->pagedir, lru_upage);

	/* mapping 된 page 가 아닌 경우, disk 로 데이터를 보낸다. */
	if (p->mapid == 0)
	{
		block_sector_t free_index = bitmap_scan_and_flip (swap_bitmap, 0, 8, false);

		if (free_index == BITMAP_ERROR)
			ASSERT (false);
		
		for (int i=0; i<8; i++)
			block_write (swap_block, free_index + i, (uint8_t *)vaddr + i * DISK_SECTOR_SIZE);

		/* sys_read 중이면 기다리기 */
		lock_acquire (&p->pin_lock);

		p->status = IN_DISK;
		p->swap_index = free_index;

		lock_release (&p->pin_lock);
	}
	/* mapping 된 page 인 경우, disk 로 보내지 않고
	   file 에 내용을 업데이트 하고 free 된다. */
	else
	{
		/* dirty bit 가 1인 경우 */
		if (*pte & 0x40)
		{
			struct file *file = find_file_by_mapid (p->mapid);

			if (file)
			{
				lock_acquire (&p->pin_lock);
				load_segment (p);

				file_write_at (file, p->upage, p->read_bytes, p->ofs);

				lock_release (&p->pin_lock);
			}
		}
		p->status = UNLOAD;
	}
	/* physical memory 정리 */
	palloc_free_page (vaddr);
	remove_frame (p->upage);
	*pte &= ~1;
	lock_release (&swap_lock);
	return true;
}

void
swap_in (block_sector_t index, void* upage, void* frame, bool writable)
{  
	lock_acquire (&swap_lock);

	/* disk 에 아무것도 없는 경우 */
	if (bitmap_test (swap_bitmap, index) == false)
		ASSERT (false);
	
	for (int i=0; i<8; i++)
	{
		bitmap_flip (swap_bitmap, index + i);		/* bitmap 초기화 */
		block_read (swap_block, index + i, (uint8_t *)frame + i * DISK_SECTOR_SIZE);
	}
	install_page (upage, frame, writable);

	/* Save pte into the frame table */
  struct frame *f = malloc (sizeof (struct frame));
  f->upage = upage;
  insert_frame (&f->elem);
	
	lock_release (&swap_lock);
}