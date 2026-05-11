struct buf {
  // lock 只保护 data 的读写
  struct sleeplock lock;
  uchar data[BSIZE];

  // 以下内容由 bcache lock 保护
  // 以下内容表示 buffer cache 状态的全局一致性
  uint ticks;
  uint bucket_index;
  int disk;    // does disk "own" buf?
  uint dev;
  int valid;   // has data been read from disk?
  uint blockno;
  uint refcnt;
  struct buf *prev; // LRU cache list
  struct buf *next;
};

