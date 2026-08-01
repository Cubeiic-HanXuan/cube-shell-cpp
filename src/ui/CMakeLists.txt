# UI layer — C++ port of the Python `ui/` package and the widgets embedded in
# cube-shell.py: main window, tab widget, config dialogs, SSHQTermWidget.

set(UI_SOURCES
    main_window.cpp
    device_list_widget.cpp
    terminal_tab_widget.cpp
    ssh_terminal_widget.cpp
    ssh_session_tab.cpp
    sftp_browser_widget.cpp
    local_file_browser_widget.cpp
    file_icons.cpp
    add_device_dialog.cpp
    status_box_item.cpp
    LanguageManager.cpp

    # --- 终端命令智能提示 (对应Python: cube-shell.py::_SuggestionPopup/SSHQTermWidget 提示逻辑) ---
    suggestion_popup.cpp
    terminal_command_suggest.cpp

    # --- dialogs (对应Python: ui/*.py 各对话框) ---
    dialogs/ConfirmDialog.cpp
    dialogs/AuthDialog.cpp
    dialogs/CompressDialog.cpp
    dialogs/AboutDialog.cpp
    dialogs/SettingsDialog.cpp
    dialogs/AddTunnelDialog.cpp
    dialogs/TunnelConfigWidget.cpp
    dialogs/DockerManagerDialog.cpp
    dialogs/DockerSoftDialog.cpp
    dialogs/NatDialog.cpp
    dialogs/ProcessManagerDialog.cpp
    dialogs/LinuxCommandsDialog.cpp
    dialogs/AiSettingsDialog.cpp

    # --- editors (对应Python: ui/code_editor.py / ui/text_editor.py) ---
    editors/CodeEditor.cpp
    editors/TextEditor.cpp

    # --- Docker Compose 可视化编辑器 (对应Python: core/docker/docker_compose_editor.py) ---
    editors/DockerComposeEditor.cpp
    editors/ServiceConfigWidget.cpp
    editors/ServiceSearchDialog.cpp
    editors/DockerDaemonConfigDialog.cpp
)

# 图标资源（resources/icons/icons.qrc，cpp 项目自带拷贝）挂在可执行目标上：
# 静态库里的 qrc 需要 Q_INIT_RESOURCE 才能避开链接器丢弃初始化符号。
# 见 src/app/CMakeLists.txt。

add_library(cube_ui STATIC ${UI_SOURCES})

target_include_directories(cube_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/qtermwidget
)

target_link_libraries(cube_ui PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    qtermwidget
    cube_core
)

# LinuxCommandsDialog 在开发环境下回退到源码树的 conf/linux_commands.json
#（与 cube_core 的 AiPreferences 探测方式一致）。CUBESHELL_RESOURCE_DIR 指向
# 源码树 resources/，供 LanguageManager 开发期探测翻译文件。
target_compile_definitions(cube_ui PRIVATE
    CUBESHELL_SOURCE_CONF_DIR="${CMAKE_SOURCE_DIR}/conf"
    CUBESHELL_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resources"
)

# --- i18n（翻译文件已拷入 cpp/resources/translations/app/，独立于 Python 版） ---
#
# C++ 侧所有用户可见字符串已用 tr() 包裹。要把 C++ 新增字符串合入现有翻译，
# 走标准 lupdate/lrelease 流程即可：
#
#   1. 提取/更新 .ts（把 C++ 源加入扫描范围）：
#        lupdate ${CMAKE_SOURCE_DIR}/src -ts <lang>.ts
#   2. 编译 .qm 并放入 ${CMAKE_SOURCE_DIR}/resources/translations/app/
#   3. 运行时加载：LanguageManager::initialize() 自动探测（bundle
#      Resources/translations/app → 可执行文件旁 resources/translations/app →
#      源码树 cpp/resources/translations/app），按 app_{lang_code}.qm →
#      app_{short_code}.qm 顺序装载（见 LanguageManager.cpp）。
#
# 当前阶段的 24 种语言 app_*.qm 已从 Python 侧拷入 resources/translations/app/，
# Python 侧已有词条（窗口标题、菜单、常用按钮等）在 C++ 侧同文案处自动命中翻译。
