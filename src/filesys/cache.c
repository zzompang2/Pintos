#include "filesys/cache.h"
#include <stdio.h>
#include "threads/malloc.h"
#include "threads/thread.h"
#include "filesys/filesys.h"
#include "devices/block.h"

#define CACHE_MAX 64						/* Maximum number of blocks in buffer cache */

struct cache_list_header
{
	uint32_t length;		      		/* number of cache entries */
	struct list_elem *recent;   	/* Most recently updated entry for second chance */
};

struct cache_entry
{
	struct list_elem elem;
	uint32_t sector;							/* sector index */
	void *addr;										/* the memory address for this cache */
	struct lock flush_lock;				/* ensure cache is written to buffer safely */
	bool chance;									/* for second chance algorithm */
	bool dirty;               		/* indicate whether cache has been written or not */
};

/*list entry for the read_queue in read ahead */
struct read_entry
{
	struct list_elem elem;
	uint32_t sector_idx;					/*the index in the disk */
};

static struct cache_list_header header;
static struct list cache_list;				/* list of cache_entry */
static struct lock cache_lock;

static struct thread *read_thread;		/* do read-ahead */
static struct thread *write_thread;		/* do write-behind */
static struct list read_queue;				/* list of read_entry */
static bool exit_signal;							/* exit signal for read/write_thread */

static struct bitmap *cache_map;			/* cache 로 만들어진 sector_idx 표시 */

static void cache_flush (struct cache_entry *ce);
static void read_ahead (void);
static void write_behind (void);

void
cache_init ()
{
	list_init (&cache_list);
	header.length = 0;
	header.recent = list_head (&cache_list);
	lock_init (&cache_lock);

	cache_map = bitmap_create (block_size (fs_device));

	list_init (&read_queue);
	exit_signal = false;
	thread_create ("read_ahead", PRI_DEFAULT, read_ahead, NULL);
	thread_create ("write_behind", PRI_DEFAULT, write_behind, NULL);
}

/* sector_idx 의 캐시가 있는지 확인한다 */
static bool
no_cache (uint32_t sector_idx)
{
	return (bitmap_test (cache_map, sector_idx) & 1) == false;
}

/* sector_idx 를 갖는 cache entry 를 찾아서 반환한다 */
static struct cache_entry *
find_cache (uint32_t sector_idx)
{
	if (no_cache (sector_idx))
		return NULL;
	
	/* 같은 섹터를 연속으로 보는 경우가 자주 있으므로 recent 부터 찾아본다 */
	struct list_elem *e = header.recent;

	for (int i=0; i<header.length; i++)
	{
		struct cache_entry *ce = list_entry (e, struct cache_entry, elem);
		if (ce->sector == sector_idx)
			return ce;

		if ((e = list_next (e)) == list_end (&cache_list))
			e = list_begin (&cache_list);
	}
}

/* second chance 방식으로 victim entry 를 선택하고 리턴한다 */
static struct cache_entry *
find_victim_entry ()
{
	struct cache_entry *ce;
	struct list_elem *e = header.recent;

	ASSERT (header.length == CACHE_MAX);

	while (1)
	{
		/* 다시 처음으로 돌아가서 계속 victim 를 찾기 위한 조건문 */
		if ((e = list_next (e)) == list_end (&cache_list))
			   e = list_begin (&cache_list);

		ce = list_entry (e, struct cache_entry, elem);
		
		if (ce->chance)
			ce->chance = 0;
		/* victim 찾음 */
		else
			break;
	}

	header.recent = e;
	return ce;
}

/* 새로운 cache_entry 를 만든다. cache 가 가득 차있다면 victim entry 를
	 선택하여 덮어쓴다. 중복 검사는 하지 않으니, 실행하기 전 find_cache 를 실행해서
	 검사를 하는 것이 안전하다. */
static struct cache_entry *
make_cache (uint32_t sector_idx)
{
	struct cache_entry *ce;

	if (header.length < CACHE_MAX)
	{
		/* cache 가 가득 차지 않은 경우 새로운 entry 를 만든다 */
		ce = malloc (sizeof (struct cache_entry));
		ce->addr = malloc (BLOCK_SECTOR_SIZE);
		ce->dirty = 0;
		lock_init (&ce->flush_lock);
		
		/* recent 의 next 위치에 넣고 recent 를 업데이트 */
		list_insert (header.recent->next, &ce->elem);
		header.recent = &ce->elem;
		header.length++;
	}
	else
	{
		/* cache 가 가득 찬 경우 victim entry 선택하고
			 dirty 한 경우 디스크에 write 한다 */
		ce = find_victim_entry ();
		cache_flush (ce);
		bitmap_set (cache_map, ce->sector, false);
	}

	/* cache entry 초기화 */
	ce->chance = 1;
	ce->sector = sector_idx;
	bitmap_set (cache_map, ce->sector, true);

	return ce;
}

/* function for read-ahead thread
 * 파일의 한 섹터를 읽었을 때, 그 다음 섹터를 미리 읽는다. */
static void
read_ahead (void)
{
	read_thread = thread_current ();
	struct read_entry *entry;

	while (1)
	{
		if (exit_signal)
			break;

		intr_disable ();
		thread_block ();

		/* idle () 에 있는 코멘트: 
			인터럽트를 다시 활성화하고 다음 인터럽트를 기다리십시오.
			'sti' instruction 은 다음 instruction 이 완료될 때까지 인터럽트를 
			비활성화하므로, 이 두 가지 지침은 원자적으로 실행된다. 이 원자성(atomicity)은 
			중요하다. 그렇지 않으면, 인터럽트를 다시 활성화하고 다음 인터럽트가 발생하기를 
			기다리는 사이에 인터럽트가 처리되어 하나의 clock tick 만큼 낭비될 수 있다. */
		asm volatile ("sti; hlt" : : : "memory");
		
		while (!list_empty (&read_queue))
		{	
			/* cache_list 탐색을 동시에 하지 못하도록 lock 을 걸어준다 */
			lock_acquire (&cache_lock);
			
			entry = list_entry (list_pop_front (&read_queue), 
													struct read_entry, elem);

			if (no_cache (entry->sector_idx))
			{
				/* sector_idx 의 캐시를 만든다 */
				struct cache_entry *ce = make_cache (entry->sector_idx);

				lock_acquire (&ce->flush_lock);
				lock_release (&cache_lock);

				block_read (fs_device, ce->sector, ce->addr);

				lock_release (&ce->flush_lock);
			}
			else
				lock_release (&cache_lock);
		}
	}

	read_thread = NULL;
}

/* function for write-behind thread
 * 일정 tick 마다 자동으로 디스크에 업데이트(block_write)한다. */
static void
write_behind (void)
{
	write_thread = thread_current ();

	while (1)
	{
		if (exit_signal)
			break;
		
		/* 50 ticks 뒤에 재실행 */
		timer_sleep (50);

		lock_acquire (&cache_lock);

		struct list_elem *e;
		for (e = list_begin (&cache_list); e != list_end (&cache_list);
				 e = list_next (e))
		{
			/* dirty 한 모든 캐시를 디스크로 write 한다 */
			struct cache_entry *ce = list_entry (e, struct cache_entry, elem);			
			cache_flush (ce);
		}

		lock_release (&cache_lock);
	}

	write_thread = NULL;
}

/* sector_idx 의 캐시에서 (offset 부터 chunk_size 바이트 만큼) 
   buffer 로 read 한다 */
void
cache_read (uint32_t sector_idx, int32_t next_sector_idx, uint8_t *buffer, int chunk_size, int sector_ofs)
{
	struct cache_entry *ce;

	ASSERT (chunk_size >= 0 && chunk_size <= BLOCK_SECTOR_SIZE);

	/* cache_list 탐색을 동시에 하지 못하도록 lock 을 걸어준다 */
	lock_acquire (&cache_lock);

	/* cache 에 존재하는지 확인 */
	if (!(ce = find_cache (sector_idx)))
	{
		/* cache 에 존재하는지 않는 경우 새로 만든다 */
		ce = make_cache (sector_idx);

		/* 디스크에 write 하기 전에 cache 가 flush 되지 않도록 lock 을 걸어준다 */
		lock_acquire (&ce->flush_lock);
		lock_release (&cache_lock);

		block_read (fs_device, ce->sector, ce->addr);
	}
	else
	{
		/* 디스크에 write 하기 전에 cache 가 flush 되지 않도록 lock 을 걸어준다 */
		lock_acquire (&ce->flush_lock);
		lock_release (&cache_lock);
	}

	/* 파일의 다음 섹터가 존재하면 read-ahead */
	if (next_sector_idx != -1 && no_cache (next_sector_idx))
	{
		struct read_entry *entry = malloc (sizeof (struct read_entry));
		entry->sector_idx = next_sector_idx;
		list_push_back (&read_queue, &entry->elem);

		/* lock_aquire 로 인해 block 된 경우에는 unblock 하지 않는다 */
		if (read_thread->status == THREAD_BLOCKED && 
				read_thread->wishing_lock == NULL)
			thread_unblock (read_thread);
	}

	/* cache 에서 버퍼로 데이터 복사 */
	memcpy (buffer, ce->addr + sector_ofs, chunk_size);
	
	/* give chance again */
	ce->chance = 1;
	lock_release (&ce->flush_lock);
}

/* buffer 에서 sector_idx 캐시의 offset 부터 chunk_size 바이트 위치에
   write 한다 */
void
cache_write (uint32_t sector_idx, int32_t next_sector_idx, 
						 uint8_t *buffer, int chunk_size, int sector_ofs)
{
	struct cache_entry *ce;

	ASSERT (chunk_size >= 0 && chunk_size <= BLOCK_SECTOR_SIZE);

	/* cache_list 탐색을 동시에 하지 못하도록 lock 을 걸어준다 */
	lock_acquire (&cache_lock);

	/* cache 에 존재하는지 확인 */
	if (!(ce = find_cache (sector_idx)))
	{
		/* cache 에 존재하는지 않는 경우 새로 만든다 */
		ce = make_cache (sector_idx);

		/* 디스크에 write 하기 전에 cache 가 flush 되지 않도록 lock 을 걸어준다 */
		lock_acquire (&ce->flush_lock);
		lock_release (&cache_lock);

		block_read (fs_device, ce->sector, ce->addr);
	}
	else
	{
		/* 디스크에 write 하기 전에 cache 가 flush 되지 않도록 lock 을 걸어준다 */
		lock_acquire (&ce->flush_lock);
		lock_release (&cache_lock);
	}

	/* 파일의 다음 섹터가 존재하면 read-ahead */
	if (next_sector_idx != -1 && no_cache (next_sector_idx))
	{
		struct read_entry *entry = malloc (sizeof (struct read_entry));
		entry->sector_idx = next_sector_idx;
		list_push_back (&read_queue, &entry->elem);

		/* lock_aquire 로 인해 block 된 경우에는 unblock 하지 않는다 */
		if (read_thread->status == THREAD_BLOCKED && 
				read_thread->wishing_lock == NULL)
			thread_unblock (read_thread);
	}

	/* 버퍼에서 cache 로 데이터 복사 */
	memcpy (ce->addr + sector_ofs, buffer, chunk_size);
	
	/* set dirty bit & give chance again */
	ce->dirty = 1;
	ce->chance = 1;
	lock_release (&ce->flush_lock);
}

/* dirty 한 cache 를 디스크에 쓴다 */
static void
cache_flush (struct cache_entry *ce)
{
	/* read 또는 write 중이라면 대기 */
	lock_acquire (&ce->flush_lock);

	if (ce->dirty)
	{
		block_write (fs_device, ce->sector, ce->addr);
		ce->dirty = 0;
	}

	lock_release (&ce->flush_lock);
}

/* 모든 cache 를 flush 하고 파일 메모리 및 entry 제거 */
void
all_cache_flush ()
{
	lock_acquire (&cache_lock);

	struct list_elem *e;
	for (e = list_begin (&cache_list); e != list_end (&cache_list);)
	{
		/* dirty 한 캐시를 디스크로 write 한다 */
		struct cache_entry *ce = list_entry (e, struct cache_entry, elem);
		cache_flush (ce);
		
		/* 메모리 정리 */
		e = list_remove (e);
		free (ce->addr);
		free (ce);
	}

	lock_release (&cache_lock);
}

/* read_thread, write_thread 가 반복을 끝내고 종료될 수 있도록 flag 를 설정한다 */
void
read_write_thread_exit ()
{
	exit_signal = true;
}