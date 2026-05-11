// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define BUCKET_NUM          13

struct buf* buckets[BUCKET_NUM];
struct spinlock bucket_locks[BUCKET_NUM];
struct buf global_buf[NBUF];

struct {
  struct spinlock lock;
} bcache;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  for (int i = 0; i < BUCKET_NUM; i++)
  {
    initlock(&bucket_locks[i], "bcache_buckets");
  }

  for(b = global_buf; b < global_buf+NBUF; b++){
    initsleeplock(&b->lock, "buffer");
  }
}

void remove4old_ifneed(struct buf* buf) {

    int b_index = buf->blockno % BUCKET_NUM;

    struct buf* it_buf = buckets[b_index];

    if (it_buf == 0)
      panic("remove4old_ifneed: empty bucket");

    buf->dev = 0;
    buf->blockno = 0;

    if (it_buf == buf) {
      // 第一个就是，特殊处理
      if (it_buf->next) {
        it_buf->next->prev = 0;
      }
      buckets[b_index] = it_buf->next;

      it_buf->prev = 0;
      it_buf->next = 0;
      return;
    }

    // 从第二个开始是，则从链表中移除
    it_buf = it_buf->next;
    while (it_buf)
    {
      if (it_buf == buf) {
        if (it_buf->next) {
          it_buf->next->prev = it_buf->prev;
        }
        it_buf->prev->next = it_buf->next;

        it_buf->prev = 0;
        it_buf->next = 0;
        return;
      }
      it_buf = it_buf->next;
    }

    panic("remove4old_ifneed: not exist");
}

void add2new_bucket(struct buf *buf) {
  int b_index = buf->blockno % BUCKET_NUM;
  struct buf* it_buf = buckets[b_index];

  if (it_buf == 0) {
    // 空桶
    buckets[b_index] = buf;
  } else {
    buf->prev = 0;
    buf->next = buckets[b_index];

    buckets[b_index]->prev = buf;
    buckets[b_index] = buf;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  // 这个锁保证了 检查是否存在 和 如果不存在则占坑 这两个动作的不变性
  // bget 试图获取扇区号 blockno 对应的内容
  // 如果两个进程都在获取 blockno 2 的扇区内容
  // 第一个进程在查找完后不存在对应的 buffer 时，直接 release
  // 在占位 acquire lock 之前，另一个进程也调用了 bget
  // 因为没有被第一个进程占位，所以 扇区 2 对应的 buffer 也不存
  // 然后进程 2 进行了占位，之后返回给 进程 1，由于进程 1 已经结束查找过程
  // 所以又会进行新的展位，导致扇区 2 在 buffer 中存在两份
  // 综上，bcache lock 保证了两个动作的不变性
  // bcache.lock 保护了对 blocks 对象的缓存
  // block->lock 保护了对 block content 的读写
  acquire(&bcache.lock);

  // Is the block already cached?
  for(b = global_buf; b < global_buf+NBUF; b++){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  struct buf *new_b = 0;
  uint min_ticks = 10000;

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = global_buf; b < global_buf+NBUF; b++){
    if(b->refcnt == 0) {
      if (b->ticks < min_ticks) {
        new_b = b;
        min_ticks = b->ticks;
      }
    }
  }

  if (new_b) {
    new_b->dev = dev;
    new_b->blockno = blockno;
    new_b->valid = 0;
    new_b->refcnt = 1;
    release(&bcache.lock);

    acquiresleep(&new_b->lock);
    return new_b;
  }
  
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->ticks++;
  }
  
  release(&bcache.lock);
}

void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}


