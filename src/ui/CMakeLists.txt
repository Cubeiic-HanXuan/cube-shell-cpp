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
    file_browser_common.cpp   # 两棵文件树共用的筛选/排序逻辑（见 file_browser_common.h）
    file_icons.cpp
    add_device_dialog.cpp
    # 代理配置控件（设备对话框与设置页共用同一个，见其头注释）
    ProxySettingsWidget.cpp
    status_box_item.cpp
    LanguageManager.cpp
    # --- TCP/Telnet UI（无条件编译，理由同 src/core/CMakeLists.txt 的 net/ 段） ---
    net_terminal_widget.cpp         # 工具栏 + QTermWidget + 状态栏
    dialogs/NetConnectDialog.cpp    # “新建 Telnet/TCP 连接”对话框 + netcombo 助手
    # --- 终端命令智能提示 (对应Python: cube-shell.py::_SuggestionPopup/SSHQTermWidget 提示逻辑) ---
    suggestion_popup.cpp
    terminal_command_suggest.cpp
    # 终端内就地读一行（SSH 连接时问密码，不弹对话框）
    terminal_prompt.cpp

    # --- dialogs (对应Python: ui/*.py 各对话框) ---
    dialogs/ConfirmDialog.cpp
    dialogs/AuthDialog.cpp
    dialogs/CompressDialog.cpp
    dialogs/AboutDialog.cpp
    dialogs/SettingsDialog.cpp
    dialogs/HostKeyDialog.cpp
    dialogs/AddTunnelDialog.cpp
    dialogs/TunnelConfigWidget.cpp
    dialogs/ProcessManagerDialog.cpp
    dialogs/LinuxCommandsDialog.cpp
    dialogs/SnippetsDialog.cpp     # 用户自定义参数化片段（Snippets）管理 + 参数填写
    dialogs/SshKeyManagerDialog.cpp # SSH 密钥管理（生成/指纹/复制公钥/ssh-copy-id 部署）
    dialogs/AiSettingsDialog.cpp

    # --- editors (对应Python: ui/code_editor.py / ui/text_editor.py) ---
    editors/CodeEditor.cpp
    editors/TextEditor.cpp
)

# Docker/内网穿透相关对话框与编辑器：后端 DockerManager/FrpManager 在
# CUBESHELL_WITH_LOCALPROC=OFF（鸿蒙沙箱禁止 exec）时不编译，UI 同步摘除。
# （TunnelConfigWidget 保留：主体走 TunnelPool/SSH 端口转发，frp 仅作可选
#  状态展示，widget 内部按 CUBESHELL_WITH_LOCALPROC 宏自行摘除。）
if(CUBESHELL_WITH_LOCALPROC)
    list(APPEND UI_SOURCES
        dialogs/DockerManagerDialog.cpp
        dialogs/DockerSoftDialog.cpp
        dialogs/NatDialog.cpp

        # --- Kubernetes 集群管理（对应 docs/Kubernetes功能实现方案.md §5） ---
        dialogs/KubeManagerDialog.cpp
        dialogs/KubeLogViewer.cpp
        editors/KubeYamlEditor.cpp

        # --- Docker Compose 可视化编辑器 (对应Python: core/docker/docker_compose_editor.py) ---
        editors/DockerComposeEditor.cpp
        editors/ServiceConfigWidget.cpp
        editors/ServiceSearchDialog.cpp
        editors/DockerDaemonConfigDialog.cpp
    )
endif()

# 串口 UI（CUBESHELL_WITH_SERIAL=ON 时才编译；宏由 cube_core PUBLIC 传递过来）。
if(CUBESHELL_WITH_SERIAL)
    list(APPEND UI_SOURCES
        serial_terminal_widget.cpp      # 工具栏 + QTermWidget + 状态栏
        dialogs/SerialConnectDialog.cpp # “新建串口连接”菜单的临时连接对话框
    )
endif()

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

# CUBESHELL_RESOURCE_DIR 指向源码树 resources/，供 LanguageManager 开发期探测
# 翻译文件。（命令库 conf/linux_commands.json 的探测已下移到 cube_core 的
# FrequentlyUsedCommands::defaultPath()，这里不再需要 CUBESHELL_SOURCE_CONF_DIR。）
target_compile_definitions(cube_ui PRIVATE
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
