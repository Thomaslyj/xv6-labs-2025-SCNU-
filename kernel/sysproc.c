#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#ifdef PGTBL_SOL
#include "riscv.h"
#endif
#include "vm.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if(t == SBRK_EAGER || n < 0) {
    if(growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if(addr + n < addr)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;


  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}


#ifdef LAB_PGTBL
int
sys_pgpte(void)
{
  uint64 va;
  struct proc *p;  

  p = myproc();
  argaddr(0, &va);
  pte_t *pte = pgpte(p->pagetable, va);
  if(pte != 0) {
      return (uint64) *pte;
  }
  return 0;
}
#endif

#ifdef LAB_PGTBL
int
sys_kpgtbl(void)
{
  struct proc *p;  

  p = myproc();
  vmprint(p->pagetable);
  return 0;
}
#endif


uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

#ifdef LAB_PGTBL
uint64
sys_pgdirty(void)
{
  uint64 start;
  uint64 usermask;
  int npages;

  argaddr(0, &start);
  argint(1, &npages);
  argaddr(2, &usermask);

  struct proc *p = myproc();

  if(npages < 1 || npages > 64)
    return -1;

  if(start % PGSIZE != 0)
    return -1;

  uint64 bytes = (uint64)npages * PGSIZE;

  // Require a complete range inside ordinary process memory.
  if(start >= MAXVA || bytes > MAXVA - start)
    return -1;

  if(start > p->sz || bytes > p->sz - start)
    return -1;

  uint64 mask = 0;

  for(int i = 0; i < npages; i++){
    uint64 va = start + (uint64)i * PGSIZE;
    pte_t *pte = walk(p->pagetable, va, 0);

    if(pte == 0)
      continue;

    if((*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
      continue;

    if(!PTE_LEAF(*pte))
      continue;

    if(*pte & PTE_D)
      mask |= 1ULL << i;
  }

  if(copyout(p->pagetable, usermask,
             (char *)&mask, sizeof(mask)) < 0)
    return -1;

  return 0;
}
#endif