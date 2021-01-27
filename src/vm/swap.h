#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stdio.h>
#include <threads/vaddr.h>
#include "devices/block.h"

#define DISK_SECTOR_SIZE PGSIZE / 8

void swap_init ();
void swap_in (block_sector_t index, void* upage, void* frame, bool writable);
bool swap_out (void);

#endif /* vm/swap.h */