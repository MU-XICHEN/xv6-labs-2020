## File System

磁盘布局映射 fs.img 中的布局信息，fs.img 在 make 的过程中由 mkfs 进行创建
之后在 xv6 启动的时候，fs.img 就会挂载到 xv6 中

Disk layout:
[  boot block | super block | log | inode blocks | free bit map | data blocks]

### Buffer Cache
Buffer cache 回收最近最少使用的 buffer (LRU)

### Logging layer
bitmap、inode、directory 内容都存在磁盘的不同 blocks 区域内  

设想一个 File truncation 过程中发生崩溃，此时理应存在两步骤：  
(1) 将 inode 的引用减少  
(2) 更新 bitmap 
这两步的执行顺序决定了最终的结果，如果是 (1) -> (2) 导致磁盘空间泄露，后续的其他 inode 也无法引用这个 block；如果是 (2) -> (1)，后续另一个 inode 有机会指向这个 block，此时两个 inode 就指向了同一个 block

#### log 更新 blocks 流程

注意区分，log blocks 和普通 data blocks 的区别，普通 data blocks 在写入磁盘时会经过 log 的过程，而 log blocks 是直接通过 log_write 绕过 log 直接更新

##### 第一步：内存里的“草稿”（In-memory Buffer）
当系统调用（比如 write）想要修改磁盘上的某个块（例如 Block 500）时：

它首先把 Block 500 读入内存。

在内存中完成修改。此时，磁盘上的 Block 500 还是旧数据。

这些被修改过的内存块被标记为“脏（Dirty）”，但它们绝不允许直接写回磁盘的 Block 500。

##### 第二步：写入日志区（The Logging Step）
日志系统把内存中那几个“脏块”的内容，原封不动地写入磁盘的日志专用区（比如日志区的第 1、2、3 号槽位）。

目的：先在磁盘上存一份“证据”，证明我们打算改哪些块，改成了什么样。

注意：此时，磁盘原位的 Block 500 依然没变，count 依然是 0。

##### 第三步：盖章生效（The Commit）
[❗这一步之前发生崩溃，下次计算机重启时，会发现 count 还是0，因此不会向磁盘写入，相当于这一次事务并没有执行]

当系统调用完成所有修改逻辑后，它会发起“提交”请求。

日志系统写入 Header Block，把 count 改为块的数量（比如 3）。

意义：这一步是瞬间完成的。一旦 count 写入成功，就意味着这一组 Log Blocks 正式从“草稿”变成了“法律命令”。

##### 第四步：安装到原位（The Install）
既然有了“法律命令”，接下来就是执行：

系统把日志区里的 Log Blocks 逐一拷贝到它们在磁盘上的真实目的地（比如搬回 Block 500）。

如果搬运过程中断电了也没关系，因为 Header Block 里的 count 还在，重启后可以重新搬运。

##### 第五步：擦除记录（The Completion）
当所有的块都安全搬回原位后：

系统将 Header Block 的 count 改回 0。

此时，日志区的 Log Blocks 完成了使命，它们占据的空间现在可以被下一个事务使用了。

#### 内存 到 disk 的更新过程表
动作阶段,内存状态 (Buffer Cache),磁盘日志区状态,磁盘原始位置状态

系统调用修改中,数据已变 (Dirty),旧/无数据,旧数据  
写 Log Blocks,数据已变,得到内存副本,旧数据  
写 Header (Commit),数据已变,已记录 + Count>0,旧数据  
安装 (Install),数据已变,已记录,变为新数据  
完成 (Clean),数据已变,已失效 (Count=0),新数据  


#### 为什么需要将操作封装成一个 transaction (事务)
举例，创建一个文件涉及：
- 分配一个 Inode  
- 在目录块中写入文件名  
- 更新位图（Bitmap）  
这三步操作必须属于同一个事务。要么全部写入磁盘，要么一个都不写。

### Inode layer
