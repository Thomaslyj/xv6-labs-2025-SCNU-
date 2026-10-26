#include "kernel/types.h"
#include "kernel/riscv.h"
#include "user/user.h"

static void
fail(char *message)
{
  fprintf(2, "dirtytest: FAIL: %s\n", message);
  exit(1);
}

static void
checkmask(char *message, void *start, int npages, uint64 expected)
{
  uint64 mask = 0;

  if(pgdirty(start, npages, &mask) != 0)
    fail("pgdirty returned an error");

  printf("%s: mask=0x%lx expected=0x%lx\n",
         message, mask, expected);

  if(mask != expected)
    fail(message);
}

int
main(void)
{
  // Allocate enough space for 64 complete, aligned ordinary pages.
  char *raw = sbrk(65 * PGSIZE);
  if(raw == SBRK_ERROR)
    fail("ordinary allocation failed");

  volatile char *pages =
    (volatile char *)PGROUNDUP((uint64)raw);

  // Kernel initialization does not dirty the user mappings.
  checkmask("initial", (void *)pages, 64, 0);

  // A read may set A, but should not set D.
  if(pages[PGSIZE] != 0)
    fail("new page was not zeroed");

  checkmask("after read", (void *)pages, 64, 0);

  // Write to pages 0, 2, and 63.
  pages[0] = 1;
  pages[2 * PGSIZE] = 2;
  pages[63 * PGSIZE] = 3;

  uint64 expected = (1ULL << 0) |
                    (1ULL << 2) |
                    (1ULL << 63);

  checkmask("after writes", (void *)pages, 64, expected);

  // Querying should not clear the dirty bits.
  checkmask("query again", (void *)pages, 64, expected);

  // Invalid arguments should return -1.
  uint64 mask;

  if(pgdirty((void *)pages, 0, &mask) != -1)
    fail("accepted zero pages");

  if(pgdirty((void *)pages, 65, &mask) != -1)
    fail("accepted more than 64 pages");

  if(pgdirty((void *)((uint64)pages + 1), 1, &mask) != -1)
    fail("accepted an unaligned address");

  if(pgdirty((void *)pages, 1, (uint64 *)MAXVA) != -1)
    fail("accepted an invalid output address");

  printf("ordinary pages and arguments: OK\n");

  // Allocate enough space to contain a complete aligned superpage.
  char *bigraw = sbrk(2 * SUPERPGSIZE);
  if(bigraw == SBRK_ERROR)
    fail("superpage allocation failed");

  volatile char *big =
    (volatile char *)SUPERPGROUNDUP((uint64)bigraw);

  uint64 pte = pgpte((void *)big);

  if((pte & (PTE_V | PTE_R | PTE_W | PTE_U)) !=
     (PTE_V | PTE_R | PTE_W | PTE_U))
    fail("unexpected superpage permissions");

  if(pte != pgpte((void *)((uint64)big + PGSIZE)))
    fail("expected a shared superpage PTE");

  checkmask("superpage initial", (void *)big, 4, 0);

  // All four queried regions share the same superpage dirty bit.
  big[PGSIZE] = 7;

  checkmask("superpage after write", (void *)big, 4, 0xF);

  printf("dirtytest: all tests passed\n");
  exit(0);
}