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
# no docker CLI / network).
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

# FRP 模块：包名映射 + FrpInstaller 纯函数 + traversal 配置逐字对比 +
# frpc.toml 写入/回读往返（离线，不发起网络请求）。
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

# 串口模块：换行转换纯函数 + SerialSettings 默认值 + 端口枚举 +
# DeviceEntry 串口字段 JSON 往返（无硬件也能全部跑过）。
if(CUBESHELL_WITH_SERIAL)
    qt_add_executable(serial_test
        serial_test.cpp
    )
    target_include_directories(serial_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src/core
        ${CMAKE_SOURCE_DIR}/src/qtermwidget   # SerialBridge.h 间接引用 Session
    )
    target_link_libraries(serial_test PRIVATE
        Qt6::Core
        cube_core
    )
endif()

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
add_test(NAME compose_yaml_test COMMAND compose_yaml_test)
add_test(NAME docker_manager_test COMMAND docker_manager_test)
add_test(NAME frp_installer_test COMMAND frp_installer_test)
add_test(NAME command_suggest_test COMMAND command_suggest_test)
add_test(NAME screen_reflow_test COMMAND screen_reflow_test)
if(CUBESHELL_WITH_SERIAL)
    add_test(NAME serial_test COMMAND serial_test)
endif()

# Integration tests need a reachable sshd; they exit(2) when the target host
# is unavailable, which CTest records as SKIPPED instead of FAILED.
add_test(NAME ssh_integration_test COMMAND ssh_integration_test)
add_test(NAME sftp_integration_test COMMAND sftp_integration_test)
add_test(NAME command_executor_test COMMAND command_executor_test)
add_test(NAME uploader_test COMMAND uploader_test)
set_tests_properties(ssh_integration_test sftp_integration_test
    command_executor_test uploader_test PROPERTIES
    SKIP_RETURN_CODE 2
)



