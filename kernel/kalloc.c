// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

#define char_size (1 << 8) - 1 

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct spinlock km_refs_lock;

struct {
  // 存在两种并发场景
  // 同一 CPU 下，interrupt 带来的临界区访问
  // 不同 CPU 下，对共享对象的访问
  char arr[MAX_KM_SPACE / PGSIZE]; // 32768 个索引，只用于记录 [KERNBASE, PHYSTOP) 之间的物理页的引用
} km_refs;

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&km_refs_lock, "krefs");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
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

  int ref_index = pa_to_km_ref_index((uint64)pa);
  if (ref_index == -1)
    panic("kfree: ref_index"); // set illegal pa to free which does't belong to free list


  if ((km_refs.arr[ref_index] != 0)) {
    return; // there are still other processes to have this memory
  }

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
  if(r) {
    kmem.freelist = r->next;
  }
  release(&kmem.lock);


  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

uint64
kamountOfFreeB(void) {
  printf("--------------------------- kamountOfFreeB: ---------------------------\n");
  uint64 total_size = 0;
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  while (r)
  {
    // printf("kamountOfFreeB: %p\n", r);
    total_size+=PGSIZE;
    r = r->next;
  }
  release(&kmem.lock);

  return total_size;
}

int pa_to_km_ref_index(uint64 pa)
{
  if (pa < (uint64)end || pa >= PHYSTOP) {
    return -1; // 只对 [end, PHYSTOP) 中的 mem 更新 refs
  }
  if ((PGROUNDDOWN(pa) - KERNBASE) < 0) 
    panic("pa_to_km_ref_index");

  return (PGROUNDDOWN(pa) - KERNBASE) / PGSIZE;
}

void print_km_refs() 
{
  uint64 mem_start;
  printf("------------------ print_km_refs ------------------ \n");
  for (mem_start = (uint64)end; mem_start < PHYSTOP; mem_start+=PGSIZE)
  {
    int ref_index = pa_to_km_ref_index(mem_start);

    acquire(&km_refs_lock);

    int ref_count = km_refs.arr[ref_index];
    if (ref_count != 0)  {
      printf("- pa: %p index: %d count: %d\n", mem_start, ref_index, ref_count);
    }

    release(&km_refs_lock);
  }
}

void increment_km_ref(uint64 pa)  {
  int index = pa_to_km_ref_index(pa);
  if (index < 0)
    return;

  int ref_count = (int)(km_refs.arr[index]) + 1;
  if (ref_count > char_size)
    panic("increment_km_ref");
  km_refs.arr[index] = ref_count;

}

void decrease_km_ref(uint64 pa) {
  int index = pa_to_km_ref_index(pa);
  if (index < 0)
    return;

  int ref_count = (int)(km_refs.arr[index]) - 1;
  if (ref_count < 0)
    panic("decrease_km_ref");
  km_refs.arr[index] = ref_count;

}
