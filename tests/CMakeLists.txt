# Headless smoke tests for the ported qtermwidget library.

qt_add_executable(qterm_smoke
    qterm_smoke.cpp
)

target_include_directories(qterm_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/src/qtermwidget
)

target_link_libraries(qterm_smoke PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    qtermwidget
)

# Pickle reader + device config store test.
qt_add_executable(pickle_test
    pickle_test.cpp
)
target_include_directories(pickle_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(pickle_test PRIVATE
    Qt6::Core
    cube_core
)

# Config store CRUD + host:port parse/format test.
qt_add_executable(config_store_test
    config_store_test.cpp
)
target_include_directories(config_store_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(config_store_test PRIVATE
    Qt6::Core
    cube_core
)

# GroupManager / FrequentlyUsedCommands / ConfigUtil / GlobalState / Secrets test.
qt_add_executable(config_group_test
    config_group_test.cpp
)
target_include_directories(config_group_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(config_group_test PRIVATE
    Qt6::Core
    cube_core
)

# StringUtil / FileUtil / DataParser / ThemeManager test.
qt_add_executable(util_test
    util_test.cpp
    # ThemeManager::styleSheet() 读的是 qdarktheme 导出资源，一并编进测试可执行
    # 文件，使测试走真实资源路径而不是内置兜底 QSS。
    # 对应资源注册：src/app/CMakeLists.txt
    ${CMAKE_SOURCE_DIR}/resources/qdarktheme/qdarktheme.qrc
)
target_include_directories(util_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(util_test PRIVATE
    Qt6::Core
    Qt6::Gui
    cube_core
)

# End-to-end SSH integration test (needs a reachable sshd; see file header).
qt_add_executable(ssh_integration_test
    ssh_integration_test.cpp
)
target_include_directories(ssh_integration_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/qtermwidget
)
target_link_libraries(ssh_integration_test PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    qtermwidget
    cube_core
    cube_ui
)

# SFTP integration test (needs a reachable sshd; see file header).
qt_add_executable(sftp_integration_test
    sftp_integration_test.cpp
)
target_include_directories(sftp_integration_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(sftp_integration_test PRIVATE
    Qt6::Core
    cube_core
)

# CommandExecutor / RemoteMonitor / MfaPromptHandler test (pure-logic part
# always runs; network part needs a reachable sshd, exits 2 to SKIP).
qt_add_executable(command_executor_test
    command_executor_test.cpp
)
target_include_directories(command_executor_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(command_executor_test PRIVATE
    Qt6::Core
    cube_core
)

# SftpUploaderCore test (pure-logic part always runs; real SFTP upload part
# needs a reachable sshd, exits 2 to SKIP).
qt_add_executable(uploader_test
    uploader_test.cpp
)
target_include_directories(uploader_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(uploader_test PRIVATE
    Qt6::Core
    cube_core
)

# 上传进行中销毁上传器的卡死回归测试（需要 sshd；连不上则直接通过跳过）。
qt_add_executable(upload_teardown_test
    upload_teardown_test.cpp
)
target_include_directories(upload_teardown_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(upload_teardown_test PRIVATE
    Qt6::Core
    cube_core
)

# 并行上传吞吐对比工具（手动运行，需要活的 sshd；不注册进 ctest）。
qt_add_executable(parallel_bench
    parallel_bench.cpp
)
target_include_directories(parallel_bench PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(parallel_bench PRIVATE
    Qt6::Core
    cube_core
)

# SftpTransferPool / 并行分片规划的纯逻辑测试（无网络：验证降级与水位语义）。
qt_add_executable(transfer_pool_test
    transfer_pool_test.cpp
)
target_include_directories(transfer_pool_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(transfer_pool_test PRIVATE
    Qt6::Core
    cube_core
)

# 主 session 通道多路复用（jms token / MFA 并行路径）的集成测试。
# 强制 setCloningEnabled(false) 走通道路径；需要活的 sshd，连不上 exit 2 跳过。
qt_add_executable(channel_multiplex_test
    channel_multiplex_test.cpp
)
target_include_directories(channel_multiplex_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(channel_multiplex_test PRIVATE
    Qt6::Core
    cube_core
)

# 「测试连接」后台探测：本地 QTcpServer 验证 TCP 可达/不可达、SSH refused、
# 重入闸门（无外部依赖；SSH 成功路径可选，由 CUBESSH_* 环境变量驱动）。
qt_add_executable(connection_tester_test
    connection_tester_test.cpp
)
target_include_directories(connection_tester_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(connection_tester_test PRIVATE
    Qt6::Core
    Qt6::Network
    cube_core
)

# UrlHandler / BastionClient unit test (pure logic, no network).
qt_add_executable(url_dispatch_test
    url_dispatch_test.cpp
)
target_include_directories(url_dispatch_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(url_dispatch_test PRIVATE
    Qt6::Core
    cube_core
)

# Platform integration test (Finder/Windows/UrlScheme facade; read-only probes,
# never touches the user's real registry / ~/Library/Services).
qt_add_executable(platform_test
    platform_test.cpp
)
target_include_directories(platform_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(platform_test PRIVATE
    Qt6::Core
    cube_core
)

# SSE parser unit test (line buffering / CRLF / [DONE] / finish flush;
# pure logic, no network).
qt_add_executable(sse_parser_test
    sse_parser_test.cpp
)
target_include_directories(sse_parser_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(sse_parser_test PRIVATE
    Qt6::Core
    cube_core
)

# CommandSafetyChecker rule engine + command extraction test (pure logic).
qt_add_executable(safety_checker_test
    safety_checker_test.cpp
)
target_include_directories(safety_checker_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(safety_checker_test PRIVATE
    Qt6::Core
    cube_core
)

# SensitiveDataMasker 脱敏规则 test（私钥/密码/IP；pure logic）。
qt_add_executable(sensitive_masker_test
    sensitive_masker_test.cpp
)
target_include_directories(sensitive_masker_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(sensitive_masker_test PRIVATE
    Qt6::Core
    cube_core
)

# ComposeYaml parse/dump/round-trip + predefined services loading test
# (yaml-cpp comes in transitively through cube_core; pure logic).
qt_add_executable(compose_yaml_test
    compose_yaml_test.cpp
)
target_include_directories(compose_yaml_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(compose_yaml_test PRIVATE
    Qt6::Core
    cube_core
)

# DockerManager static parse helpers + compose path rule test (pure logic,
# no docker CLI / network)。DockerManager 本体在 CUBESHELL_WITH_LOCALPROC=OFF
#（鸿蒙）时不编译，测试同步摘除。
if(CUBESHELL_WITH_LOCALPROC)
qt_add_executable(docker_manager_test
    docker_manager_test.cpp
)
target_include_directories(docker_manager_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(docker_manager_test PRIVATE
    Qt6::Core
    cube_core
)
endif()

# FRP 模块：包名映射 + FrpInstaller 纯函数 + traversal 配置逐字对比 +
# frpc.toml 写入/回读往返（离线，不发起网络请求）。
# CUBESHELL_WITH_LOCALPROC=OFF（鸿蒙）时 FrpInstaller/FrpManager 不编译，测试同步摘除。
if(CUBESHELL_WITH_LOCALPROC)
qt_add_executable(frp_installer_test
    frp_installer_test.cpp
)
target_include_directories(frp_installer_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(frp_installer_test PRIVATE
    Qt6::Core
    cube_core
)
endif()

# CommandIndex / CommandHistory / extractCommandFromPrompt unit test
# (pure logic, no network, no GUI display).
qt_add_executable(command_suggest_test
    command_suggest_test.cpp
)
target_include_directories(command_suggest_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/ui
    ${CMAKE_SOURCE_DIR}/src/qtermwidget
)
target_link_libraries(command_suggest_test PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    cube_ui
    cube_core
)

# SFTP 面板「代理端不提供文件浏览」判定（纯逻辑，无网络、无显示）。
qt_add_executable(sftp_browser_test
    sftp_browser_test.cpp
)
target_include_directories(sftp_browser_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/ui
    ${CMAKE_SOURCE_DIR}/src/qtermwidget
)
target_link_libraries(sftp_browser_test PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    cube_ui
    cube_core
)

# Screen reflow unit test (resizeImage with reflow=true; pure logic, no GUI).
qt_add_executable(screen_reflow_test
    screen_reflow_test.cpp
)
target_include_directories(screen_reflow_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/qtermwidget
)
target_link_libraries(screen_reflow_test PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    qtermwidget
)

# 终端内容搜索：命中定位换算 + 序号判定 + 正则构造规则（纯逻辑，无 PTY）。
qt_add_executable(terminal_search_test
    terminal_search_test.cpp
)
target_include_directories(terminal_search_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/qtermwidget
)
target_link_libraries(terminal_search_test PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    qtermwidget
)

# 更新检查：仓库地址、版本号解析/比较、Release JSON 解析、平台 asset 匹配
# （纯逻辑，不发起网络请求）。
qt_add_executable(update_checker_test
    update_checker_test.cpp
)
target_include_directories(update_checker_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(update_checker_test PRIVATE
    Qt6::Core
    cube_core
)

# 明文密码 → 钥匙串的迁移正确性测试。跑真实钥匙串后端（macOS 上一个聚合条目）。
# 重点不是「能迁移」，而是「迁移失败时一条密码都不会少」。
qt_add_executable(secrets_migration_test
    secrets_migration_test.cpp
)
target_include_directories(secrets_migration_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(secrets_migration_test PRIVATE
    Qt6::Core
    cube_core
)

# 串口模块：换行转换纯函数 + SerialSettings 默认值 + 端口枚举 +
# 下拉框取值解析（含手输回归用例）+ DeviceEntry 串口字段 JSON 往返
# （无硬件也能全部跑过）。
if(CUBESHELL_WITH_SERIAL)
    qt_add_executable(serial_test
        serial_test.cpp
    )
    target_include_directories(serial_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src/core
        ${CMAKE_SOURCE_DIR}/src/ui          # serialcombo 辅助函数（下拉框取值）
        ${CMAKE_SOURCE_DIR}/src/qtermwidget   # SerialBridge.h 间接引用 Session
    )
    target_link_libraries(serial_test PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        cube_core
        cube_ui
    )
endif()

# TCP/Telnet 模块：IAC 状态机（含分块边界）、协商应答字节、自动登录提示词匹配、
# defaultPortFor、netcombo 下拉框助手、telnet:// URL 解析、DeviceEntry 的
# tcp/telnet 字段 JSON 往返（无对端也能全部跑过）。
# 不带 if() 包裹——TCP/Telnet 无条件编译（Qt6::Network 是顶层必需组件）。
qt_add_executable(telnet_test
    telnet_test.cpp
)
target_include_directories(telnet_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/ui          # netcombo 助手 + netSettingsFromDevice
    ${CMAKE_SOURCE_DIR}/src/qtermwidget   # TcpBridge.h 间接引用 Session
)
target_link_libraries(telnet_test PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    cube_core
    cube_ui
)

# Telnet 端到端集成测试（需要一个可达的 telnetd；见文件头的 socat 一行命令）。
# 目标不可达时 exit(2) → CTest 记为 SKIPPED。
qt_add_executable(telnet_integration_test
    telnet_integration_test.cpp
)
target_include_directories(telnet_integration_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/qtermwidget
)
target_link_libraries(telnet_integration_test PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    qtermwidget
    cube_core
    cube_ui
)

# ---------------------------------------------------------------------------
# CTest registration
# ---------------------------------------------------------------------------
add_test(NAME qterm_smoke COMMAND qterm_smoke)
add_test(NAME pickle_test COMMAND pickle_test)
add_test(NAME config_store_test COMMAND config_store_test)
add_test(NAME config_group_test COMMAND config_group_test)
add_test(NAME util_test COMMAND util_test)
add_test(NAME url_dispatch_test COMMAND url_dispatch_test)
add_test(NAME platform_test COMMAND platform_test)
add_test(NAME sse_parser_test COMMAND sse_parser_test)
add_test(NAME safety_checker_test COMMAND safety_checker_test)
add_test(NAME sensitive_masker_test COMMAND sensitive_masker_test)
add_test(NAME compose_yaml_test COMMAND compose_yaml_test)
add_test(NAME connection_tester_test COMMAND connection_tester_test)
if(CUBESHELL_WITH_LOCALPROC)
    add_test(NAME docker_manager_test COMMAND docker_manager_test)
    add_test(NAME frp_installer_test COMMAND frp_installer_test)
endif()
add_test(NAME command_suggest_test COMMAND command_suggest_test)
add_test(NAME sftp_browser_test COMMAND sftp_browser_test)
add_test(NAME screen_reflow_test COMMAND screen_reflow_test)
add_test(NAME terminal_search_test COMMAND terminal_search_test)
add_test(NAME update_checker_test COMMAND update_checker_test)
add_test(NAME secrets_migration_test COMMAND secrets_migration_test)
if(CUBESHELL_WITH_SERIAL)
    add_test(NAME serial_test COMMAND serial_test)
endif()
add_test(NAME telnet_test COMMAND telnet_test)

# Integration tests need a reachable sshd; they exit(2) when the target host
# is unavailable, which CTest records as SKIPPED instead of FAILED.
add_test(NAME ssh_integration_test COMMAND ssh_integration_test)
add_test(NAME sftp_integration_test COMMAND sftp_integration_test)
add_test(NAME command_executor_test COMMAND command_executor_test)
add_test(NAME uploader_test COMMAND uploader_test)
add_test(NAME upload_teardown_test COMMAND upload_teardown_test)
add_test(NAME transfer_pool_test COMMAND transfer_pool_test)
add_test(NAME channel_multiplex_test COMMAND channel_multiplex_test)
add_test(NAME telnet_integration_test COMMAND telnet_integration_test)
set_tests_properties(ssh_integration_test sftp_integration_test
    command_executor_test uploader_test upload_teardown_test
    channel_multiplex_test
    telnet_integration_test PROPERTIES
    SKIP_RETURN_CODE 2
)



