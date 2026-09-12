# ADR-0002: 执行层选择 stdexec，经 iox::exec 隔离

- 状态：已接受（M0/M1）
- 范围：sender/receiver 执行层的实现选型与集成方式

## 背景

统一前端以 C++26 `std::execution`（P2300）为异步模型。可选实现：

1. **stdexec**（NVIDIA 的 P2300 参考实现）
2. libstdc++ 15 的部分 `std::execution`（需 `-std=c++26`，特性不全）
3. 自研最小 sender/receiver 子集

## 决策

引入 stdexec（vendor 于 `third_party/stdexec/`），但**所有 iox 代码与用户代码
只依赖 `iox::exec`**——它是命名空间别名 + 我们自己的扩展（`sync_wait(ctx, s)`）。
当 libstdc++ 的 `std::execution` 足够完整时，仅改 `iox::exec` 一处。

实测集成成本：GCC 15.2 + C++23 下单 TU 编译 ~2s，无运行时依赖。

## 集成要点（这版 stdexec 的接口形态）

- 自定义 sender：内嵌 `using sender_concept = stdexec::sender_tag;` +
  `completion_signatures` + **deducing-this 的 `.connect(receiver)` 成员**
  （C++23；stdexec 内部会以左值连接，纯 `&&` 限定不可用）。
- 自定义 op_state：`using operation_state_concept = ...;` + `.start() noexcept`
  成员（tag_invoke 已弃用）。
- 自定义 receiver：内嵌 `using receiver_concept = stdexec::receiver_tag;`，
  `set_error` 必须能接收 `std::exception_ptr`（算法异常通道）与自有错误类型。
- libstdc++ 15 的 `<variant>` **不提供** `std::tuple_element<variant>` 特化，
  用 `std::variant_alternative_t`。

## iox::exec::sync_wait 的特殊性

stdexec 的 `sync_wait` 在内部 run_loop 上阻塞——与"完成由 io_uring 产生、
必须由本线程泵 io_context"的模型死锁。因此 iox 提供自己的
`sync_wait(io_context&, sender)`：启动后若未同步完成，则在**当前线程**跑
`ctx.run()`，完成回调 `stop()` 循环。

## 备胎

若 stdexec 编译时间随词汇增长失控，回退方案是自研 P2300 最小子集
（connect/start/set_* + then/let_value/when_all），`iox::exec` 接口不变。
