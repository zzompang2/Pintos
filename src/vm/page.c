#include <stdio.h>
#include "vm/page.h"
#include "userprog/process.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"

/* Returns a hash value for page p. */
unsigned
page_hash (const struct hash_elem *elem)
{
  const struct page *p = hash_entry (elem, struct page, hash_elem);
  return hash_bytes (&p->upage, sizeof p->upage);
}

/* Returns true if page a precedes page b. */
bool
page_less (const struct hash_elem *a_, const struct hash_elem *b_)
{
  const struct page *a = hash_entry (a_, struct page, hash_elem);
  const struct page *b = hash_entry (b_, struct page, hash_elem);

  return a->upage < b->upage;
}
 	
/* Returns the page containing the given virtual address,
   or a null pointer if no such page exists. */
struct page *
page_lookup (const void *address)
{
  struct page p;
  struct hash_elem *e;

  p.upage = pg_round_down (address);
  e = hash_find (&thread_current ()->upage_table, &p.hash_elem);
  return e != NULL ? hash_entry (e, struct page, hash_elem) : NULL;
}

/* allocate page(s) for the stack. This function is called when a 
   page fault occurs */
void
stack_growth (void *addr)
{
  while (thread_current ()->stack_ofs != pg_round_down (addr))
  {
    uint8_t *kpage;
    bool success = false;

    kpage = palloc_get_page (PAL_USER | PAL_ZERO);
    if (kpage != NULL) 
    {
      success = install_page ((uint8_t *)thread_current ()->stack_ofs - PGSIZE, kpage, true);
      if (!success)
        palloc_free_page (kpage);
    }
    thread_current()->stack_ofs -= PGSIZE;
  }
}