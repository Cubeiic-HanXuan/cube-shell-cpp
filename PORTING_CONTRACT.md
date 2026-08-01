# qtermwidget C++ 移植契约(并行翻译必读)

本文件是把 Python 版 `qtermwidget/` 包翻译回 C++ 的**统一约定**。所有并行翻译 agent 必须严格遵守,以保证各自产出的模块能无冲突地编译、链接、集成。

## 背景

- Python 版 `qtermwidget/` 本身就是从**上游 QTermWidget 2.2.0 (C++/Qt)** 逐行移植来的,移植者在注释里保留了大量 `对应C++: <原始C++签名/实现>` 的映射。
- 因此每个 Python 文件都对应一个明确的上游 C++ 文件。**优先参照 Python 注释里给出的 C++ 签名与逻辑**,再参照上游 QTermWidget 2.2.0 的同名文件补全细节。
- Python 版存在若干**移植引入的 bug**(例如把 16 位标志截断成 8 位),翻译时要按**语义正确性**修正,不要盲目照抄。

## 硬性约定

1. **命名空间**:所有终端模拟核心类放在 `namespace Konsole { ... }`(与上游一致)。顶层对外 widget(`QTermWidget`)也沿用此命名空间下的内部实现,再暴露一个无命名空间的 `QTermWidget` 外壳类。
2. **文件命名**:沿用上游 C++ 文件名,首字母大写类名同名。映射:
   - `character.py` → `Character.h/.cpp`(已完成)
   - `character_color.py` → `CharacterColor.h/.cpp`(已完成)
   - `wcwidth.py` → `konsole_wcwidth.h/.cpp`(已完成)
   - `tools.py` → `tools.h/.cpp`(已完成)
   - `block_array.py` → `BlockArray.h/.cpp`
   - `history.py` → `History.h/.cpp`
   - `terminal_character_decoder.py` → `TerminalCharacterDecoder.h/.cpp`
   - `color_scheme.py` → `ColorScheme.h/.cpp`
   - `keyboard_translator.py` → `KeyboardTranslator.h/.cpp`
   - `filter.py` → `Filter.h/.cpp`
   - `screen.py` → `Screen.h/.cpp`
   - `screen_window.py` → `ScreenWindow.h/.cpp`
   - `emulation.py` → `Emulation.h/.cpp`
   - `vt102_emulation.py` → `Vt102Emulation.h/.cpp`
   - `kprocess.py` → `KProcess.h/.cpp`
   - `kpty.py` → `KPty.h/.cpp`
   - `kpty_device.py` → `KPtyDevice.h/.cpp`
   - `kptyprocess.py` → `KPtyProcess.h/.cpp`
   - `pty.py` → `Pty.h/.cpp`
   - `shell_command.py` → `ShellCommand.h/.cpp`
   - `session.py` → `Session.h/.cpp`
   - `search_bar.py` → `SearchBar.h/.cpp`
   - `history_search.py` → `HistorySearch.h/.cpp`
   - `terminal_display.py` → `TerminalDisplay.h/.cpp`
   - `qtermwidget.py` → `qtermwidget.h/.cpp`(对外 widget,注意小写文件名同上游)
3. **头文件**:用 `#pragma once`。
4. **Qt 版本**:Qt6。**不要**用 KDE 框架(KF5/KF6)——所有 `KProcess`/`KPty` 等都用本项目自带的同名实现替代。
5. **字符串/容器**:统一用 `QString`/`QVector`/`QList`/`QHash`。Python 的 `bytes` 对应 `QByteArray`,`str` 对应 `QString`,单个字符用 `QChar` 或 `uint`(码点)。
6. **信号槽**:Python 的 `Signal(...)` 对应 Qt 信号;`emulation.sendData.connect(...)` 这类在 SSH 桥接里被用到,**必须保留** `Emulation::sendData(const char*, int)`(或 `QByteArray`)信号与 `Session` 的相关信号。
7. **cube-shell 依赖的钩子**(务必保留,`SSHQTermWidget`/`paramiko_bridge` 用到):
   - `QTermWidget` 内部要能被外部访问到 `TerminalDisplay`(Python 里是 `m_impl.m_terminalDisplay`)。提供一个访问器。
   - `TerminalDisplay` 暴露 `_color_table`(C++ 里做成可访问的颜色表访问器)。
   - `Emulation::sendData` 信号、`Session::emulation()`、`Session::onReceiveBlock()` 槽、pty 的 `setWindowSize(lines, cols)`。
   - `QTermWidget` 公共 API:`setColorScheme`、`sendText`、`setTerminalFont`、`copyClipboard`、`pasteClipboard`、`pasteSelection`、`selectedText`、`clearSelection`、`findText`、`setSuppressProgramBackgroundColors`、`setScrollBarPosition`、`setHistoryType`、`setKeyBindings`、`setBlinkingCursor`、`setTerminalSizeHint`、`setFlowControlEnabled`、`setMotionAfterPasting`。
8. **平台代码**:`kpty`/`pty` 里的 PTY 逻辑,POSIX 用 `posix_openpt`/`grantpt`/`unlockpt`/`ptsname`,macOS 注意 `/dev/ttysXXX` 命名;**Windows 的 ConPTY/winpty 先留 stub**(标 `// TODO(win32)`),不阻塞 POSIX 主路径。
9. **不要修改**已完成且与本模块无关的文件;不要动 `CMakeLists.txt` 里别人已启用的行(集成由主控统一做)。
10. **每个 agent 完成后**:报告产出的文件清单、对外类/信号清单、以及对其他模块的**接口假设**(你 import 了哪些还没翻好的类的什么成员)。

## 代码风格

- 与已完成的 `Character.h`/`CharacterColor.h`/`tools.cpp` 保持一致:`// 对应C++: ...` 注释标注映射、4 空格缩进、`Konsole` 命名空间。
- 注释保留原版权头。
- 翻译要"地道 C++",不是机械逐行:Python 的动态特性(猴子补丁、鸭子类型)要转成合理的 C++ 接口/虚函数/信号。

## 编译验证

- 完成后主控会把新文件加进 `cpp/src/qtermwidget/CMakeLists.txt` 并 `cmake --build build`。agent 自己**不要**改 CMake,只需保证 `.cpp` 能独立编译(语法正确、include 齐全)。
