# ADR-004: 类型化句柄层与取消链路（M2）

- 状态：已接受（M2）
- 范围：能力概念、域句柄、注册内存、stop_token 取消、基准方法论

## 决策

### 1. 能力即类型（concepts 驱动词汇分派）

句柄以结构化成员声明能力：`read_handle()/write_handle()` → `io::readable/
writable`，`is_seekable()` → `io::seekable`，`fd_slot()` → 可 `io::close`。
CPO 的 requires 子句据此选择重载。效果（全部编译期）：

- `io::read(ctx, pipe_write_end, buf)` 无匹配重载 → 编译错误；
- `pipe::read_end/write_end` 是两个类型——「读写端拿反」从运行期 EBADF
  变成编译期事实；
- `uoffset_t/io_size_t` newtype：偏移与大小不可互换传参。

`tests/test_capabilities.cc` 用 void_t 检测惯用法把这些断言固化为
static_assert 契约（注：GCC 15 在 requires 表达式体内把"候选全被约束排除"
当硬错误而非软失败，因此否定性检测走 void_t 而非 requires）。

### 2. 注册内存：类型驱动的零拷贝路径

`buffer_pool` 一次性注册整块对齐内存（`io_uring_register_buffers`，每 ring
一张表），`take()` 返回携带表索引的 `registered_buffer`。词汇层按**参数类型**
选择路径：`registered_buffer` → `IORING_OP_*_FIXED`（内核直接引用 pinned
页），普通视图 → 常规操作。零拷贝与否不需要运行期标志位。

### 3. 取消：stop_token → ASYNC_CANCEL → set_stopped

- op 在 `start()` 时查询 receiver env 的 stop token；`stdexec::inplace_stop_token`
  可停止则登记回调：`submit_cancel(user_data)` 提交 `IORING_OP_ASYNC_CANCEL`。
- 取消请求自身的 CQE 生命周期由 io_context 的**收据池**持有（64 个常驻
  receipt），杜绝"目标 op 已完成而 cancel CQE 后到"的悬垂 user_data。
- `-ECANCELED → set_stopped()`：取消永不表现为错误。
- **已在 start 前请求停止**的操作同步完成 set_stopped，不进内核（否则
  cancel 可能赶在目标提交之前，导致漏取消）。
- 停止回调存于 op 内的**原始字节槽**（`stop_cb_slot`）：inplace_stop_callback
  不可移动，直接做成员会连累整个 op_state 不可移动、破坏 connect 期放置；
  字节槽让 op 在 start 前保持可移动、start 后构造回调，恰好匹配 P2300 生命周期。

### 4. 性能：方法论教训与最终数字

M2 基准过程中修掉的两个真实开销与两个测量偏差：

真实开销：
1. `ring::flush()` 在 SQ 空时也发 `io_uring_enter`——运行循环每轮白付一次
   syscall（实测 12,000 enter / 4,096 op）。修复：空队跳过。
2. 运行循环顶部抢先 flush 会把"完成一个→武装一个"塌缩成每操作一次 enter。
   修复：先 drain 全部就绪完成、无事可做时才 submit+wait——每次 enter 自然
   携带批量（enter 次数降 ~3 倍）。

测量偏差（教训，已固化在基准代码注释里）：
3. tmpfs 首写要分配页——先跑的一方吃全部首触成本。修复：不计时的预热 pass。
4. iox 若每 64 op 做全屏障而 raw 是滚动流水——结构性不公平。修复：iox 改为
   完成回调内补位的滚动窗口（这也是真实服务的形态）。

最终（256MiB / 64KiB / QD64，vs 裸 liburing）：**写 97~101%，读 95~102%**。
nop 风暴微基准 ~70%（绝对开销 +33ns/op，来自取消槽与能力分派的结构成本；
预算"<100ns/op"实际 ~110ns，略超，如实记录——真实 IO 场景无影响，后续可用
更瘦的 op 布局回收）。

## 后果

- io::close 完成前句柄必须存活（P2300 op 生命周期已保证常见用法）；
  析构兜底 `::close` 保证异常路径不泄漏。
- 每 ring 一张注册表：一个 io_context 一个 buffer_pool（内核语义，M3+ 若需
  多池则引入表分段）。
- 取消目前覆盖词汇全部操作；阻塞读的取消依赖内核对该 op 的 cancel 支持
  （pipe/socket poll/read 均支持）。
