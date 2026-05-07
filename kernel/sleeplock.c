// Sleeping locks

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"

void
initsleeplock(struct sleeplock *lk, char *name)
{
  initlock(&lk->lk, "sleep lock");
  lk->name = name;
  lk->locked = 0;
  lk->pid = 0;
}

void
acquiresleep(struct sleeplock *lk)
{
  // 用 spinlock 来保护 sleeplock 中的状态
  acquire(&lk->lk); // 同时获取这个 sleeplock 的时候，使得其他进程自旋
  while (lk->locked) { 
    sleep(lk, &lk->lk); //没有获得这个锁的进程，则进行睡眠，同时释放自旋锁
  }
  lk->locked = 1; // 先获得自旋锁的进程获得该 sleeplock
  lk->pid = myproc()->pid; // 同上
  release(&lk->lk);
}

void
releasesleep(struct sleeplock *lk)
{
  acquire(&lk->lk);
  lk->locked = 0;
  lk->pid = 0;
  wakeup(lk);
  release(&lk->lk);
}

int
holdingsleep(struct sleeplock *lk)
{
  int r;
  
  acquire(&lk->lk);
  r = lk->locked && (lk->pid == myproc()->pid);
  release(&lk->lk);
  return r;
}



