#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern char etext[];
extern void forkret(void);
static void wakeup1(struct proc *chan);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// initialize the proc table at boot time.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");

      // Allocate a page for the process's kernel stack.
      // Map it high in memory, followed by an invalid
      // guard page.

      // proc 本身的内容存在于 kernel data 段中
      // 每个进程的内核栈存在于新分配的 free page 中
      // 因此是这个 free page 在 kernel_pagetable 中存在映射
      // 这样，在进程进入内核态的时候，可以通过 kstack 存的内容，访问到实际的page中
      // 当前，内核态中的映射关系统一通过 kernel_pagetable 来访问
      // 后续，当每个进程拥有自己的 内核 pagetable 的时候，意味着进入内核态会使用该进程提供的 kernel pagetable
      // 因此也要保证，通过这个 pagetable 可以访问到内核栈的物理内存
      char *pa = kalloc();
      if(pa == 0)
        panic("kalloc");
      uint64 va = KSTACK((int) (p - proc));
      kvmmap(va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
      p->kstack = va; // kstack 存的是以 kernel_pagetable 为映射关系的虚拟地址
  }

  // 这里还没有进程运行，但是因为之前 kvminithart 的执行，到procinit 期间，TLB 已经加载了 PT 缓存了
  // 上述部分因为实际 VA 和 PA 是一致的，缓存不更新也不会出问题
  // 但是后续，会访问 TRAMPOLINE 以及调度用户进程，因此就要更新了
  kvminithart();
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() {
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  // An kernel page table (identical to the existing global kernel page table)
  // todo：疑问-可以直接使用 p->trapframe->kernel_satp 吗
  p->kernel_pagetable = proc_kernel_pagetable(p);
  
  if(p->pagetable == 0 || p->kernel_pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  if (p->kernel_pagetable)
    proc_free_kernel_pagetable(p->kernel_pagetable, p->sz, p->kstack);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

struct va_info {
  uint64 va;
  uint64 pg_sz;
};

void free_pg_arr(pagetable_t pagetable, struct va_info *arr, int len) {
  int i = 0;
  while (i < len)
  {
    uvmunmap(pagetable, arr[i].va, PGROUNDUP(arr[i].pg_sz) / PGSIZE, 0);
    i++;
  }
  uvmfree_onlyPTEs(pagetable, 0);
}

void set_va_info(struct va_info *info, uint64 va, uint64 sz) {
  info->va = va;
  info->pg_sz = sz;
}

pagetable_t proc_kernel_pagetable(struct proc *p) {
  pagetable_t pagetable;
  struct va_info va_valid_arr[8] = {0};
  int i = 0;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

   // uart registers
   if (uvm_kernel_map(pagetable, UART0, UART0, PGSIZE, PTE_R | PTE_W) != 0) {
    free_pg_arr(pagetable, va_valid_arr, i);
    return 0;
   } else {
    set_va_info(&(va_valid_arr[i++]), UART0, PGSIZE);
   }

  // virtio mmio disk interface
  if (uvm_kernel_map(pagetable, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W) != 0) {
    free_pg_arr(pagetable, va_valid_arr, i);
    return 0;
   } else {
    set_va_info(&(va_valid_arr[i++]), VIRTIO0, PGSIZE);
   }
    
  // // CLINT
  // uvm_kernel_map(pagetable, CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  if (uvm_kernel_map(pagetable, PLIC, PLIC, 0x400000, PTE_R | PTE_W) != 0) {
    free_pg_arr(pagetable, va_valid_arr, i);
    return 0;
   } else {
    set_va_info(&(va_valid_arr[i++]), PLIC, 0x400000);
   }

  // map kernel text executable and read-only.
  if (uvm_kernel_map(pagetable, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R |  PTE_X) != 0) {
    free_pg_arr(pagetable, va_valid_arr, i);
    return 0;
   } else {
    set_va_info(&(va_valid_arr[i++]), KERNBASE, (uint64)etext-KERNBASE);
   }

  // map kernel data and the physical RAM we'll make use of.
  if (uvm_kernel_map(pagetable, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W) != 0) {
    free_pg_arr(pagetable, va_valid_arr, i);
    return 0;
   } else {
    set_va_info(&(va_valid_arr[i++]), (uint64)etext, PHYSTOP-(uint64)etext);
   }

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  if (uvm_kernel_map(pagetable, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X) != 0) {
    free_pg_arr(pagetable, va_valid_arr, i);
    return 0;
   } else {
    set_va_info(&(va_valid_arr[i++]), TRAMPOLINE, PGSIZE);
   }

  // kstack 更新，更新为以进程自身的 kernel pagetable 为映射关系的 va
  uint64 stack_va = KSTACK((int) (p - proc));
  uint64 pa = kvmpa(stack_va); // 在 procinit 的时候，kstack 已经分配了物理页

  if (uvm_kernel_map(pagetable, stack_va, (uint64)pa, PGSIZE, PTE_R | PTE_W) != 0) {
    free_pg_arr(pagetable, va_valid_arr, i);
    return 0;
  } else {
    set_va_info(&(va_valid_arr[i++]), stack_va, PGSIZE);
  }

  return pagetable;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}


// free kernel page tables
void
proc_free_kernel_pagetable(pagetable_t k_pagetable, uint64 sz, uint64 kstack_va) {
  // 主要是 free 页表占用的 page memory，最终指向的内容不释放，uvmfree 中确保所有映射都取消后，统一全部释放
  uvmunmap(k_pagetable, UART0, 1, 0);
  uvmunmap(k_pagetable, VIRTIO0, 1, 0);
  // uvmunmap(k_pagetable, CLINT, 0x10000 / PGSIZE, 0);
  uvmunmap(k_pagetable, PLIC, 0x400000 / PGSIZE, 0);
  uvmunmap(k_pagetable, KERNBASE, PGROUNDUP((uint64)etext-KERNBASE) / PGSIZE, 0);
  uvmunmap(k_pagetable, (uint64)etext, PGROUNDUP(PHYSTOP-(uint64)etext) / PGSIZE, 0);
  uvmunmap(k_pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(k_pagetable, kstack_va, 1, 0);

  uvmfree_onlyPTEs(k_pagetable, sz); // 回收 PTEs
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  // 用户进程所处于的地址空间在 PHYSTOP 到 kernel 结束之间，用户的实际物理页分散在这一片区域中，被 free page list 进行管理
  // sz 指定的是用户最高已用的虚拟地址的上边界
  // freewalk 需要保证 All leaf mappings must already have been removed. 否则会 panic
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
// 只是初始化了一个用户进程（执行内容是 initCode 中的内容），此时并不会运行，因为还没有被 kernel 调度
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p, initcode, sizeof(initcode));

  p->sz = PGSIZE; // free 的时候记得从0 的位置 free 掉 sz 大小的page和对应 memory

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, p->kernel_pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, p->kernel_pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){ // 此时 np 的 pagetable 还是空的，kernel pt 也因为 pt 是空的，所以没有映射进程代码段
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }

  np->sz = p->sz;

  np->parent = p;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  np->state = RUNNABLE;

  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold p->lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    // this code uses pp->parent without holding pp->lock.
    // acquiring the lock first could cause a deadlock
    // if pp or a child of pp were also in exit()
    // and about to try to lock p.
    if(pp->parent == p){
      // pp->parent can't change between the check and the acquire()
      // because only the parent changes it, and we're the parent.
      acquire(&pp->lock);
      pp->parent = initproc;
      // we should wake up init here, but that would require
      // initproc->lock, which would be a deadlock, since we hold
      // the lock on one of init's children (pp). this is why
      // exit() always wakes init (before acquiring any locks).
      release(&pp->lock);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  // we might re-parent a child to init. we can't be precise about
  // waking up init, since we can't acquire its lock once we've
  // acquired any other proc lock. so wake up init whether that's
  // necessary or not. init may miss this wakeup, but that seems
  // harmless.
  acquire(&initproc->lock);
  wakeup1(initproc);
  release(&initproc->lock);

  // grab a copy of p->parent, to ensure that we unlock the same
  // parent we locked. in case our parent gives us away to init while
  // we're waiting for the parent lock. we may then race with an
  // exiting parent, but the result will be a harmless spurious wakeup
  // to a dead or wrong process; proc structs are never re-allocated
  // as anything else.
  acquire(&p->lock);
  struct proc *original_parent = p->parent;
  release(&p->lock);
  
  // we need the parent's lock in order to wake it up from wait().
  // the parent-then-child rule says we have to lock it first.
  acquire(&original_parent->lock);

  acquire(&p->lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup1(original_parent);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&original_parent->lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  // hold p->lock for the whole time to avoid lost
  // wakeups from a child's exit().
  acquire(&p->lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      // this code uses np->parent without holding np->lock.
      // acquiring the lock first would cause a deadlock,
      // since np might be an ancestor, and we already hold p->lock.
      if(np->parent == p){
        // np->parent can't change between the check and the acquire()
        // because only the parent changes it, and we're the parent.
        acquire(&np->lock);
        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&p->lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&p->lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&p->lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &p->lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    
    int found = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);

      // 循环持续的消费 RUNNABLE，直到 RUNNING 的进程切回来之后都不再进入 RUNNABLE
      // 当遍历一圈之后发现没有 RUNNABLE 进程的时候，意味着所有都处于 UNUSED、SLEEPING、ZOMBIE
      // （不可能进入这里的时候有进程处于 RUNNING）
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;


        // 注意，这里并不是直接切到用户态去执行内容，只是切换到了另一个用户进程的内核态
        // 只有当进程进入内核态之后，才可能发生切换
        /**
         * 进程被切换的场景：
         * 1、主动让出 CPU（yield / sleep）
         * 2、时间片用完（timer interrupt）：timer interrupt → trap → kernel → yield → scheduler
         * ...
         * 以上最终都会通过 sched 切换回当前，意味着进程都进入了自己的内核态
         */

        // 切换为下一个进程的内核页表，然后再去恢复其上下文，继续以内核态运行
        // 这里是内核调度导致的切换内核页表，swtch 之后直接进入对应进程的内核态
        w_satp(MAKE_SATP(p->kernel_pagetable)); 
        sfence_vma();

        swtch(&c->context, &p->context); // 从这里回来的时候，此时就没有 RUNNING 的进程了

        // restore to kernel_pagetable
        // 意味着，用户进程处于 RUNNING 态的时候，用户进程进入内核态会切换成自己的内核页表
        // 但是当进程变化状态后，通过 sched swtch 回 scheduler 的时候，就会统一使用 kernel_pagetable 了
        kvminithart();

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;

        found = 1;
      }
      release(&p->lock);
    }
#if !defined (LAB_FS)
    if(found == 0) {
      intr_on();
      asm volatile("wfi");
    }
#else
    ;
#endif
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.
  if(lk != &p->lock){  //DOC: sleeplock0
    acquire(&p->lock);  //DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &p->lock){
    release(&p->lock);
    acquire(lk);
  }
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == SLEEPING && p->chan == chan) {
      p->state = RUNNABLE;
    }
    release(&p->lock);
  }
}

// Wake up p if it is sleeping in wait(); used by exit().
// Caller must hold p->lock.
static void
wakeup1(struct proc *p)
{
  if(!holding(&p->lock))
    panic("wakeup1");
  if(p->chan == p && p->state == SLEEPING) {
    p->state = RUNNABLE;
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
