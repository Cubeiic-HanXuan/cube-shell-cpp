# Terminal emulator widget — C++ port of the Python `qtermwidget` package
# (which was itself ported from upstream QTermWidget 2.2.0).
#
# The Python port preserved "对应C++: ..." mapping comments throughout; this
# module is the C++ re-implementation those comments point back to, extended
# with the cube-shell-specific hooks (m_impl / m_terminalDisplay access,
# Session emulation signals, setWindowSize interception) that SSHQTermWidget
# and the SSH bridge rely on.

set(QTERMWIDGET_SOURCES
    # --- core emulation / screen ---
    Character.cpp
    CharacterColor.cpp
    konsole_wcwidth.cpp
    tools.cpp
    KProcess.cpp
    BlockArray.cpp
    ColorScheme.cpp
    KeyboardTranslator.cpp
    ShellCommand.cpp
    TerminalCharacterDecoder.cpp
    History.cpp
    # --- wave 2: core coupled ---
    Screen.cpp
    ScreenWindow.cpp
    Emulation.cpp
    Vt102Emulation.cpp
    Filter.cpp
    SearchBar.cpp
    # --- wave 3: top-level ---
    Session.cpp
    TerminalDisplay.cpp
    HistorySearch.cpp
    qtermwidget.cpp
)

# 本地终端后端（KPty/KPtyDevice/KPtyProcess/Pty）。
# HarmonyOS 沙箱下无 forkpty/posix_openpt——CUBESHELL_WITH_LOCALPTY=OFF，
# 终端只走 Session::runEmptyPTY() + 手动接管 emulation 字节流的远程数据源路径。
if(CUBESHELL_WITH_LOCALPTY)
    list(APPEND QTERMWIDGET_SOURCES KPty.cpp Pty.cpp KPtyDevice.cpp KPtyProcess.cpp)
endif()

# Placeholder translation unit kept only until enough real sources exist.
# Currently unused since Character.cpp / CharacterColor.cpp are enabled.
set(QTERMWIDGET_PLACEHOLDER ${CMAKE_CURRENT_SOURCE_DIR}/qtermwidget_placeholder.cpp)
if(NOT QTERMWIDGET_SOURCES)
    set(QTERMWIDGET_SOURCES ${QTERMWIDGET_PLACEHOLDER})
endif()

add_library(qtermwidget STATIC ${QTERMWIDGET_SOURCES})

# Windows 走 ConPTY（CreatePseudoConsole 位于 kernel32.lib，MSVC 默认链接，
# 无需额外 target_link_libraries）；其余平台继续使用 KPty 系列实现。
if(WIN32 AND CUBESHELL_WITH_LOCALPTY)
    target_sources(qtermwidget PRIVATE ConPtyProcess.cpp)
endif()

target_include_directories(qtermwidget PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(qtermwidget PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

# Linux/Unix 的 KPty 用 openpty()/forkpty()（KPty.cpp，<pty.h>）。这两个符号定义在
# libutil（glibc）里，不在 libc——必须显式链接，否则链接期 undefined reference to
# `openpty'。Ubuntu 上常被工具链/Qt 间接带入而侥幸通过，Rocky/RHEL 等更「干净」的
# 链接环境会直接报错。macOS 的 openpty 在 libSystem（无需额外库），故仅 Unix 非 Apple 链接。
if(UNIX AND NOT APPLE AND CUBESHELL_WITH_LOCALPTY)
    find_library(UTIL_LIBRARY util)   # → /usr/lib64/libutil.so（glibc）
    if(UTIL_LIBRARY)
        target_link_libraries(qtermwidget PUBLIC ${UTIL_LIBRARY})
    else()
        # 极少数环境找不到 libutil.so（如静态/精简镜像），退化为按名链接
        target_link_libraries(qtermwidget PUBLIC util)
    endif()
endif()

target_compile_definitions(qtermwidget PRIVATE
    QTERMWIDGET_VERSION_MAJOR=2
    QTERMWIDGET_VERSION_MINOR=2
    QTERMWIDGET_VERSION_PATCH=0
    # 开发期兜底路径：指向源码树 cpp/src/qtermwidget/ 下的资源目录
    # （与 Python 版 qtermwidget/ 包内布局一致：color-schemes、kb-layouts、
    # 根级 default.keytab）。运行期优先探测 bundle Resources/ 与可执行文件旁
    # resources/，见 tools.cpp。
    KB_LAYOUT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/kb-layouts"
    COLORSCHEMES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/color-schemes"
    TRANSLATIONS_DIR="${CMAKE_SOURCE_DIR}/resources/translations/qtermwidget"
)

# 本地终端后端的可用性。PUBLIC：Session.h/qtermwidget.h 的使用者（cube_core 的
# SshBridge/SerialBridge、cube_ui 的终端面板）也要按此摘除「起本地 shell」的调用。
if(CUBESHELL_WITH_LOCALPTY)
    target_compile_definitions(qtermwidget PUBLIC CUBESHELL_WITH_LOCALPTY=1)
endif()
