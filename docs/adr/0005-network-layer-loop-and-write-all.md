# ADR-005: 网络层形状、io::loop 组合子与 write_all 部分写语义

状态：已接受（M3）
范围：`include/iox/net/*`、`include/iox/loop.h`、`include/iox/blocking.h`

## 背景

M3 要把统一词汇扩展到网络（TCP/Unix/UDP）并交付 `ioxpump` 压轴示例。
三个决策在此固化：网络句柄如何暴露能力、循环如何在不堆分配的前提下组合
sender、以及流式写「部分完成」的正确语义。

## 决策

### 1. 网络句柄：能力由类型成员静态暴露

- `net::tcp::acceptor` 只提供 `accept_handle()`（只可 accept，不可读写）；
  `net::tcp::socket` 提供 `read_handle/write_handle/connect_handle`。
  词汇 CPO 按能力 concept 分派，握错操作在编译期失败。
- 所有 socket 类型声明 `static constexpr bool message_based = true`：
  `io::write` 据此选择 `IORING_OP_SEND`（带 `MSG_NOSIGNAL`）而非 `WRITE`，
  从根上消灭 SIGPIPE。文件/管道走 `WRITE`。同一 write sender 类型、
  运行期 `use_send` 标志分派——**类型决定策略，类型不膨胀**。
- `net::endpoint` 统一解析 `"1.2.3.4:80"`、`"[::1]:80"`、`"/path"`、
  `"unix:path"`、`"@abstract"`，句柄只收 `endpoint`，不收裸 sockaddr。
- UDP `send_to/recv_from` 是独立 CPO（带端点的收发），与流式 `read/write`
  分开——语义不同不共用签名。

### 2. io::loop / exec::detach：组合子的存储问题

- `io::loop(ctx, body)`：body 返回一个 sender of bool，完成 `true` 停止。
  重臂（re-arm）必须经过 `io::schedule` 跳转：在旧 child 的完成栈上直接
  析构/重建 child 不安全（完成回调还在栈上）。
- stdexec 适配器（then/let_value…）的 operation_state **不可移动**
  （move 构造未定义）。loop op 因此持有 `child_raw` 原始字节存储，
  每次重臂 placement-new——零堆分配、零虚调用。
- `exec::detach(sender)`：分离运行、自我删除的 op。会话生命周期模式：
  `new session` → `detach(loop(...) | upon_error(...) | then(delete sn))`
  ——完成与错误两条路径都保证回收。
- **陷阱记录**：loop body 里声明的局部变量（如 `bool eof`）在 body 返回时
  已析构，异步完成后经引用读它是 UB。跨迭代状态必须放进 loop lambda 的
  **捕获**（随 op 状态存活）。M3 的 echo 基准死锁即源于此，已修并在
  loop.h 注释警示。

### 3. write_all：部分写重发与单一 sender 类型

- 单次 `io::write` 语义 = 一次内核调用（SEND 可能短写）。**流式语义**
  （全部写完才算完成）由 `io::write_all` 提供：余量在 loop 内重发，
  偏移状态放 lambda 捕获，零堆分配、零共享计数。完成签名 `set_value()`
  （void），EOF 由**读侧**判定并经捕获的标志传出。
- 实现体放在**非模板 inline 函数** `detail::write_all_impl` 中：lambda
  闭包类型对任何句柄种类唯一，`write_all(file)` 与 `write_all(socket)`
  返回同一 sender 类型。这是 ioxpump 用 `std::variant` 通道做双端点
  分发的前提（无需类型擦除）。教训：定义在模板 operator() 内的 lambda
  即使捕获/函数体完全相同，每个实例化也是不同类型。

### 4. DNS 阻塞桥

`blocking_pool`（工作线程 + eventfd 唤醒 → 完成注入回 io_context）承载
`net::resolve(pool, host, service)`。任何阻塞函数都可经此桥变成 sender，
eventfd fd 由 ctx 生命周期持有。

## 后果

- 网络路径零虚调用/零分配目标维持（p50 21.5µs vs 裸 liburing 28.3µs）。
- write 的「短写」对用户不可见只在 write_all 层成立；裸 write 保留
  短写可见性（与 POSIX 语义一致，供需要者组合）。
- benchmark 教训：两个 acceptor 不能共存于同一端口（EADDRINUSE 后
  `*expected` 解引用是 UB）；对照实现与被测实现必须各自独立建听。
