# ADR-006: 源码布局——目录=功能模块，文件=最小功能单元

状态：已接受（M3 后重构）
范围：`include/iox/**`

## 背景

M3 结束时 `ops.h` 膨胀到 704 行（16 个操作 + 骨架 + 策略全部在一个
文件），`loop.h` 同时装着三个不相关的东西（loop / detach / write_all），
基础类型与域句柄混在同一层目录。随着 M4+（信号/进程/inotify）与 M5
（Driver SPI）继续扩张，单体头文件会让依赖关系不可见：改一个操作要读
全部操作，include 一个词汇就拖进所有词汇的内核头。

## 决策

**目录 = 功能模块，文件 = 最小功能单元**：

```
core/     fd, error, units, buffer, concepts, mr, exec   —— 与操作无关的基础层
uring/    ring                                            —— L1 驱动封装
runtime/  io_context, blocking_pool                       —— L2 执行引擎
ops/      fd_sender(骨架) + 一文件一操作(CPO+policy)      —— L3 统一词汇
compose/  loop, detach, write_all                         —— 词汇上的组合子
net/ fs/ pipe/ stdio/                                     —— L4 域句柄
```

配套规则：

1. **一文件一操作**：每个 `ops/*.h` = 一个 CPO + 它的 policy（+ sender
   别名）。骨架（fd_sender、完成 helper、取消槽、fd 提取）单独成
   `ops/fd_sender.h`——它是机制，不是词汇。
2. **伞头保留**：`iox/ops.h` 一条 include 提供全部词汇，用户零心智负担；
   库内部与精细用户按单元引用（如 blocking_pool 只引 `ops/poll.h`）。
3. **库内部禁止引伞头**：依赖图保持最窄，伞头只是用户入口。
4. **组合子独立于词汇**：loop/detach/write_all 各自一个文件——write_all
   依赖 loop 与 write 的关系在 include 里直接可见。
5. 命名空间不变（`iox::io` / `iox::exec` / `iox::net` …）——纯文件重组，
   零代码改动，回归验证靠既有 55 测试 + 基准。

## 后果

- 改某操作只触碰一个文件；新增操作 = 新增文件 + 伞头一行。
- include 依赖即架构图：ops → runtime → uring → core 的分层在 include
  里可读，不再靠注释。
- 文件数 20 → 36，但最大头文件从 704 行降到 ~250 行（fd_sender 骨架）。
- 旧路径（iox/io_context.h 等）不保留转发头：库未发布、无外部用户，
   一次性改完（测试/示例/基准同步更新）。
