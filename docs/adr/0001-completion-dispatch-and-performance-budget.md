# ADR-0001: 完成分发设计与性能预算

- 状态：已接受（M1）
- 范围：io_context 完成分发、热路径预算、基准门禁

## 背景

统一词汇层的每个异步操作都由 io_uring 完成。设计要求（设计文档 §三）：
热路径零堆分配、零原子、零虚调用，提交路径 <100ns/op，吞吐 ≥ 裸 liburing 的 85%。

## 决策

1. **user_data = op 指针 + 单函数指针 thunk**。每个 op_state 继承
   `iox::op_base{ thunk_t thunk }`，地址放进 SQE user_data；CQE 回来后
   `io_context::dispatch` 调用 `op->thunk(op, ctx, res, flags)`。
   无 vtable、无查表、无分配。唯一的间接调用是 thunk 本身——这是异构完成
   分发的下界。
2. **op_state 内联生命周期**。op 由 sender 的 `connect` 按值返回、由调用方
   存储（栈/父 op 内嵌），P2300 的连接机制天然免堆（§三.②）。
3. **批量提交**。`start()` 不提交；`io_context::run()` 每轮循环顶部统一
   flush，`batch_scope` 供用户显式聚合（一次 `io_uring_enter` 刷出整批）。
4. **op_base 按 cacheline 对齐**（`alignas(64)`），完成分发目标不共享缓存行。
5. **基准即门禁**：`bench/submit_path.cc` 以裸 liburing 为对照，每次
   里程碑必须报告比值（M1 实测 94–99%）。

## 后果

- `dispatch()` 是公开入口——它同时是单测的"假 CQE 注入"点，也是 M5
  Driver SPI「主动注入」完成源的桥接口。
- op_state 地址在 start 之后必须稳定（不能移动）；P2305 语义已保证。
- 若未来引入 SQPOLL，flush 语义不变（`io_uring_submit` 自动走无 syscall 路径），
  但需评估内核线程占核成本（见 ADR-0003）。

## 实测（M1，GCC 15.2 -O2，65536 nops）

| 路径 | ns/op | Mops/s |
|---|---|---|
| 裸 liburing | 40–42 | 24–25 |
| iox 全词汇路径（CPO→sender→connect→start→SQE→CQE→receiver） | 42.7 | 23.4 |
| 比值 | **94–99%** | — |
