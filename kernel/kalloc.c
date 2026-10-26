// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;
// Reserve the last 32 MiB for sixteen 2 MiB pages.
#define SUPERBASE (PHYSTOP - 16UL * SUPERPGSIZE)

static struct {
  struct spinlock lock;
  struct run *freelist;
} supermem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&supermem.lock, "supermem");

  if(SUPERBASE % SUPERPGSIZE != 0 ||
     (uint64)end >= SUPERBASE)
    panic("super pool layout");

  // Ordinary pages use only memory below SUPERBASE.
  freerange(end, (void *)SUPERBASE);

  // Superpages use the remaining 32 MiB.
  for(uint64 pa = SUPERBASE;
      pa + SUPERPGSIZE <= PHYSTOP;
      pa += SUPERPGSIZE)
    superfree((void *)pa);
}
void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 ||
     (char *)pa < end ||
     (uint64)pa >= SUPERBASE)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
// Return one 2 MiB page to the superpage pool.
void
superfree(void *pa)
{
  uint64 addr = (uint64)pa;

  if(addr % SUPERPGSIZE != 0 ||
     addr < SUPERBASE ||
     addr >= PHYSTOP)
    panic("superfree");

  memset(pa, 1, SUPERPGSIZE);

  struct run *r = (struct run *)pa;

  acquire(&supermem.lock);
  r->next = supermem.freelist;
  supermem.freelist = r;
  release(&supermem.lock);
}

// Allocate one 2 MiB page, or return 0 if none is available.
void *
superalloc(void)
{
  struct run *r;

  acquire(&supermem.lock);
  r = supermem.freelist;
  if(r)
    supermem.freelist = r->next;
  release(&supermem.lock);

  if(r)
    memset((void *)r, 5, SUPERPGSIZE);

  return (void *)r;
}