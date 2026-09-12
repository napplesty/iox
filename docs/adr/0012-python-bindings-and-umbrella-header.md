# ADR-012: nanobind Python 绑定与全库伞头文件

日期：2026-09-12　状态：已实施（M8）

## 背景

两个需求：① 一个"专门的 api.h"——此前只有 `iox/ops.h` 这类部分聚合头，没有全库单 include；② 用 nanobind 做 Python API。环境约束：系统 Python 3.14 无 `Python.h`（无 python3-dev、无 pip、无 sudo）。

## 决策

### 1. `include/iox/iox.h` 伞头文件

L2（runtime/exec）+ L3（ops.h + compose）+ L4（fs/net/pipe/signal/process/stdio）+ Driver SPI 一次引入。**刻意不包含** `iox/nvme/`、`iox/xdp/`：参考驱动会拉内核 uapi 头与硬件预期，用到时显式 include。55 个头全部自含（含伞头自身）。

### 2. Python API：同步阻塞形态 + GIL 释放

`python/iox.cc`（nanobind 3.0.1，模块名 `iox`）。每个操作都是统一词汇的 `iox::exec::sync_wait` 包装：

| Python | C++ 词汇 |
|---|---|
| `iox.Context()` | `io_context` |
| `iox.open_file(ctx, path, mode)` / `File.read_at/write_at/read/write/fsync/close` | `fs::file::open` / `io::read_at/write_at/read/write/fsync/close` |
| `iox.connect/listen` / `TcpSocket.send/recv/close` / `Listener.accept` | `io::connect` / `io::write_all/read/close` / `io::accept` |
| `iox.sleep(ctx, s)` | `io::sleep_for`（走 ring 的 timeout） |

- **GIL**：`sync_wait`（及阻塞的 open/listen）期间 `gil_scoped_release`，作用域在触碰任何 Python C-API 之前结束（错误转换全部在重取 GIL 之后）。
- **错误**：`iox::error → OSError(errno, message)`（code 取自 `error::code()`）；stopped → ValueError。
- **生命周期**：句柄只存 `PyIOContext*`，靠 nanobind `keep_alive` 链保活（File→Context，accept 返回的 Socket→Listener→Context）。
- **单线程约束**：Context（及其句柄）不得跨线程使用——ring 与 op 状态单线程是库的设计前提，违反即死锁（挂点 `io_cqring_wait`，见下）。多线程 Python 用户应每线程一个 Context。模块 docstring 已写明。
- mode 参数收 `unsigned`：Python 侧算术枚举 `Mode.rw | Mode.create` 产生 int，不做隐式枚举转换。

### 3. 构建集成

`IOX_BUILD_PYTHON`（默认 ON，找不到 nanobind/Development.Module 时优雅跳过并打印提示）。构建方式（零系统污染，全在 ~/.local 与项目内）：

```
uv venv --python 3.14 .venv && uv pip install --python .venv nanobind
PATH=$PWD/.venv/bin:$PATH cmake -S . -B build-py \
  -Dnanobind_DIR=$(./.venv/bin/python -c 'import nanobind; print(nanobind.cmake_dir())')
cmake --build build-py -j && (cd build-py && ctest -R iox_python)
```

解释器用 uv 管理的 CPython 3.14.6（`~/.local/share/uv/python/`，自带完整头文件）。ctest 目标 `iox_python` 跑 `tests/test_python.py`（文件往返/ENOENT/sleep/TCP echo，PYTHONPATH 指向构建目录）。

## 踩坑记录（对未来绑定扩展有用）

1. **CPython C-API 已占用 `PyContext`**（contextvars 的 C 类型名）——包装类型改名 `PyIOContext`，报错形态是费解的 "template argument 1 is invalid"。
2. nanobind 的重抛异常是 `nb::python_error`，不是 pybind11 的 `error_already_set`。
3. nanobind 3.x `nanobind_add_module` 只收**纯位置**源文件（无 `SOURCE`/`OUTPUT_NAME` 关键字）；输出名用 `set_target_properties` 改。
4. 其 config 要求 `find_package(Python)`（`Python_*` 变量），`FindPython3` 的变量它看不见。
5. `wait()` 返回 `*res.value`（tuple）必须 move 返回：`tuple<tcp::socket>` 不可拷贝。
6. `Python.h` 必须最先 include——它拥有 `_POSIX_C_SOURCE/_XOPEN_SOURCE` 宏，与 libstdc++ 头互相重定义。
7. 测试初版把 `accept()` 放进服务线程、与主线程共用 Context → `io_cqring_wait` 永眠（跨线程用 ctx）；改为单线程 + python socket 对端（listen backlog 使 connect 先完成）。

## 后续（未做）

asyncio 桥（`completion_source` → 事件循环回调是天然缝）、buffer protocol 零拷贝 recv（当前 bytes 拷出）、每线程 Context 的辅助封装、其余域（signal/process/pipe/udp）、uv 包形态（pyproject + uv build）。这些都记为候选，不阻塞 M8 验收。

## 验收

`tests/test_python.py` 全过；ctest `iox_python` 绿；默认构建路径（无 venv）不受影响；55 头自含；主 C++ 套件不受影响（115/115 release + ASan 不变）。

## Addendum: pip 安装形态（2026-09-12 补齐）

`pyproject.toml`（scikit-build-core + nanobind 作为 build requires）落地：
`uv pip install .`（等价 `pip install .`）走隔离构建环境出 wheel——cmake
args 关掉 tests/examples/bench，`build-wheel/` 隔离于手写构建树，模块
install 到 wheel 根并带 `~/.local/lib` RPATH（liburing 位置）。装后
`import iox` 无需 PYTHONPATH，`tests/test_python.py` 对已安装副本全过；
树内 `-Dnanobind_DIR` 路径并行不悖。nanobind **不是** third_party
submodule（本项目无 git；third_party 只有 doctest/stdexec 纯源码），
它来自构建环境的 pip 包——vendoring 它（~2MB 源码 + cmake）留作将来
离线构建的可选项。
- 2026-09-12 晚补：读路径改为单拷贝（PyBytes 直读内核写入 + _PyBytes_Resize 截断，
  替代 std::string 暂存），吞吐翻倍至与内置打平；`examples/python/` 四个场景示例
  （file_copy/echo_server/http_get/read_bench）落地，read_bench 以"边界开销探针"
  框架如实呈现 sync 绑定的适用边界（批量 API 是将来的答案）。
