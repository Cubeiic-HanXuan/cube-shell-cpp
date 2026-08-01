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

    # --- tunnels / forwarding ---
    forwarder/FrpManager.cpp    # port of core/frp_manager.py
    forwarder/FrpInstaller.cpp  # port of core/frp_manager.py (下载/解压/远端部署)
    forwarder/FrpConnectWorker.cpp # port of cube-shell.py::FRPConnectThread

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
    ai/MarkdownRenderer.cpp     # port of core/ai/ai_panel.py rendering logic
    ai/SshAiAgent.cpp           # port of core/ai/ssh_agent.py
    ai/CommandConfirmDialog.cpp # port of core/ai/confirm_dialog.py
    ai/AiChatPanel.cpp          # port of core/ai/ai_panel.py UI
    ai/VoiceInput.cpp           # port of core/ai/voice_input.py
    ai/ServerProfileBuilder.cpp # port of core/ai/server_profile.py

    # --- docker ---
    docker/DockerManager.cpp    # port of core/docker/ (CLI + compose 编辑)
    docker/ComposeYaml.cpp      # port of yaml.safe_load/dump + util.get_compose_service

    # --- update ---
    update/UpdateChecker.cpp    # port of core/update/ (version + github_api + platform_match)
    update/UpdateDownloader.cpp # port of core/update/installer.py (下载+安装器调起)

    # --- hermes ---
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
    Qt6::Multimedia # VoiceInput (QAudioSource/QMediaDevices)
    qtermwidget
)

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

    find_package(FreeRDP QUIET)
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
    elseif(FREERDP_FOUND)
        message(STATUS "FreeRDP found (pkg-config ${FREERDP_VERSION}): RDP uses the library backend")
        target_compile_definitions(cube_core PRIVATE CUBESHELL_HAVE_FREERDP=1)
        target_include_directories(cube_core PRIVATE ${FREERDP_INCLUDE_DIRS})
        target_link_libraries(cube_core PRIVATE ${FREERDP_LIBRARIES})
        target_link_directories(cube_core PRIVATE ${FREERDP_LIBRARY_DIRS})
    else()
        message(STATUS "FreeRDP not found: RDP falls back to xfreerdp/mstsc command line")
    endif()
endif()
