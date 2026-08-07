# Business-logic layer — C++ port of the Python `core/` and `function/`
# packages: SSH bridge, sessions, config, tunnels, AI integration, RDP glue.

set(CORE_SOURCES
    # --- ssh ---
    ssh/SshClient.cpp           # libssh2 wrapper (replaces paramiko.SSHClient)
    ssh/SshBridge.cpp           # port of core/paramiko_bridge.py
    ssh/ShellMfaWatcher.cpp     # port of core/shell_mfa_watcher.py
    ssh/SftpClient.cpp          # port of SFTP operations (function/ssh_func.py)
    ssh/PortForwarder.cpp       # port of core/forwarder.py (local/remote/dynamic)
    ssh/ForwarderManager.cpp    # port of core/forwarder.py (manager)
    ssh/CommandExecutor.cpp     # port of core/automation.py command execution
    ssh/RemoteMonitor.cpp       # port of remote monitoring collection
    ssh/MfaPromptHandler.cpp    # port of MFA prompt handling
    ssh/TunnelPool.cpp          # port of tunnel pool management
    ssh/SftpUploaderCore.cpp    # port of core/uploader (chunked/resumable upload)

    # --- config / persistence ---
    config/PickleReader.cpp     # minimal pickle reader for config.dat
    config/DeviceConfigStore.cpp # device config (read pickle / write JSON)
    config/GlobalState.cpp      # port of core/vars.py + function/theme.py state
    config/GroupManager.cpp     # port of core/group_manager.py
    config/FrequentlyUsedCommands.cpp # port of core/frequently_used_commands.py
    config/ConfigUtil.cpp       # JSON/TOML config loading helpers
    config/Secrets.cpp          # port of keyring-backed secret storage

    # --- util ---
    util/StringUtil.cpp         # port of function/util.py string helpers
    util/FileUtil.cpp           # port of function/traversal.py + core/compressor.py
    util/DataParser.cpp         # port of function/parse_data.py
    util/ThemeManager.cpp       # port of function/theme.py

    # --- terminal (命令提示数据层) ---
    terminal/CommandIndex.cpp   # port of function/ssh_prompt_client.py::load_linux_commands + _compute_suggestions
    terminal/CommandHistory.cpp # port of cube-shell.py::_add_history_entry/_history_suggestions 等

    # --- url dispatch ---
    url_dispatch/UrlHandler.cpp   # port of core/url_dispatch/url_handler.py
    url_dispatch/BastionClient.cpp # port of core/url_dispatch/bastion_client.py

    # --- platform integration ---
    platform/WindowsIntegration.cpp  # port of core/windows_integration.py (stub off-Windows)
    platform/UrlSchemeRegistrar.cpp  # port of core/url_dispatch/url_scheme_register.py

    # --- ai ---
    ai/SseClient.cpp            # generic SSE client (replaces openai SDK streaming)
    ai/AiPreferences.cpp        # port of core/ai/prefs.py + secrets.py
    ai/AiChatWorker.cpp         # port of core/ai/worker.py
    ai/CommandSafetyChecker.cpp # port of core/ai/safety.py
    ai/ChatHistory.cpp          # port of core/ai/conversation.py (+JSON persistence)
    ai/SensitiveDataMasker.cpp  # 发往大模型前的敏感信息脱敏（私钥/密码/IP）
    ai/MarkdownRenderer.cpp     # port of core/ai/ai_panel.py rendering logic
    ai/SshAiAgent.cpp           # port of core/ai/ssh_agent.py
    ai/TerminalExecutor.cpp     # port of core/ai/ssh_agent.py::_TerminalExecutor
    ai/CommandConfirmDialog.cpp # port of core/ai/confirm_dialog.py
    ai/AiChatPanel.cpp          # port of core/ai/ai_panel.py UI
    ai/ServerProfileBuilder.cpp # port of core/ai/server_profile.py

    # --- docker (compose YAML 解析；纯数据层，不起子进程) ---
    docker/ComposeYaml.cpp      # port of yaml.safe_load/dump + util.get_compose_service

    # --- update ---
    update/UpdateChecker.cpp    # port of core/update/ (version + github_api + platform_match)
    update/UpdateDownloader.cpp # port of core/update/installer.py (下载+安装器调起)

    # --- hermes（HTTP/SSE 部分；本地 CLI 后端见 LOCALPROC 分支） ---
    hermes/HermesBackend.cpp    # port of core/hermes/backend.py (+自带 SSE 解析)
    hermes/HermesTaskModel.cpp  # port of core/hermes/cron_widget.py (CronWorker)
    hermes/HermesGateway.cpp    # port of core/hermes/gateway_widget.py + skills_widget.py
    hermes/HermesPanel.cpp      # port of core/hermes/ UI (7 Tab 容器面板)
    hermes/HermesAgentWidget.cpp  # port of core/hermes/agent_widget.py
    hermes/HermesStatusWidget.cpp # port of core/hermes/status_widget.py
    hermes/HermesConfigWidget.cpp # port of core/hermes/config_widget.py
    hermes/HermesMemoryWidget.cpp # port of core/hermes/memory_widget.py

    # --- claude code ---
    claude_code/ClaudeCodeBackend.cpp  # port of core/claude_code/backend.py (CLI + JSON-line)
    claude_code/ClaudeCodeRenderer.cpp # 轻量 Markdown→HTML (聊天渲染)
    claude_code/ClaudeCodePanel.cpp    # port of core/claude_code/claude_code_panel.py (容器+连接模式)
    claude_code/StatusPanel.cpp        # port of core/claude_code/status_widget.py (状态 Tab)
    claude_code/SessionPanel.cpp       # port of core/claude_code/session_widget.py (会话 Tab)
    claude_code/SettingsPanel.cpp      # port of core/claude_code/settings_widget.py (设置 Tab)
    claude_code/McpPanel.cpp           # port of core/claude_code/mcp_widget.py (MCP Tab)
    claude_code/McpEditDialog.cpp      # port of core/claude_code/mcp_widget.py::McpEditDialog
)

# 语音输入（QAudioSource 采集 + HTTP 识别）。HarmonyOS 上 QtMultimedia
# 支持不完整，CUBESHELL_WITH_VOICE=OFF 时整体不编译，也不链接 Qt6::Multimedia。
if(CUBESHELL_WITH_VOICE)
    list(APPEND CORE_SOURCES
        ai/VoiceInput.cpp       # port of core/ai/voice_input.py
    )
endif()

# 依赖 QProcess 起本地子进程的模块。HarmonyOS 沙箱禁止 exec，
# CUBESHELL_WITH_LOCALPROC=OFF 时整体不编译，UI 侧入口由同名宏摘除。
#   * frp 内网穿透  —— 需要下载并运行 frpc/frps 二进制
#   * docker CLI    —— 需要调用本机 docker 命令（ComposeYaml 纯解析层不受影响，
#                      走 SSH 管理远程 Docker 的路径也不在此列）
if(CUBESHELL_WITH_LOCALPROC)
    list(APPEND CORE_SOURCES
        # --- tunnels / forwarding ---
        forwarder/FrpManager.cpp    # port of core/frp_manager.py
        forwarder/FrpInstaller.cpp  # port of core/frp_manager.py (下载/解压/远端部署)
        forwarder/FrpConnectWorker.cpp # port of cube-shell.py::FRPConnectThread

        # --- docker (CLI 驱动部分) ---
        docker/DockerManager.cpp    # port of core/docker/ (CLI + compose 编辑)
    )
endif()

# Finder integration: Objective-C++ on macOS, plain stub elsewhere.
# 对应Python: core/finder_integration.py (platform.system() == 'Darwin' 分支)
if(APPLE)
    enable_language(OBJCXX)
    list(APPEND CORE_SOURCES platform/FinderIntegration_mac.mm)
else()
    list(APPEND CORE_SOURCES platform/FinderIntegration.cpp)
endif()

# RDP support is opt-in (see CUBESHELL_WITH_RDP in the top-level CMakeLists).
# 对应Python: core/rdp/rdp_client.py
if(CUBESHELL_WITH_RDP)
    list(APPEND CORE_SOURCES
        rdp/RdpClient.cpp   # port of core/rdp/rdp_client.py (connection mgmt)
        rdp/RdpPanel.cpp    # port of core/rdp/rdp_client.py::RDPWidget (UI)
    )
endif()

# 串口支持（CUBESHELL_WITH_SERIAL，默认 ON）。Python 版无对应实现，
# 设计上取 RdpClient 的异步信号形态 + SshBridge 的终端接线方式。
if(CUBESHELL_WITH_SERIAL)
    list(APPEND CORE_SOURCES
        serial/SerialClient.cpp   # QSerialPort 封装（端口枚举/热插拔/日志）
        serial/SerialBridge.cpp   # 串口字节流 ↔ qtermwidget Session
    )
endif()

set(CORE_PLACEHOLDER ${CMAKE_CURRENT_SOURCE_DIR}/core_placeholder.cpp)
if(NOT CORE_SOURCES)
    set(CORE_SOURCES ${CORE_PLACEHOLDER})
endif()

add_library(cube_core STATIC ${CORE_SOURCES})

target_include_directories(cube_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(cube_core PUBLIC
    Qt6::Core
    Qt6::Gui       # ThemeManager (QPalette/QColor)
    Qt6::Widgets   # ThemeManager (QApplication)
    Qt6::Network
    Qt6::Concurrent
    qtermwidget
)

# 语音输入（VoiceInput，QAudioSource/QMediaDevices）。宏用 PUBLIC：
# cube_ui 侧的 AI 面板要靠它摘除麦克风按钮等入口代码。
if(CUBESHELL_WITH_VOICE)
    target_compile_definitions(cube_core PUBLIC CUBESHELL_WITH_VOICE=1)
    target_link_libraries(cube_core PUBLIC Qt6::Multimedia)
endif()

# 本地子进程工具集（frp / docker CLI）。同样 PUBLIC：cube_ui 的
# NatDialog / DockerManagerDialog 等入口要按此摘除。
if(CUBESHELL_WITH_LOCALPROC)
    target_compile_definitions(cube_core PUBLIC CUBESHELL_WITH_LOCALPROC=1)
endif()

# ComposeYaml 解析任意 Compose YAML。对应Python: PyYAML。
# 目标名已在顶层 CMakeLists 统一为 yaml-cpp::yaml-cpp（find_package/FetchContent 兼容）。
target_link_libraries(cube_core PRIVATE yaml-cpp::yaml-cpp)

# AiPreferences 在开发环境下回退到源码树的 conf/llm_providers.json（安装包
# 内则用 appDir 相对路径探测）。对应Python: prefs.py 直接读工程根 conf/。
target_compile_definitions(cube_core PRIVATE
    CUBESHELL_SOURCE_CONF_DIR="${CMAKE_SOURCE_DIR}/conf"
)

# Secrets.cpp talks to the macOS Keychain directly; FinderIntegration_mac.mm
# needs Cocoa/AppKit (NSBundle/NSWorkspace/NSFileManager) and CoreServices
# (LSRegisterURL).
if(APPLE)
    target_link_libraries(cube_core PRIVATE
        "-framework Security" "-framework CoreFoundation"
        "-framework Cocoa" "-framework AppKit" "-framework CoreServices"
    )
endif()

if(CUBESHELL_WITH_SSH)
    target_compile_definitions(cube_core PUBLIC CUBESHELL_WITH_SSH=1)
    target_include_directories(cube_core PRIVATE ${LIBSSH2_INCLUDE_DIRS})
    # Static libssh2 pulls in OpenSSL and zlib.
    find_package(OpenSSL REQUIRED)
    find_package(ZLIB REQUIRED)
    target_link_libraries(cube_core PRIVATE
        ${LIBSSH2_LIBRARIES}
        OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB
    )
endif()

# 串口后端（CUBESHELL_WITH_SERIAL=ON）。Qt6::SerialPort 与宏都用 PUBLIC：
# cube_ui 里的串口面板既要包含 QSerialPort 头（枚举类型出现在 SerialSettings
# 里），也要用 CUBESHELL_WITH_SERIAL 摘除入口代码。
if(CUBESHELL_WITH_SERIAL)
    target_compile_definitions(cube_core PUBLIC CUBESHELL_WITH_SERIAL=1)
    target_link_libraries(cube_core PUBLIC Qt6::SerialPort)
endif()

# ---------------------------------------------------------------------------
# RDP backend resolution (only when CUBESHELL_WITH_RDP=ON)
# ---------------------------------------------------------------------------
# RdpClient has two backends (see src/core/rdp/RdpClient.h):
#   1. FreeRDP library  -> compiled in when the package is found; the client
#                          renders frames itself and emits frameUpdated().
#   2. command line     -> fallback, QProcess drives xfreerdp / mstsc.
# FreeRDP is frequently absent (no brew formula on macOS), so the lookup is
# QUIET and the fallback keeps the build green either way.
if(CUBESHELL_WITH_RDP)
    target_compile_definitions(cube_core PUBLIC CUBESHELL_WITH_RDP=1)

    # cJSON 已由根 CMakeLists 在 add_subdirectory() 之前导入（那里才能让 src/app
    # 的最终链接也看见 cjson 目标）。这里再调一次只为让本目录单独配置时
    # 仍能拿到目标；根作用域已找到时此调用是无副作用的重复。
    # 关键约束不变：必须先于 find_package(FreeRDP)，否则 WinPR 的
    # INTERFACE_LINK_LIBRARIES 里的裸名 `cjson` 会变成不带路径的 cjson.lib。
    find_package(cJSON CONFIG QUIET)

    find_package(FreeRDP CONFIG QUIET)
    if(NOT FreeRDP_FOUND)
        # Distro packages ship pkg-config files rather than a CMake config.
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(FREERDP QUIET freerdp3 freerdp2 freerdp)
        endif()
    endif()

    if(FreeRDP_FOUND)
        message(STATUS "FreeRDP found (CMake config): RDP uses the library backend")
        target_compile_definitions(cube_core PRIVATE CUBESHELL_HAVE_FREERDP=1)
        target_link_libraries(cube_core PRIVATE freerdp winpr)
        # freerdp_client_load_channels()（PreConnect 里加载虚拟通道插件）不在 freerdp
        # 主库里，而在单独打包的 freerdp-client 中，须另找一次并单独链接。
        find_package(FreeRDP-Client CONFIG QUIET)
        if(TARGET freerdp-client)
            message(STATUS "FreeRDP-Client found: virtual channel loading enabled")
            target_compile_definitions(cube_core PRIVATE CUBESHELL_HAVE_FREERDP_CLIENT=1)
            target_link_libraries(cube_core PRIVATE freerdp-client)
        endif()
        # Static builds do not re-export cJSON transitively; link it explicitly.
        # vcpkg exports it as the lowercase, unnamespaced target `cjson`.
        if(TARGET cjson)
            target_link_libraries(cube_core PRIVATE cjson)
        endif()
    elseif(FREERDP_FOUND)
        message(STATUS "FreeRDP found (pkg-config ${FREERDP_VERSION}): RDP uses the library backend")
        target_compile_definitions(cube_core PRIVATE CUBESHELL_HAVE_FREERDP=1)
        target_include_directories(cube_core PRIVATE ${FREERDP_INCLUDE_DIRS})
        target_link_libraries(cube_core PRIVATE ${FREERDP_LIBRARIES})
        target_link_directories(cube_core PRIVATE ${FREERDP_LIBRARY_DIRS})
        # 同上：虚拟通道加载入口在 freerdp-client 里，freerdp3.pc 不带它。
        pkg_check_modules(FREERDP_CLIENT QUIET freerdp-client3)
        if(FREERDP_CLIENT_FOUND)
            message(STATUS "FreeRDP-Client found (pkg-config): virtual channel loading enabled")
            target_compile_definitions(cube_core PRIVATE CUBESHELL_HAVE_FREERDP_CLIENT=1)
            target_include_directories(cube_core PRIVATE ${FREERDP_CLIENT_INCLUDE_DIRS})
            target_link_libraries(cube_core PRIVATE ${FREERDP_CLIENT_LIBRARIES})
            target_link_directories(cube_core PRIVATE ${FREERDP_CLIENT_LIBRARY_DIRS})
        endif()
    else()
        message(STATUS "FreeRDP not found: RDP falls back to xfreerdp/mstsc command line")
    endif()

    # 第三道兜底：若调用方自定义了 VCPKG_INSTALLED_DIR（不在根 CMakeLists 的
    # ${CMAKE_BINARY_DIR}/vcpkg_installed 下），根里的 link_directories 就指不到位。
    # PUBLIC：cube_core 是静态库，搜索路径必须传递到 cube-shell.exe 的最终链接。
    if(VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
        target_link_directories(cube_core PUBLIC
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
        )
    elseif(DEFINED CMAKE_PREFIX_PATH)
        # Fallback: resolve cjson to an absolute path ourselves.
        find_library(CJSON_LIBRARY NAMES cjson)
        if(CJSON_LIBRARY)
            target_link_libraries(cube_core PRIVATE ${CJSON_LIBRARY})
        endif()
    endif()
endif()
