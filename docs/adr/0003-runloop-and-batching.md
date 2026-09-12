# ADR-0003: 事件循环、run_for 与批量提交语义

- 状态：已接受（M1）
- 范围：io_context::run/run_for/stop、batch_scope、SQPOLL 默认值

## 决策

### run() 循环形态

```
while (!stopped_) {
    if (batch_depth_ == 0) ring_.flush();   // 收拢本轮提交
    if (drain() != 0) continue;             // 收割并分发就绪 CQE（不阻塞）
    if (stopped_) break;
    ring_.flush_and_wait(1);                // 无就绪事件：阻塞等待
}
```

- 完成在收割循环内**内联恢复 receiver**（§三.⑧），无中间队列。
- 无未决操作时 `run()` 会阻塞——事件驱动语义（同 libuv）。驱动到点用
  `run_for` 或在完成里 `stop()`。

### run_for 的陈旧 deadline 问题

`run_for` 用 IORING_OP_TIMEOUT(abs) 实现截止。若循环被完成提前停止，
deadline op 仍挂在内核里，之后触发时会误停**下一次** run。解法：deadline op
常驻 io_context（地址恒有效），携带 `epoch`；仅当触发时的 epoch 与当前
`run_epoch_` 相等才 stop。副作用：迟到的 CQE 分发到同一个常驻 thunk，
幂等安全。

- `run_for` 入口重置 `stopped_`，可重复调用。

### start() 不提交

op 的 `start()` 只填充 SQE，不 enter——run() 循环顶部统一 flush，天然批量
（§三.③）。需要立即生效的场景：`ctx.flush()` 或离开 `batch_scope`。

### SQPOLL 默认关闭

SQPOLL 依赖常驻内核线程烧一个核，仅在极高提交速率下划算。默认用户态
submit；`uring::ring_params::sq_poll` 保留开关。

## 已知限制（M1）

- run() 阻塞在 `flush_and_wait(1)` 期间，同线程无法注入工作；跨线程唤醒
  （eventfd）在后续里程碑提供。
- CQ 溢出依赖下一轮 enter 收敛（io_uring 标准行为）；高吞吐场景应调大
  `cq_entries`（memlock 限制内）。
