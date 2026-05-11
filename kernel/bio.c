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
    buf->bucket_index = 0;

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

static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int target_idx = blockno % BUCKET_NUM;

  // --- 阶段 1: 在目标桶中查找 ---
  acquire(&bucket_locks[target_idx]);

  b = cached_buckets[target_idx];
  while(b) {
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bucket_locks[target_idx]);
      acquiresleep(&b->lock);
      return b;
    }
    b = b->next;
  }
  
  // 目标桶没找到，准备驱逐。释放桶锁，避免死锁
  release(&bucket_locks[target_idx]);

  // --- 阶段 2: 驱逐流程 ---
  // 这里使用全局锁来进行加锁序列化，避免 A -> B 以及 B -> A 导致死锁，通过全局锁，可以保证 A -> B 和 B -> A 完整先后进行
  acquire(&bcache.lock);

  // ❗❗❗拿回目标桶锁后，必须再次检查，防止在释放锁期间其他 CPU 已经加载了该块
  acquire(&bucket_locks[target_idx]);
  b = cached_buckets[target_idx];
  while(b) {
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bucket_locks[target_idx]);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
    b = b->next;
  }

  struct buf *new_b = 0;
  uint min_ticks = 0xffffffff;

  // 全局寻找最久未使用的 buf
  for(b = global_buf; b < global_buf+NBUF; b++){
    // 这里对 refcnt 的检查受 bcache.lock 保护，虽然不完美但能跑通
    if(b->refcnt == 0) {
      if (b->ticks < min_ticks) {
        new_b = b;
        min_ticks = b->ticks;
      }
    }
  }

  if (new_b) {
    uint old_idx = new_b->bucket_index;

    if (old_idx != target_idx) {
      // 跨桶迁移：必须持有旧桶锁
      acquire(&bucket_locks[old_idx]);
      remove4old_ifneed(new_b); // 从旧桶移除
      release(&bucket_locks[old_idx]);
      
      add2new_bucket(new_b, target_idx); // 加入新桶
    } else {
      // 如果 new_b 就在当前桶，只需修改身份，无需移动链表
    }

    new_b->dev = dev;
    new_b->blockno = blockno;
    new_b->valid = 0;
    new_b->refcnt = 1;

    release(&bucket_locks[target_idx]);
    release(&bcache.lock);

    acquiresleep(&new_b->lock);
    return new_b;
  }
  
  release(&bucket_locks[target_idx]);
  release(&bcache.lock);
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

void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  // 为什么对 bucket_index 的访问不用加锁? 
  // 只有在 refcnt == 0 的时候才有进程来驱逐 b，使其转向到另一个桶
  // 而在访问 brelse 时，表示 b 当前至少被一个宿主持有，因此可以安全访问
  uint idx = b->bucket_index;
  acquire(&bucket_locks[idx]);
  b->refcnt--;
  if (b->refcnt == 0) {
    b->ticks++; 
  }
  release(&bucket_locks[idx]);
}

void
bpin(struct buf *b) {
  uint idx = b->bucket_index;
  acquire(&bucket_locks[idx]);
  b->refcnt++;
  release(&bucket_locks[idx]);
}

void
bunpin(struct buf *b) {
  uint idx = b->bucket_index;
  acquire(&bucket_locks[idx]);
  b->refcnt--;
  release(&bucket_locks[idx]);
}