# iox API 速查（M6 后）

> 一个头拿全部词汇：`#include <iox/ops.h>`；执行层：
> `#include <iox/core/exec.h>`（`namespace ex = iox::exec`）。
> 详见各头文件 doc-comment 与 docs/adr/。

## 运行时

| | |
|---|---|
| `io_context ctx{uring::ring_params{...}}` | 单线程无锁事件循环；`sqe128=true` 供 uring_cmd 驱动 |
| `ctx.run()` / `ctx.run_for(d)` | 驱动到 stop / 到期限（可嵌套：内层不记账）；入口均自重启（清 stop 态）；`run_for` 返回 `iox::error`——活 `batch_scope` 内→`EDEADLK`；`!ctx.ok()`（ring 初始化失败）时立即返回 |
| `ex::sync_wait(ctx, sender)` | 驱动到真完成；结果 `{value, error, stopped}`；活 `batch_scope` 内→`EDEADLK` |
| `ex::sync_wait(ctx, stop_src, sender)` | 外部 stop source（取消入口） |
| `batch_scope scope{ctx}` | RAII 批量：退出时一次 `io_uring_enter`；批内 SQE 执行顺序任意，依赖操作（write→fsync→close）须经完成链表达 |
| `ctx.arm_failpoint(n, -errno)` | 测试故障注入：第 n 次提交内联失败 |

## 词汇操作（19 个 CPO，皆可 tag_invoke 定制）

| 操作 | 完成 | 备注 |
|---|---|---|
| `io::read(ctx, h, wbytes)` / `(…, registered_buffer&)` | `set_value(字节数)`；0=EOF | 读句柄 |
| `io::write(ctx, h, rbytes)` / `(…, registered_buffer&)` | `set_value(字节数)` | socket 自动 SEND\|MSG_NOSIGNAL；0 长度即值 0 |
| `io::read_at / write_at(ctx, h, buf, uoffset_t)` | `set_value(字节数)` | 强类型偏移；哨兵→EOVERFLOW/EINVAL 前置 |
| `io::open(ctx, path, fs::mode)` | `set_value(fs::file)` | OPENAT；路径归调用方存活 |
| `io::close(ctx, h)` / `(ctx, std::move(h))` | `set_value()` | 借用式 / 持有式 |
| `io::poll(ctx, fd, events)` | `set_value(revents)` | 就绪等待；完成源桥的基石 |
| `io::fsync(ctx, h)` | `set_value()` | nvme 设备上=FLUSH 直通 |
| `io::accept(ctx, acceptor)` | `set_value(该类型的 socket)` | |
| `io::connect(ctx, sock, endpoint)` | `set_value()` | |
| `io::send_to(ctx, udp, rbytes, ep)` / `io::recv_from(ctx, udp, wbytes)` | `n` / `(n, endpoint)` | |
| `io::signal(ctx, signal::watcher)` | `set_value(消费的 siginfo)` | |
| `io::wait_pid(ctx, process)` | `set_value(exit_status)` | pidfd 收割 |
| `io::splice(ctx, in_fd, out_fd, len, [off_in], [off_out], flags)` | `set_value(搬运字节数)` | len>4GiB→EOVERFLOW 前置，绝不静默少搬 |
| `io::tee(ctx, in_pipe, out_pipe, len)` | `set_value(复制字节数)` | 同上，len>4GiB→EOVERFLOW 前置 |

核心原语：`io::schedule(ctx)`、`io::sleep_for(ctx, d)`、`io::sleep_until`
（负时长/过去时刻视为已到期，立即完成）。

**完成语义统一**：EOF=值 0；死端=类型化错误（EPIPE…，永不 SIGPIPE）；
取消=`set_stopped`（永不错误）；`when_all` 外部 stop→整体 stopped
（ADR-010 §4）。**线程契约**：事件循环单线程，但有两个跨线程安全入口——
`request_stop`（stop token 触发的取消）与 `ctx.stop()`：外部线程调用时经
eventfd 收件箱转交 io 线程执行（TSan 实证干净）。此外 `ctx.post(fn, arg)`
是公开的跨线程投递口：把 `fn(ctx, arg)` 排到 io 线程上下一次排空点执行。
其余入口——`attach/detach_source`、`dispatch`、sender 的 `connect/start`——
仍必须在 io 线程上发起。

## 组合子（`iox/compose/` + stdexec 全套经 `ex::`）

`io::loop(ctx, body→sender<bool>)`（循环体可取消）、`io::write_all(ctx, h, rbytes)`
（部分写重发；恒零进展写→EIO 而非死循环）、`io::pump(ctx, src, dst, chunk)`
（splice 零拷贝；chunk 自动钳到 bounce 管真实容量）、
`ex::detach(s)`（自管生命周期）、`ex::then/let_value/let_error/upon_stopped/
when_all/upon_error…`。

## 句柄（能力即类型：readable/writable/seekable/acceptable/connectable/datagram）

`fs::file`、`fs::watcher`(inotify) + `fs::event_range`、`pipe::pair{r,w}`、
`std_in/out/err`、`net::tcp::{acceptor,socket}`、`net::unix_dom::{acceptor,
socket,pair}`、`net::udp::socket`、`net::endpoint`（统一解析）、
`signal::set`+`signal::watcher`、`process::process`（spawn+pidfd）、
裸 `iox::fd`（逃生口）、DNS `net::resolve(pool, host, service)`。

## 驱动面（M5 SPI，docs/driver_guide.md）

`iox::completion_source`（fd 挂载 / 忙槽 / 主动注入）、`ctx.attach_source/
detach_source`、`io::supports(zero_copy/mmap/dma, h)`、
`iox::driver::registered_driver<D>`、定制 = 句柄旁 `tag_invoke`。

## 参考驱动（M6）

- `nvme::device`（/dev/ngXnY，uring_cmd）：`read_at/write_at`（字节偏移→LBA）、
  `fsync`=FLUSH、`nvme::admin`（CAP_SYS_ADMIN 逃生口）；需 `sqe128` 环
- `xdp::socket`（AF_XDP）：`xdp::write_frame/read`（copy-mode TX 无需 BPF）、
  `xdp::umem`/`xdp::frame`；fd 挂载完成源。chunk 单一池：`create` 时借一半给
  fill ring，回收时自动回填，RX 不饿死、TX 不死锁；`tx_frame()` 池空时返回
  `std::nullopt`（等下一次完成回收后重试）；socket 销毁时挂起的 rx/tx 操作
  以 ENODEV 经 `set_error` 排空；op 级 stop token 取消尚未接线（顺延）

## Python 面（python/，tests/test_python*.py）

双表面。**同步面**（每线程一个 `Context`，等待时释放 GIL）：
`open_file`（`read_at/write_at/read/write/fsync/read_into/read_at_many`）、
`connect/listen`（`send/recv/recv_into`）、`sleep`；send/write 零拷贝
（直接提交 bytes 内存），`read_at_many` 一次提交 N 个定位读。
**asyncio 面**：`AsyncContext`（内建一个 io 线程，进程内任意 asyncio loop
均可使用），方法返回可 await 的 future——`AsyncFile.aread/awrite/
aread_at/awrite_at/aread_at_many/afsync/aclose`、`AsyncTcpSocket.asend/
arecv/arecv_into/aclose`、`AsyncListener.aaccept`、`await actx.connect/
actx.sleep`；任务取消经 stop token 直达 ASYNC_CANCEL，`close()` 自动强停
并排空在途操作（等待方收 CancelledError，退出零泄漏）。完成经
`call_soon_threadsafe` 编组回 loop 线程。

## 示例与基准索引

`timer_hello`（组合入门）/ `stdin_echo` / `file_copy`（注册内存）/ 
`echo_server`（detach 会话）/ `child`（子进程+pidfd）/ `eventfd_device`
（驱动教程 90 行）/ `ioxpump`（压轴：任意端点零拷贝搬运 + ^C 优雅退出）。
Python：`kv_cache`（分层 KV 池 + 远端节点）/ `echo_server` / 
`async_echo_server`（asyncio 并发回声）。
基准：`bench_submit_path` / `bench_file_rw` / `bench_echo` / `bench_splice` /
`bench_nvme`（无权限自跳过）。
