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

void kfree_init(void *pa);

struct run {
  struct run *next;
};

struct free_list_info {
  struct spinlock lock;
  struct run *freelist;
};

struct free_list_info kmems[NCPU];

void
kinit()
{
  for (int i = 0; i < NCPU; i++)
  {
    initlock(&kmems[i].lock, "kmem");
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree_init(p);
}

// only used by freerange while initializing
void
kfree_init(void *pa) {
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  r->next = kmems[0].freelist;
  kmems[0].freelist = r;
}


// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();
  int id = cpuid();
  acquire(&kmems[id].lock);
  pop_off();

  r->next = kmems[id].freelist;
  kmems[id].freelist = r;
  release(&kmems[id].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  push_off(); // 保证 kalloc 期间，id 始终指向当前进程的 CPU，不会因为 timer 切换到 其他 CPU，而导致 id 与 cpu 不一致

  int id = cpuid();

  acquire(&kmems[id].lock); // acquire 之后也不会切进程，同时阻止多 CPU 之间的竞态
  // 不用完全包裹，如果没找到，在进行 steal 的时候，不用关心当前运行的 cpu 是谁
  // 因此，在 release 之后，允许进程 切换到 其他 CPU 执行
  pop_off(); 
  

  struct run *r;
  r = kmems[id].freelist;
  if(r) {
    kmems[id].freelist = r->next;
    release(&kmems[id].lock);
  } else {
    // Steal：当前 freelist 没有空闲块，去访问其他 cpu 的 freelist
    release(&kmems[id].lock); // 避免嵌套 acquire lock，此时允许其他进程访问当前 freelist（查找空闲块）

    for (int i = 0; i < NCPU; i++)
    {
      if (i == id)
        continue;

      acquire(&kmems[i].lock);
      r = kmems[i].freelist;
      if (r) {
        kmems[i].freelist = kmems[i].freelist->next;
        release(&kmems[i].lock);
        break; // 找到空闲的 page
      }
      release(&kmems[i].lock); // 没找到空闲的块
    }
  }
  

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
