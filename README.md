# iox

![CI](https://github.com/napplesty/iox/actions/workflows/ci.yml/badge.svg)

Linux 统一异步 IO 库（C++23）：一套类型安全的操作词汇，对任何后端句柄——文件、
管道、网络、NVMe 队列、XDP socket——语义一致；异步模型统一为 sender/receiver
（P2300 形态）；io_uring 直达内核，性能以裸 liburing 为对照门禁。

```
io::read(ctx, sock, buf.wview()) | exec::then(...)   // 同一个词，任何句柄
```

## 特性

- **统一词汇**：19 个 CPO（read/write/read_at/write_at/fsync/open/close/poll/
  accept/connect/send_to/recv_from/splice/tee/schedule/sleep_for/sleep_until/
  signal/wait_pid），`tag_invoke` 可定制，编译期单态化、零虚调用。
- **类型即约束**：能力 concepts（readable/writable/…）让不支持的操作编译报错；
  buffer 方向化（`wbytes`/`rbytes`）；错误分域 `iox::error`；`uoffset_t`/
  `io_size_t` newtype 防参数错位。
- **性能即门禁**：提交路径零堆分配、零原子；`batch_scope` RAII 批量
  `io_uring_enter`；注册内存零拷贝（`write_fixed`）；splice 系内核内搬运。
  文件读写基准 95~102% of 裸 liburing，echo 吞吐持平、p50 反超。
- **Driver SPI**：外部完成源三通道接入（fd 挂载/忙槽/主动注入），NVMe
  （uring_cmd 直通）与 XDP（AF_XDP）为参考驱动，核心零改动接入新硬件。
- **取消是一等公民**：stop_token → cancel receipt，提交后立即取消的竞态有
  故障注入用例锁定；优雅退出见 `ioxpump`。
- **Python API**：nanobind 同步表面、GIL 释放、每线程一个 Context。

## 快速开始

```bash
git clone --recursive https://github.com/napplesty/iox.git   # nanobind 子模块嵌套 robin_map
cd iox
cmake -B build -G Ninja
cmake --build build -j
(cd build && ctest --output-on-failure)
```

需要 CMake ≥ 3.24、C++23 编译器（GCC 15 验证）、liburing（发行版版本过旧缺
SQE128 时仅 NVMe 驱动不可用；CI 从源码构建最新版）。构建开关：
`IOX_BUILD_TESTS / IOX_BUILD_EXAMPLES / IOX_BUILD_BENCH / IOX_BUILD_PYTHON /
IOX_SANITIZE`。

Python 侧（需带开发头文件的解释器）：

```bash
pip install .            # scikit-build-core + nanobind
python examples/python/file_copy.py src.bin dst.bin
```

## 架构（摘要）

```
L4 域句柄      net / fs / pipe / stdio / process / signal / dns + 驱动句柄（nvme / xdp）
L3 统一词汇    CPO（read/write/poll/connect/…，tag_invoke 可定制）+ 能力 concepts，
               编译期单态化
L2 运行时      io_context：io_uring 底座 + 完成分发 + batch_scope
               外部完成源桥（M5）+ 阻塞逃生舱线程池（M3 DNS）
L1 驱动层      fd driver（默认实现）+ 参考驱动 nvme / xdp（已落地）
```

设计决策记录见 [docs/adr/](docs/adr/)。性能预算与手段见
[ADR-001](docs/adr/0001-completion-dispatch-and-performance-budget.md)。
文档套件（M7）：[why_iox 前后对比](docs/why_iox.md)（同一个 echo，
裸 liburing vs iox）、[驱动开发指南](docs/driver_guide.md)（四个轮子 +
三种完成通道）、[API 速查](docs/api_reference.md)。

## 目录结构（目录=功能模块，文件=最小功能单元）

```
include/iox/
  iox.h  ops.h       伞头：一条 include 拿到全部词汇
  core/              基础类型层：fd / error / units / buffer(方向化视图) /
                     concepts(能力) / mr(注册内存) / exec(stdexec 隔离+sync_wait) / cpo
  uring/             liburing RAII 封装（ring）
  runtime/           执行引擎：io_context / blocking_pool(阻塞逃生舱)
  ops/               统一词汇：一文件一操作（fd_sender.h 是共享骨架，
                     其余每个文件 = 一个 CPO + 它的 policy）
  compose/           词汇之上的组合子：loop / detach / write_all / pump
  net/               域句柄：endpoint / tcp / unix / udp / dns
  fs/  pipe/  stdio/ 域句柄：file / channel / stream / watcher(inotify)
  signal/ process/   域句柄：set+watcher(signalfd) / process(posix_spawn+pidfd)
  driver/            Driver SPI：completion_source(完成源桥) / capabilities /
                     registry；nvme/  xdp/ 为参考驱动
python/              nanobind 绑定（同步表面，GIL 释放）
examples/            C++ 示例（ioxpump 为压轴）+ python/ 场景示例
bench/               submit_path / echo / file_rw / splice / nvme_rw
tests/               doctest 套件 + 一致性/故障/红队回归 + python e2e
```

## 里程碑

- [x] M0 环境 + 骨架（CMake/C++23、stdexec、doctest）
- [x] M1 核心底座（io_context / schedule / timer / fd poll+read+write / batch /
      sync_wait）
- [x] M2 类型化句柄 + 注册内存零拷贝 + 取消链路 + fsync/close + 一致性雏形
      （file_copy 示例；文件读写基准 95~102% of 裸 liburing）
- [x] M3 网络全套（TCP/UDP/Unix）+ DNS 线程池桥 + `io::loop`/`exec::detach`
      + `write_all` 部分写重发（echo 基准 100% of 裸 liburing，p50 反超；
      ioxpump file↔tcp 端到端字节一致）
- [x] M4 信号 / 进程(pidfd) / inotify / splice 零拷贝（ioxpump ^C 优雅退出 +
      `child` 子进程管道示例；详见 ADR-007。异步 open 顺延至 M5 与故障注入一起）
- [x] M5 Driver SPI + 完成源桥（词汇 CPO 化 + `eventfd_device` 验收教程；
      详见 ADR-009）
- [x] M6 前置：`io::open` + 故障注入（`arm_failpoint` 第 N 次提交注入 errno +
      真实断连/取消竞态）+ 一致性套件全量化（EOF × pipe/unix/tcp/file、
      死端 × pipe/tcp、取消 × pipe/unix/tcp；详见 ADR-010）
- [x] M6 参考驱动：**NVMe 已落地**（uring_cmd 直通、字节偏移 LBA 映射、
      fsync=FLUSH、admin 逃生口、sqe128 环、运行时检测无权限即清晰 SKIP——
      `setfacl -m u:$USER:rw /dev/ng0n1` 一条命令即可点亮真机用例与基准；
      详见 ADR-011）；**XDP 已落地**（AF_XDP：umem/环形区、fd 挂载完成源——
      xsk fd 可读即 on_ready 派发、copy-mode TX 无需 BPF 程序、frame 借还
      回收；本机无 CAP_NET_RAW 时用例带原因跳过，umem/能力用例常绿）。
      **RDMA 移出路线图**（小众硬件；M5 SPI 保证了后补通道，ibverbs 完成
      通道即 fd 挂载源）
- [x] M7 文档完备（why_iox 前后对比 / 驱动开发指南 / API 速查；架构摘要在本
      README，决策全记录于 ADR）
- [x] 仓库化与 Python 表面：GitHub Actions CI（ubuntu-26.04，源码 liburing，
      C++ 与 Python 双 job）、nanobind 绑定 + `pip install .`、红队三轮加固
      （ADR-008）、组织重构（ADR-0013）、注释密度策略（ADR-0014）、
      Python 场景示例（file_copy / echo_server / http_get / read_bench /
      kv_cache——LLM 推理分层 KV 池）

## 测试

- doctest 单元 + 集成：115 用例 / 616 断言，release 与 ASan+UBSan 双绿。
- **一致性套件**：同一份参数化断言跑 pipe/unix/tcp/file/raw fd（EOF、死端、
  取消、注册缓冲）——“统一前端”的可执行证明。
- 故障注入：第 N 次提交失败、EMFILE、mid-op 断连、提交后立即取消的竞态。
- 红队回归（test_redteam）：三轮攻击战役中复现过的缺陷全部固化为用例。
- 头文件自含扫描：全部头可独立 include。
- Python e2e：文件 roundtrip / ENOENT / 定时 / TCP echo / kv_cache 示例
  （CI 常跑）。

## License

Apache-2.0
