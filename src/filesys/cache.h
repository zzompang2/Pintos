#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include <stdint.h>
#include <stdbool.h>
#include "list.h"
#include "threads/synch.h"

void cache_read (uint32_t sector_idx, int32_t next_sector_idx, uint8_t *buffer, int chunk_size, int sector_ofs);
void cache_write (uint32_t sector_idx, int32_t next_sector_idx, uint8_t *buffer, int chunk_size, int sector_ofs);
void all_cache_flush ();
void read_write_thread_exit ();

#endif /* filesys/cache.h */