# iox API 速查（M6 后）

> 一个头拿全部词汇：`#include <iox/ops.h>`；执行层：
> `#include <iox/core/exec.h>`（`namespace ex = iox::exec`）。
> 详见各头文件 doc-comment 与 docs/adr/。

## 运行时

| | |
|---|---|
| `io_context ctx{uring::ring_params{...}}` | 单线程无锁事件循环；`sqe128=true` 供 uring_cmd 驱动 |
| `ctx.run()` / `ctx.run_for(d)` | 驱动到 stop / 到期限（可嵌套：内层不记账） |
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
| `io::splice(ctx, in_fd, out_fd, len, [off_in], [off_out], flags)` | `set_value(搬运字节数)` | |
| `io::tee(ctx, in_pipe, out_pipe, len)` | `set_value(复制字节数)` | |

核心原语：`io::schedule(ctx)`、`io::sleep_for(ctx, d)`、`io::sleep_until`。

**完成语义统一**：EOF=值 0；死端=类型化错误（EPIPE…，永不 SIGPIPE）；
取消=`set_stopped`（永不错误）；`when_all` 外部 stop→整体 stopped
（ADR-010 §4）。

## 组合子（`iox/compose/` + stdexec 全套经 `ex::`）

`io::loop(ctx, body→sender<bool>)`（循环体可取消）、`io::write_all(ctx, h, rbytes)`
（部分写重发）、`io::pump(ctx, src, dst, chunk)`（splice 零拷贝自动回退）、
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
  `xdp::umem`/`xdp::frame`；fd 挂载完成源

## 示例与基准索引

`timer_hello`（组合入门）/ `stdin_echo` / `file_copy`（注册内存）/ 
`echo_server`（detach 会话）/ `child`（子进程+pidfd）/ `eventfd_device`
（驱动教程 90 行）/ `ioxpump`（压轴：任意端点零拷贝搬运 + ^C 优雅退出）。
基准：`bench_submit_path` / `bench_file_rw` / `bench_echo` / `bench_splice` /
`bench_nvme`（无权限自跳过）。
