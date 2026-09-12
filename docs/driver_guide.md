# iox 驱动开发指南

> 把一个设备/后端接进 iox 的完整路径。三份可直接读的范例由浅入深：
> `examples/eventfd_device.cc`（90 行教程）、`include/iox/nvme/`（device +
> io + admin，uring_cmd 直通）、`include/iox/xdp/`（umem + socket +
> read/write_frame，完成源桥）。决策背景见
> ADR-009（SPI 形状）与 ADR-011（两个真实驱动的取舍）。

## 一辆车的四个轮子

1. **句柄类型** —— 你定义一个类（不需要任何基类；句柄各自独立类型是
   刻意设计，无公共 object 模型）。
2. **词汇定制** —— 在句柄的命名空间写 `tag_invoke`，接管它支持的
   词汇操作（16 个 CPO 清单见 `include/iox/ops.h`）。
3. **完成通道** —— 驱动的完成不来自你提交的 SQE 时，从
   `iox::completion_source` 三模式里选一种（下文）。完成全部经
   `ctx.dispatch(op地址, res, flags)` 注入，op 走 `iox::op_base` thunk 协议。
4. **一行注册**（可选）—— `namespace iox::driver { template<> inline constexpr
   bool registered_driver<你的驱动> = true; }`，能力答案
   `tag_invoke(io::detail::supports_t, io::zero_copy_t, const H&)` 按需覆盖。

## 第 2 步：接管一个词汇操作

```cpp
// include/iox/mydev/device.h
namespace iox::mydev {
class device { /* … fd、几何、能力 … */ };

// 具体重载放在句柄旁（ADL 可见），天然击败 iox::io 里的 fd 默认模板
inline auto tag_invoke(io::read_at_t, io_context& ctx, device& d,
                       wbytes dest, uoffset_t at) noexcept {
    return io::detail::fd_sender<my_read_policy>{&ctx, d.cmd_handle(), {…}};
}
}
```

规则：

- **完成签名守契约**：`read_at` 系完成 `set_value(字节数)`；错误走
  `set_error(iox::error)`；可取消就必须声明并 honour `set_stopped_t()`。
  一致性套件（`tests/test_conformance.cc`）就是这套契约的可执行文本。
- **前置校验放 `immediate()`**：几何驱动的对齐、哨兵值在
  `Policy::immediate()` 里同步完成（参考 nvme 的 EINVAL/EOPNOTSUPP 守卫）
  ——绝不让内核替你"四舍五入"。
- **载荷存活期**：SQE/cmd 结构随 op state 走（fd_sender 的 args_t），
  数据缓冲归调用方（零拷贝契约）。

## 第 3 步：选完成通道

| 模式 | 什么时候 | 怎么做 |
|---|---|---|
| **fd 挂载** | 设备有就绪 fd（IRQ fd、xsk fd、完成通道） | `completion_fd()` 返回它；`on_ready()` 排空并 dispatch。零忙耗；`ctx.attach_source(d)` 一行接入 |
| **忙槽** | 无 fd、无线程，工作随时间自变就绪 | `completion_fd()` 返回无效 fd；`has_work()` 返回真时上下文每 ~1ms 调 `on_ready()`。耗 CPU，按定义 |
| **主动注入** | 驱动代码已在 io 线程上跑（另一个完成/定时器续体内） | 不挂载，直接 `ctx.dispatch(...)`。NVMe uring_cmd 的 CQE 本身走 ring，就属于这类 |

on_ready 里能做的事（包括 `ctx.detach_source(*this)` 自撤）都有测试背书
（`tests/test_driver.cc`）。

## op 的形状（不走 fd_sender 时）

```cpp
template <class R>
struct my_op final : iox::op_base {   // 地址 + 单函数指针 = 完成协议
    device* dev; R r;
    my_op(...) : op_base(&my_op::on_done), … {}
    static void on_done(op_base* self, io_context&, std::int32_t res, std::uint32_t) noexcept {
        auto* o = static_cast<my_op*>(self);
        res < 0 ? stdexec::set_error(std::move(o->r), iox::error::from_negative(res))
                : stdexec::set_value(std::move(o->r), 域值);
    }
    void start() noexcept { dev->submit(this); }
};
// sender：sender_concept + completion_signatures + connect(this Self&&, R&&)
```

## 测试与门禁怎么落

- **无硬件/无权限环境自跳过**：先探测（open 失败/socket EPERM），
  doctest `MESSAGE` + return。参考 `tests/test_nvme.cc` 的 `NVME_OR_SKIP`。
  至少留一个**无硬件也跑**的用例（nvme 的 EOPNOTSUPP 常驻测试）。
- **写路径要显式 opt-in**：参考 `IOX_NVME_TEST_WRITE` 只打最后一个 LBA。
- **基准对照裸实现**：`bench/nvme_rw.cc` 模式——同窗口同 QD，iox 侧
  用库组合子（`io::loop`），对照裸 liburing 循环，报告 ≥85% 比率。

## 已知边界（v1 诚实清单，ADR-011）

- 驱动操作的 stop_token 接线尚未标准化（fd_sender 词汇自动有；手写 op
  需要自己在 start() 里 arm）——M6+ 驱动规模化时补 SPI 约定。
- XDP copy 模式 TX 完成按 FIFO 配对；乱序 zerocopy 驱动需要 per-chunk 映射。
