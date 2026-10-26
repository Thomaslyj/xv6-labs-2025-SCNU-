#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

#ifdef LAB_NET
  // PCI-E ECAM (configuration space), for pci.c
  kvmmap(kpgtbl, 0x30000000L, 0x30000000L, 0x10000000, PTE_R | PTE_W);

  // pci.c maps the e1000's registers here.
  kvmmap(kpgtbl, 0x40000000L, 0x40000000L, 0x20000, PTE_R | PTE_W);
#endif  

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
#ifdef LAB_PGTBL
      if(PTE_LEAF(*pte)) {
        return pte;
      }
#endif
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}
// Map one aligned 2 MiB region using a level-1 leaf PTE.
int
supermappages(pagetable_t pagetable, uint64 va, uint64 pa, int perm)
{
  if(va >= MAXVA ||
     va % SUPERPGSIZE != 0 ||
     pa % SUPERPGSIZE != 0)
    panic("supermappages: alignment");

  // Find the level-2 entry.
  pte_t *pte = &pagetable[PX(2, va)];
  pagetable_t next;

  if(*pte & PTE_V){
    if(PTE_LEAF(*pte))
      return -1;

    next = (pagetable_t)PTE2PA(*pte);
  } else {
    next = (pagetable_t)kalloc();
    if(next == 0)
      return -1;

    memset(next, 0, PGSIZE);
    *pte = PA2PTE((uint64)next) | PTE_V;
  }

  // Install a leaf directly in the level-1 table.
  pte = &next[PX(1, va)];

  if(*pte & PTE_V){
    if(PTE_LEAF(*pte))
      return -1;

    pagetable_t small = (pagetable_t)PTE2PA(*pte);

    // Replace only a completely empty level-0 table.
    for(int i = 0; i < 512; i++){
      if(small[i] & PTE_V)
        return -1;
    }

    *pte = 0;
    sfence_vma();
    kfree((void *)small);
  }

  *pte = PA2PTE(pa) | perm | PTE_V;
  return 0;
}

// Find a valid leaf PTE and report its level.
static pte_t *
walkleaf(pagetable_t pagetable, uint64 va, int *leaflevel)
{
  if(va >= MAXVA)
    return 0;

  for(int level = 2; level >= 0; level--){
    pte_t *pte = &pagetable[PX(level, va)];

    if((*pte & PTE_V) == 0)
      return 0;

    if(PTE_LEAF(*pte)){
      *leaflevel = level;
      return pte;
    }

    if(level == 0)
      return 0;

    pagetable = (pagetable_t)PTE2PA(*pte);
  }

  return 0;
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
// Return the physical base of the 4 KiB page containing va.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  int level;
  pte_t *pte = walkleaf(pagetable, va, &level);

  if(pte == 0)
    return 0;

  if((*pte & PTE_U) == 0)
    return 0;

  uint64 size = 1UL << PXSHIFT(level);
  uint64 offset = PGROUNDDOWN(va) & (size - 1);

  return PTE2PA(*pte) + offset;
}


#if defined(LAB_PGTBL) || defined(SOL_MMAP) || defined(SOL_COW)

// Print valid entries, then visit any lower-level page tables.
static void
vmprint_walk(pagetable_t pagetable, int level, uint64 base)
{
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];

    // Skip invalid entries.
    if((pte & PTE_V) == 0)
      continue;

    uint64 va = base | ((uint64)i << PXSHIFT(level));
    uint64 pa = PTE2PA(pte);

    // L2: one " .."; L1: two; L0: three.
    for(int depth = 0; depth < 3 - level; depth++)
      printf(" ..");

    printf("%p: pte %p pa %p\n",
       (void *)va, (void *)pte, (void *)pa);

    // Recurse only if this entry points to another page table.
    if(level > 0 && (pte & (PTE_R | PTE_W | PTE_X)) == 0)
      vmprint_walk((pagetable_t)pa, level - 1, va);
  }
}

void
vmprint(pagetable_t pagetable)
{
  printf("page table %p\n", (void *)pagetable);
  vmprint_walk(pagetable, 2, 0);
}

#endif



// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}
// Replace one level-1 leaf with 512 ordinary page mappings.
// On allocation failure, leave the original mapping unchanged.
static int
superdemote(pte_t *pte)
{
  uint64 oldpa = PTE2PA(*pte);
  uint flags = PTE_FLAGS(*pte);

  pagetable_t small = (pagetable_t)kalloc();
  if(small == 0)
    return -1;

  memset(small, 0, PGSIZE);

  int i;
  for(i = 0; i < 512; i++){
    char *mem = kalloc();
    if(mem == 0)
      goto fail;

    memmove(mem, (void *)(oldpa + (uint64)i * PGSIZE),
            PGSIZE);

    small[i] = PA2PTE((uint64)mem) | flags;
  }

  // Publish the new page table only after all copies succeed.
  *pte = PA2PTE((uint64)small) | PTE_V;
  sfence_vma();

  superfree((void *)oldpa);
  return 0;

fail:
  for(int j = 0; j < i; j++)
    kfree((void *)PTE2PA(small[j]));

  kfree((void *)small);
  return -1;
}
// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va,
         uint64 npages, int do_free)
{
  if(va % PGSIZE != 0)
    panic("uvmunmap: not aligned");

  uint64 end = va + npages * PGSIZE;
  uint64 a = va;

  while(a < end){
    int level;
    pte_t *pte = walkleaf(pagetable, a, &level);

    // Holes are allowed, as in the original implementation.
    if(pte == 0){
      a += PGSIZE;
      continue;
    }

    if(level == 1){
      // Complete, aligned superpage removal.
      if(a % SUPERPGSIZE == 0 &&
         end - a >= SUPERPGSIZE){
        uint64 pa = PTE2PA(*pte);

        *pte = 0;
        sfence_vma();

        if(do_free)
          superfree((void *)pa);

        a += SUPERPGSIZE;
        continue;
      }

      // Partial removal requires ordinary page mappings.
      if(!do_free)
        panic("uvmunmap: partial superpage without free");

      if(superdemote(pte) < 0)
        panic("uvmunmap: demote out of memory");

      // Revisit this address using the new level-0 table.
      continue;
    }

    if(level != 0)
      panic("uvmunmap: unsupported leaf level");

    uint64 pa = PTE2PA(*pte);

    *pte = 0;
    sfence_vma();

    if(do_free)
      kfree((void *)pa);

    a += PGSIZE;
  }
}


// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz,
         uint64 newsz, int xperm)
{
  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  uint64 a = oldsz;

  while(a < newsz){
    // Prefer a superpage for a complete, aligned 2 MiB region.
    if(a % SUPERPGSIZE == 0 &&
       newsz - a >= SUPERPGSIZE){
      char *mem = superalloc();

      if(mem != 0){
        memset(mem, 0, SUPERPGSIZE);

        if(supermappages(pagetable, a, (uint64)mem,
                        PTE_R | PTE_U | xperm) == 0){
          a += SUPERPGSIZE;
          continue;
        }

        superfree(mem);
      }
    }

    // Ordinary pages handle small regions and allocation fallback.
    char *mem = kalloc();
    if(mem == 0)
      goto fail;

    memset(mem, 0, PGSIZE);

    if(mappages(pagetable, a, PGSIZE, (uint64)mem,
                PTE_R | PTE_U | xperm) != 0){
      kfree(mem);
      goto fail;
    }

    a += PGSIZE;
  }

  return newsz;

fail:
  uvmunmap(pagetable, oldsz, (a - oldsz) / PGSIZE, 1);
  return 0;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  uint64 start = PGROUNDUP(newsz);
  uint64 end = PGROUNDUP(oldsz);

  if(start < end){
    int level;
    pte_t *pte = walkleaf(pagetable, start, &level);

    // A release starting inside a superpage needs demotion.
    if(pte != 0 && level == 1 &&
       start % SUPERPGSIZE != 0){
      if(superdemote(pte) < 0)
        return oldsz;
    }

    uvmunmap(pagetable, start, (end - start) / PGSIZE, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      // backtrace();
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  uint64 i = 0;

  while(i < sz){
    int level;
    pte_t *pte = walkleaf(old, i, &level);

    // Preserve holes left by lazy allocation.
    if(pte == 0){
      i += PGSIZE;
      continue;
    }

    uint64 pa = PTE2PA(*pte);
    uint flags = PTE_FLAGS(*pte);

    if(level == 1){
      // A superpage must be copied as one complete aligned region.
      if(i % SUPERPGSIZE != 0 ||
          PGROUNDUP(sz) - i < SUPERPGSIZE)
      goto err;

      char *mem = superalloc();
      if(mem == 0)
        goto err;

      memmove(mem, (void *)pa, SUPERPGSIZE);

      if(supermappages(new, i, (uint64)mem, flags) < 0){
        superfree(mem);
        goto err;
      }

      i += SUPERPGSIZE;
    } else if(level == 0){
      char *mem = kalloc();
      if(mem == 0)
        goto err;

      memmove(mem, (void *)pa, PGSIZE);

      if(mappages(new, i, PGSIZE, (uint64)mem, flags) < 0){
        kfree(mem);
        goto err;
      }

      i += PGSIZE;
    } else {
      // This lab does not implement level-2 (1 GiB) pages.
      goto err;
    }
  }

  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;

    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }

    if((pte = walk(pagetable, va0, 0)) == 0) {
      // printf("copyout: pte should exist %lx %ld\n", dstva, len);
      return -1;
    }


    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
    
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;
  
  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}




// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();
  

  if (va >= p->sz)
    return 0;
  va = PGROUNDDOWN(va);
  if(ismapped(pagetable, va)) {
    return 0;
  }
  mem = (uint64) kalloc();
  if(mem == 0)
    return 0;
  memset((void *) mem, 0, PGSIZE);
  if (mappages(p->pagetable, va, PGSIZE, mem, PTE_W|PTE_U|PTE_R) != 0) {
    kfree((void *)mem);
    return 0;
  }
  return mem;
}

int
ismapped(pagetable_t pagetable, uint64 va) {
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}



#ifdef LAB_PGTBL
pte_t*
pgpte(pagetable_t pagetable, uint64 va) {
  return walk(pagetable, va, 0);
}
#endif
