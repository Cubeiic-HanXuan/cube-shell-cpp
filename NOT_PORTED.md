# 不移植清单（NOT_PORTED）

记录 Python 侧存在、但**有意不移植**到 `cpp/` 的文件及原因。这些文件解决的是
CPython/PySide6 运行时特有的问题，在 C++/Qt6 目标下不存在对应问题，或已由
C++ 语言/标准库天然覆盖。

## core/shiboken_heal.py — 不移植

ARM 架构上 shiboken6 对 `QPainter`/`QFont` 等 void 方法的包装器每次调用都会
多执行一次 `Py_DECREF(Py_None)`，导致 `None` 引用计数持续下降并触发 CPython
`fatal error: none_dealloc`。该模块用 `ctypes` 直接改写 `None` 的 `ob_refcnt`
并起定时器持续补充。

**不移植原因**：这是 Python/C 绑定层（shiboken6）的引用计数 bug workaround。
C++ 直接调用 Qt API，不存在 Python 对象与引用计数，问题本身不成立。

## core/automation.py — 不移植（其命令执行语义已在别处覆盖）

Python 版的自动化编排：`@dataclass` 定义步骤/执行记录/修复动作，用
`appdirs` 定位配置目录，把编排剧本以 JSON 落盘，并驱动 AI 修复重试循环。

**不移植原因**：
1. 该模块本体是「dataclass + JSON 序列化 + 回调」的 Python 动态胶水层，
   直接翻译只会得到大量样板代码，无移植收益。
2. 其中真正被复用的**命令执行**语义已经在 Phase 2 落到
   `cpp/src/core/ssh/CommandExecutor.cpp`（见该文件 `对应Python:` 注释）。
3. AI 修复重试链路属于 AI 模块范围（`cpp/src/core/ai/`，尚未开工），届时按
   C++ 惯用法（信号槽 + 状态机）重新设计，而非照搬 Python 的回调结构。

## 其他随之落空的 Python 专属代码

以下不是独立文件，但同样不随移植，集中记录以免后续 agent 反复确认：

- `core/rdp/rdp_client.py` 里的三个模块级猴子补丁
  （`_install_asn1_cache` / `_install_wheel_steps_patch` / `_install_read_diag`）：
  均为纯 Python 库 `aardwolf` 的专属 workaround（ASN.1 schema 编译耗时、
  滚轮 `PTRFLAGS.WHEEL` 缺失、`UniConnection.read` 吞异常）。C++ 侧 RDP 走
  FreeRDP 库 / `xfreerdp`·`mstsc` 命令行（见 `src/core/rdp/RdpClient.cpp`），
  这些 bug 不存在。
- `core/windows_integration.py` 的 `_is_frozen` / `_get_exe_path`
  多重探测：用于区分 Nuitka 打包与源码运行。C++ 可执行文件天然就是 exe 本身，
  统一用 `QCoreApplication::applicationFilePath()`。
- `core/url_dispatch/url_scheme_register.py` 的 `sys.frozen` / `__compiled__`
  检测：同上。
