# ADR-013: 文件组织重构（内聚/短文件/命名）

日期：2026-09-12　状态：已实施。审计：`.attack3/org/REPORT.md`（后台 agent，
量化 + grep 实证，含"不要动"清单）。全部改动后门禁：115/115 release + ASan、
python 绑定端到端、63 头自含。

## 动机

用户诉求：单文件功能内聚、文件短（利于并行编译与可读性）、少注释、命名不用
缩写。审计结论先行：**该库组织纪律优于平均**——ops/ 一文件一 CPO 是正确范式、
注释几乎全是"为什么"、`impl/cb/cnt/ret/ptr/num/idx` 全库为零。真正动手的集中
在 3 个长头文件与一批缩写。

## 拆分（全部 header-only，依赖链 grep 核实单向）

| 原文件 | 拆后 | 行数 |
|---|---|---|
| xdp/socket.h (508) | umem.h 85（frame+共享内存；顺带删除死代码 `friend class socket`） / socket.h 344（句柄+能力+注册） / write_frame.h 63 / read.h 58（词汇，对齐 ops/ 范式） | −164 |
| runtime/io_context.h (505) | op.h 22（op_base ABI，fd_sender/驱动/桥共用） / source_registry.h 83 + source_registry_impl.h 142（驱动桥注册表；两段式定义避免头环——方法体需完整 io_context，文本性后置包含） / io_context.h 335（纯事件循环） | −170 |
| nvme/device.h (379) | device.h 169（句柄+几何+能力，去 ops 依赖） / io.h 163（read_at/write_at/fsync 定制） / admin.h 72（admin 逃生口；顺带删除死代码 op_identify） | 0（3 文件） |

配套：`ops/completers.h` 61 行（4 个公共完成器，fd_sender.h 251→191）；
`tests/support/softdev.h` 261（软件设备夹具，test_driver.cc 451→221；
教程形态的 eventfd_device.cc 刻意保持单文件）。消费方 include 增补：
test_xdp（+read/write_frame）、test_nvme/bench_nvme（+io/admin）。

**拆分时修复一个潜伏 bug**：`socket::take_pending_frame` 是 private，却被
`detail::rx_op::on_done` 调用——只因 `xdp::read` 从未实例化而未爆。已提到公共
词汇面（与 submit_rx 同类的完成缝）。

## 注释

审计实证"注释过多"不成立：需删的复述式注释仅 6 处（已删）。高占比文件
（completion_source 61%、cpo 52%）是刻意的内核语义/生命周期文档，列入保护
清单。另修 3 处 banner 与文件名不符、1 处 UTF-8 双重编码、8 处 `.cpp` 残留。

## 命名

- **一词三义修复**：`rc`（mr/process 的 return code → `result`；io_context 的
  cancel receipt → `receipt`；loop 的 receiver → `recv`）。
- 全库（头+测试+示例+绑定）：`sigs→signatures`、`src→source/stop_source`、
  `dst→dest`、`buf→buffer`、`addr→address`、`ep→endpoint`、`len→length`、
  `efd→event_fd`、`pfd→pidfd`、`tmp→scratch`、`tok→token`、`eptr→exception`、
  `fl/pr/pw/m→flags/bounce_*_fd/spliced`（pump）、`ns→nsid`、`lbs→block_size`、
  `nd/sx/reg/olen/o/fr`（xdp）、`d→dev`（nvme）等，共 40+ 项按报告 4.2 表执行。
- **刻意保留**（领域惯用，报告 5.3）：fd/ctx/io、sqe/cqe、res（CQE 镜像参数）、
  ts/wd/sin/sun（内核 API 术语）、tx/rx、recv/send_to、newtype 成员 `.v`、
  stdexec 生态的 r/Sndr/env。

## 未做（记录）

- pump.h 的 `recover_stuck` 微拆（247→180 行，收益小）；bench/echo 的公共段
  抽取（A/B 同文件是基准交付形态）；`.v→value` 全局改（94 处，newtype 惯用）。
- `io_size_t` 近乎死代码、ioxpump 掏 `io::detail`——语义决策，留给后续。
- 头文件匿名 namespace 的 ODR 噪音（pump.h 的 set_nonblock）——随 pump 微拆
  一起做。

## 工具链注意

sed 词边界 `\b` 在"前导边界+字母"组合（`\box`）下不生效——本案用
`\bword\b` 全词匹配均安全；一次内核字段误伤（`xdp_umem_reg::addr`）由编译门
立即捕获。无 git 仓库，改动前 tar 快照（/tmp/iox-org-backup-0912.tgz）。
