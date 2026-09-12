# why iox — 同一件事的两种写法

> 对照实验：一个 TCP echo 会话（读到什么回什么，EOF 即退，错误清理，不许
> SIGPIPE 杀进程）。左边是裸 liburing，右边是 iox。两份都能跑；左边的完整
> 版本在 `bench/echo.cc` 里（作为基准对照实现，445 行），右边的完整版本
> 就是 `examples/echo_server.cc`（75 行）。

## 会话主体

**裸 liburing**（每个会话一个手写状态机，CQE 手工路由）：

```c
struct echo_conn {                       // 你自己的"操作对象"体系
    int fd;
    char buf[4096];
    enum { READ, WRITE } state;          // 手写状态机
};
static void on_cqe(struct echo_conn* c, int res) {
    switch (c->state) {
    case READ:
        if (res <= 0) { close(c->fd); free(c); return; }   // EOF/错误清理
        c->state = WRITE; c->len = res;
        sqe_prep_send(fd, MSG_NOSIGNAL, c->buf, res);      // 记得 MSG_NOSIGNAL，
        break;                                            // 否则一个死客户端 SIGPIPE 杀全进程
    case WRITE:
        if (res < 0) { close(c->fd); free(c); return; }
        c->state = READ;                                   // 部分写？裸写不重发——
        sqe_prep_recv(fd, c->buf, sizeof c->buf);          // 要正确就得再写一层重发状态
        break;
    }
}
// 外层还需要：CQE 遍历分发、user_data → echo_conn* 的映射约定、
// 退出/取消时在途 SQE 的追踪、并发连接的内存管理……
```

**iox**（`examples/echo_server.cc` 的会话体）：

```cpp
auto body = iox::io::loop(ctx, [sn, &ctx, eof = false]() mutable {
    return iox::io::read(ctx, sn->sock, iox::wbytes{sn->buf.get(), 4096})
         | iox::exec::let_value([sn, &ctx, &eof](std::size_t n) {
               eof = (n == 0);
               return iox::io::write_all(ctx, sn->sock,
                                         iox::rbytes{sn->buf.get(), n});
           })
         | iox::exec::then([&eof]() { return eof; });
});
iox::exec::detach(std::move(body) | iox::exec::then([sn] { delete sn; }));
```

不在右边的：状态机（`loop` 表达循环）、部分写重发（`write_all` 内建）、
SIGPIPE（类型化 socket 自动 `SEND|MSG_NOSIGNAL`）、CQE 路由（op 地址即
user_data）、EOF/错误的统一清理（两个通道都进 `then` 的删除）。

## 换个后端呢

会话体里只有 `io::read` / `io::write_all` —— 词汇按**能力**分派。
把 `net::tcp::socket` 换成 `fs::file`、`pipe` 端、`net::unix_dom::socket`
或 M6 的 `nvme::device`，同一段代码搬运不同的东西。裸 liburing 版本里
"换后端"意味着重写 prep/完成路径——每个 opcode 一套。

ioxpump（压轴示例）就是这句话的可执行证明：file→file / file→tcp / 任意
fd 端点组合，同一份核心；fd 端点自动走 splice 零拷贝，^C 优雅退出带统计。

## 优雅退出

裸 liburing：在途 SQE 追踪表 + exit 时逐个 cancel + 等待 CQE 收敛 + 信号
掩码处理（signal 会中断 io_uring_enter 的哲学问题）。

iox（`examples/ioxpump.cc`）：`signal::set` 阻塞信号 → `io::signal` 完成
消费掉的 siginfo → `src.request_stop()` → stop_token 沿 `when_all`/`loop`/
`pump` 传播 → 在途操作 `set_stopped`（不是错误）→ 带字节数离场。
信号只是另一种 readable 句柄。

## 代价

零抽象税：echo 基准 **100% of 裸 liburing**（p50 持平或反超，如 21.5µs vs 28.3µs / 23.2µs vs 29.7µs / 22.5µs vs 30.2µs——多次门禁记录），
文件读写 95~102%，提交路径绝对开销 +33ns/op（ADR-001/004）。手段：编译期
单态化、操作状态内联免堆、`batch_scope` 批量提交、CQE 内联恢复 receiver。

## 什么时候不该用 iox

- 需要跨平台（仅 Linux 6.x+，io_uring）
- 协议栈/序列化（只做字节流与数据块搬运）
- C ABI 兼容或纯 C 项目
- 线程池并行算法（单线程模型，多核靠分片，每线程一 context）
