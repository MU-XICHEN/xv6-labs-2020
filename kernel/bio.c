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

struct buf* cached_buckets[BUCKET_NUM];
struct spinlock bucket_locks[BUCKET_NUM];
struct buf global_buf[NBUF];

struct {
  struct spinlock lock;
} bcache;

void add2new_bucket(struct buf *buf, uint64 target_buk_idx);

void
binit(void)
{
  struct buf *b;
  uint i;

  initlock(&bcache.lock, "bcache");

  for (int i = 0; i < BUCKET_NUM; i++)
  {
    initlock(&bucket_locks[i], "bcache_buckets");
  }

  for(i = 0, b = global_buf; b < global_buf+NBUF; b++, i++){
    initsleeplock(&b->lock, "buffer");
    uint idx = i % BUCKET_NUM;
    add2new_bucket(b, idx);
  }
}

void remove4old_ifneed(struct buf* buf) {

    int b_index = buf->bucket_index;

    struct buf* it_buf = cached_buckets[b_index];

    if (it_buf == 0)
      panic("remove4old_ifneed: empty bucket");

    if (it_buf == buf) {
      // 第一个就是，特殊处理
      if (it_buf->next) {
        it_buf->next->prev = 0;
      }
      cached_buckets[b_index] = it_buf->next;

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

void add2new_bucket(struct buf *buf, uint64 target_buk_idx) {
  buf->bucket_index = target_buk_idx;

  struct buf* it_buf = cached_buckets[target_buk_idx];

  if (it_buf == 0) {
    // 空桶
    cached_buckets[target_buk_idx] = buf;
  } else {
    buf->prev = 0;
    buf->next = cached_buckets[target_buk_idx];

    cached_buckets[target_buk_idx]->prev = buf;
    cached_buckets[target_buk_idx] = buf;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int target_buks_idx = blockno % BUCKET_NUM;

  acquire(&bcache.lock);

  b = cached_buckets[target_buks_idx];

  // Is the block already cached?
  while(b) {
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock);

      acquiresleep(&b->lock);
      return b;
    }
    b = b->next;
  }

  // Not cached.

  struct buf *new_b = 0;
  uint min_ticks = 10000;

  for(b = global_buf; b < global_buf+NBUF; b++){
    if(b->refcnt == 0) {
      if (b->ticks < min_ticks) {
        new_b = b;
        min_ticks = b->ticks;
      }
    }
  }


  if (new_b) {
    /**
     * 情况 1：加入到了其他桶；
     * 情况 2：已经存在于当前桶中
     */
    if (new_b->blockno != 0) {
      uint old_b_index = new_b->bucket_index;
      if (old_b_index != target_buks_idx) {               // 情况 1：现在在其他桶中
        // remove
        remove4old_ifneed(new_b);
        
        // add
        add2new_bucket(new_b, target_buks_idx);
      }
      // 情况 2：已经存在于当前桶中
    }

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


