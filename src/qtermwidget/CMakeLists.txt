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
    # screen.cpp
    # vt102_emulation.cpp
    # emulation.cpp
    # terminal_character_decoder.cpp
    Character.cpp
    CharacterColor.cpp
    konsole_wcwidth.cpp
    tools.cpp
    KProcess.cpp
    KPty.cpp
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
    KPtyDevice.cpp
    KPtyProcess.cpp
    Pty.cpp
    SearchBar.cpp
    # --- wave 3: top-level ---
    Session.cpp
    TerminalDisplay.cpp
    HistorySearch.cpp
    qtermwidget.cpp

    # --- rendering / widget ---
    # terminal_display.cpp
    # qtermwidget.cpp
    # filter.cpp
    # color_scheme.cpp
    # keyboard_translator.cpp

    # --- process / pty ---
    # session.cpp
    # shell_command.cpp
    # kprocess.cpp
    # kpty.cpp
    # kpty_device.cpp
    # kptyprocess.cpp
    # pty.cpp

    # --- misc ---
    # tools.cpp
    # search_bar.cpp
    # history_search.cpp
)

# Placeholder translation unit kept only until enough real sources exist.
# Currently unused since Character.cpp / CharacterColor.cpp are enabled.
set(QTERMWIDGET_PLACEHOLDER ${CMAKE_CURRENT_SOURCE_DIR}/qtermwidget_placeholder.cpp)
if(NOT QTERMWIDGET_SOURCES)
    set(QTERMWIDGET_SOURCES ${QTERMWIDGET_PLACEHOLDER})
endif()

add_library(qtermwidget STATIC ${QTERMWIDGET_SOURCES})

# Windows 走 ConPTY（CreatePseudoConsole 位于 kernel32.lib，MSVC 默认链接，
# 无需额外 target_link_libraries）；其余平台继续使用 KPty 系列实现。
if(WIN32)
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
