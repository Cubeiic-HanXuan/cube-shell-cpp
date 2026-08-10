cmake_minimum_required(VERSION 3.24)

project(cube-shell
    VERSION 3.2.1
    DESCRIPTION "CubeShell - C++/Qt6 rewrite of the Python terminal/SSH/RDP/AI client"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------

# 版本号编进二进制，作为「本机运行版本」的唯一权威来源。
# 更新检查与关于对话框都读它，而不是 conf/theme.json 的 "version"——
# 该配置文件与 Python 版共用，键可能缺失（老配置）或停在 Python 版的版本号，
# 两种情况都会让更新检查得出错误结论。
add_compile_definitions(CUBESHELL_VERSION="${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Export compile_commands.json for tooling (clangd, IDEs)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Put binaries in a predictable place
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Windows：统一整个项目（含 tests/）使用动态 CRT（/MD）。
# Qt 官方预编译包是 /MD 构建的；vcpkg 侧用 -static-md triplet（静态库 + 动态 CRT）
# 与之对齐。若应用代码不统一为 /MD，则同时链接 Qt(/MD) 与 vcpkg 静态库的目标会在
# 链接期碰撞（LNK2038 RuntimeLibrary 不匹配 / LNK2005 符号重复）。cmake_minimum
# 3.24 > 3.15，CMP0091 已为 NEW，此变量直接生效。仅 vcpkg 静态库场景下需要。
if(WIN32 AND VCPKG_TARGET_TRIPLET MATCHES "static-md")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# ---------------------------------------------------------------------------
# 目标平台：鸿蒙PC（HarmonyOS 5 / OpenHarmony，麒麟 X90 ARM64）
# ---------------------------------------------------------------------------
# 见 docs/鸿蒙PC适配方案.md。
#
# 鸿蒙PC 虽是桌面形态（键鼠/窗口/菜单，Qt Widgets 主场），但应用模型与手机相同：
# HAP 包 + 沙箱，因而三类能力在系统层面就不存在，只能整体裁剪：
#   1. 无本地 shell —— 拿不到 forkpty/posix_openpt，本地终端标签页不可实现；
#      SSH/串口终端不受影响（走 Session::runEmptyPTY() + 手动接管 emulation
#      字节流，即 SerialBridge 已验证的模式），qtermwidget 渲染能力 100% 保留。
#   2. 禁止 exec 子进程 —— QProcess 全线不可用，frp/docker CLI/claude CLI
#      等「起本地进程」的功能失去载体。
#   3. Qt 模块子集 —— 官方 Qt for OpenHarmony 不含 QtSerialPort，
#      QtMultimedia 支持不完整。
#
# 落地约束：本开关**默认 OFF**，且所有裁剪都收敛在下面这一个 if() 内。
# 桌面三平台（macOS/Windows/Linux）不传 -DCUBESHELL_PLATFORM_OHOS=ON 时，
# 此段整体不执行，源码路径与编译产物与适配前逐字节一致。
option(CUBESHELL_PLATFORM_OHOS "Build for HarmonyOS PC (OpenHarmony, aarch64-linux-ohos)" OFF)

# 三个新开关的默认值随平台走：桌面 ON（保持现状），鸿蒙 OFF（能力不存在）。
# 用变量做默认值而非事后 FORCE，是为了让 `cmake -LH` 展示的值就是真实生效值，
# 同时保留使用者显式覆盖的余地（例如二期串口/RDP 补齐后单独打开）。
if(CUBESHELL_PLATFORM_OHOS)
    set(CUBESHELL_LOCAL_CAP_DEFAULT OFF)
else()
    set(CUBESHELL_LOCAL_CAP_DEFAULT ON)
endif()

# 本地终端后端（KPty/KPtyDevice/KPtyProcess/Pty/ConPtyProcess）。
# OFF 时 qtermwidget 只保留远程数据源（SSH/串口）的渲染路径。
option(CUBESHELL_WITH_LOCALPTY
    "Build the local shell terminal backend (forkpty / ConPty)"
    ${CUBESHELL_LOCAL_CAP_DEFAULT})

# 语音输入（core/ai/VoiceInput，QAudioSource 采集 + HTTP 识别）。
# OFF 时不再需要 Qt6::Multimedia，find_package 也随之跳过。
option(CUBESHELL_WITH_VOICE
    "Build voice input (Qt Multimedia / QAudioSource)"
    ${CUBESHELL_LOCAL_CAP_DEFAULT})

# 依赖 QProcess 起本地子进程的工具集：frp 内网穿透、本地 docker CLI、
# claude/hermes 的本地 CLI 后端、本地文件管理器调起（open/explorer/xdg-open）。
option(CUBESHELL_WITH_LOCALPROC
    "Build tools that spawn local subprocesses (frp / docker CLI / local CLI backends)"
    ${CUBESHELL_LOCAL_CAP_DEFAULT})

if(CUBESHELL_PLATFORM_OHOS)
    # 源码侧统一用这一个宏做条件编译（Qt 侧的 Q_OS_OHOS 需 Qt for OHOS 才定义，
    # 这里自定义宏可让裁剪在任意工具链下都成立，也便于 clangd 索引）。
    add_compile_definitions(CUBESHELL_PLATFORM_OHOS=1)

    # 串口与 RDP 的 option() 在下方声明（历史顺序）。option() 不会覆盖已存在的
    # 缓存条目，因此这里先写入缓存即可让后续 option() 保持 OFF，
    # 且 find_package(Qt6 SerialPort) 不会被触发。
    set(CUBESHELL_WITH_SERIAL OFF CACHE BOOL
        "Serial port is unavailable on HarmonyOS (no QtSerialPort module)" FORCE)
    set(CUBESHELL_WITH_RDP OFF CACHE BOOL
        "RDP is unavailable on HarmonyOS (FreeRDP not ported yet)" FORCE)

    message(STATUS "cube-shell: 目标平台为鸿蒙PC（OpenHarmony）——"
                   "已裁剪 本地终端/串口/RDP/语音/本地子进程工具")
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
# Workaround: Qt 6.9's FindWrapOpenGL.cmake unconditionally links the AGL
# framework on macOS (line ~45), but AGL was removed from recent Xcode SDKs,
# causing "framework 'AGL' not found" at link time. Terminal rendering is
# raster/CPU-based and does not need AGL, so pre-define an empty WrapOpenGL
# target; FindWrapOpenGL then returns early (its `if(TARGET ...)` guard) and
# never adds the -framework AGL flag.
if(APPLE AND NOT TARGET WrapOpenGL::WrapOpenGL)
    add_library(WrapOpenGL::WrapOpenGL INTERFACE IMPORTED)
endif()

find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Network
    Concurrent
)
# VoiceInput 需要 QtMultimedia（QAudioSource）。
# HarmonyOS 上 QtMultimedia 支持不完整，默认关闭。
if(CUBESHELL_WITH_VOICE)
    find_package(Qt6 REQUIRED COMPONENTS Multimedia)
endif()

qt_standard_project_setup()

# Optional protocol backends (enabled as their modules are ported)
option(CUBESHELL_WITH_SSH "Build SSH support (libssh2)" ON)
option(CUBESHELL_WITH_RDP "Build RDP support (FreeRDP)" OFF)
# 串口默认 ON：Qt6::SerialPort 是 Qt 官方模块，各平台都随 Qt 提供，不像
# FreeRDP 那样经常缺失。OFF 时 serial/ 目录不编译，UI 侧入口由
# CUBESHELL_WITH_SERIAL 宏统一摘除。
option(CUBESHELL_WITH_SERIAL "Build Serial Port support (QSerialPort)" ON)

# SerialPort 组件单独查找：放在 option 之后才能按开关决定是否 REQUIRED，
# 使 -DCUBESHELL_WITH_SERIAL=OFF 的环境即便没装 qtserialport 也能配置成功。
if(CUBESHELL_WITH_SERIAL)
    find_package(Qt6 REQUIRED COMPONENTS SerialPort)
endif()

# cJSON —— 必须在任何 add_subdirectory() / FreeRDP 查找之前导入。
# WinPR 的 imported target 把 `cjson` 写进了 INTERFACE_LINK_LIBRARIES。若此时
# cjson 不是已知的 imported target，CMake 就把它当普通库名原样传给链接器
#（MSVC 下变成不带路径的 `cjson.lib` → LNK1181）；一旦目标存在，CMake 会替换
# 成 vcpkg 里的绝对路径。放在根作用域是为了让 src/core（链接 winpr）与 src/app
#（最终链接 cube-shell.exe）两个目录都能看见这个目标。
if(CUBESHELL_WITH_RDP)
    find_package(cJSON CONFIG QUIET)
endif()

if(CUBESHELL_WITH_SSH)
    # Resolve libssh2. Order of preference:
    #   1. a caller-provided static lib (LIBSSH2_LIBRARY / LIBSSH2_INCLUDE_DIR)
    #   2. a system / brew install (find_library + find_path)
    #   3. FetchContent: build libssh2 from source (self-contained fallback)
    #      — NOT available on OHOS: FetchContent uses the host toolchain, not the
    #        cross-compiler.  OHOS builds MUST pre-cross-compile libssh2 (and its
    #        transitive deps) with the OHOS NDK, then pass them via
    #        LIBSSH2_LIBRARY / LIBSSH2_INCLUDE_DIR.
    find_path(LIBSSH2_INCLUDE_DIR NAMES libssh2.h
        HINTS ${LIBSSH2_INCLUDE_DIR} /tmp/libssh2-install/include /opt/homebrew/include /usr/local/include)
    find_library(LIBSSH2_LIBRARY NAMES libssh2 ssh2
        HINTS ${LIBSSH2_LIBRARY} /tmp/libssh2-install/lib /opt/homebrew/lib /usr/local/lib)

    if(LIBSSH2_INCLUDE_DIR AND LIBSSH2_LIBRARY)
        set(LIBSSH2_FOUND TRUE)
        set(LIBSSH2_INCLUDE_DIRS ${LIBSSH2_INCLUDE_DIR})
        set(LIBSSH2_LIBRARIES ${LIBSSH2_LIBRARY})
        message(STATUS "libssh2 found: ${LIBSSH2_LIBRARY}")
    elseif(NOT CUBESHELL_PLATFORM_OHOS)
        message(STATUS "libssh2 not found locally; fetching and building from source")
        include(FetchContent)
        FetchContent_Declare(libssh2
            URL https://www.libssh2.org/download/libssh2-1.11.1.tar.gz
        )
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
        find_package(OpenSSL REQUIRED)
        FetchContent_MakeAvailable(libssh2)
        set(LIBSSH2_FOUND TRUE)
        set(LIBSSH2_INCLUDE_DIRS ${libssh2_SOURCE_DIR}/include)
        set(LIBSSH2_LIBRARIES libssh2_static OpenSSL::SSL OpenSSL::Crypto)
    else()
        message(FATAL_ERROR
            "cube-shell OHOS: libssh2 not found, and FetchContent is unavailable on "
            "OHOS (it would use the host toolchain, not the OHOS NDK).\n"
            "  Cross-compile openssl/zlib/libssh2 with the OHOS NDK first "
            "(see scripts/build-ohos-deps.sh), then re-run cmake with:\n"
            "  -DLIBSSH2_INCLUDE_DIR=<...>/aarch64-linux-ohos/include\n"
            "  -DLIBSSH2_LIBRARY=<...>/aarch64-linux-ohos/lib/libssh2.a")
    endif()

    if(NOT LIBSSH2_FOUND)
        message(WARNING "libssh2 unavailable; SSH support disabled. Set CUBESHELL_WITH_SSH=OFF")
        set(CUBESHELL_WITH_SSH OFF)
    endif()
endif()

# yaml-cpp — Compose YAML 解析（core/docker 移植需要解析任意 YAML）。
# 对应Python: PyYAML (yaml.safe_load / yaml.dump)。
# Order of preference:
#   1. a system / brew install (find_package config mode)
#   2. FetchContent: build yaml-cpp 0.8.0 from source (self-contained fallback)
find_package(yaml-cpp QUIET)
if(yaml-cpp_FOUND)
    message(STATUS "yaml-cpp found: ${yaml-cpp_DIR}")
else()
    message(STATUS "yaml-cpp not found locally; fetching and building from source")
    include(FetchContent)
    # 多 URL 兜底：github 直连在国内环境常超时，追加代理镜像；URL_HASH 固定为
    # 官方 0.8.0 发布包哈希，镜像内容被校验，来源可信性不受影响。
    FetchContent_Declare(yaml-cpp
        URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz
            https://gh-proxy.com/https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz
        URL_HASH SHA256=fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16
        TIMEOUT 120     # 单次下载上限，github 不通时尽快切换到镜像 URL
    )
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
    set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(yaml-cpp)
endif()
# 目标名统一：0.8.0 源码构建与新版 config 都导出 yaml-cpp::yaml-cpp，但老版
# find_package 只导出裸目标 yaml-cpp——补一个别名让子目录统一链接 namespaced 名。
if(NOT TARGET yaml-cpp::yaml-cpp AND TARGET yaml-cpp)
    add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
endif()

# ---------------------------------------------------------------------------
# Submodules
# ---------------------------------------------------------------------------
enable_testing()                    # required for add_test()/ctest discovery

# macOS bundle metadata must be set *before* add_subdirectory(src/app): the
# MACOSX_BUNDLE_* variables are expanded in the scope that owns the target, so
# values assigned after the subdirectory call would not reach packaging/
# Info.plist.in and CMake would silently fall back to its own defaults.
if(APPLE)
    set(MACOSX_BUNDLE_EXECUTABLE_NAME cube-shell)
    set(MACOSX_BUNDLE_BUNDLE_NAME CubeShell)
    set(MACOSX_BUNDLE_GUI_IDENTIFIER com.cubeshell.app)
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
    set(MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
    set(MACOSX_BUNDLE_ICON_FILE logo.icns)
    set(MACOSX_BUNDLE_COPYRIGHT "Copyright (c) CubeShell")
    # LSMinimumSystemVersion only labels the bundle; it deliberately does *not*
    # force CMAKE_OSX_DEPLOYMENT_TARGET, because pinning the deployment target
    # below the SDK of the prebuilt dependencies (libssh2, openssl@3) makes the
    # linker warn on every target. Pass -DCMAKE_OSX_DEPLOYMENT_TARGET=... when a
    # real backwards-compatible build is wanted.
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CUBESHELL_MACOS_MIN_VERSION ${CMAKE_OSX_DEPLOYMENT_TARGET})
    else()
        set(CUBESHELL_MACOS_MIN_VERSION "11.0")
    endif()
endif()

# 兜底：即使上面的 find_package(cJSON) 没能创建 imported target（vcpkg 布局差异
# 等原因），也让链接器能在 vcpkg 的 lib 目录里找到裸名 cjson.lib。
# 必须写在 add_subdirectory() 之前——link_directories 只影响之后创建的目标。
# 用 CMAKE_BINARY_DIR 而非 VCPKG_INSTALLED_DIR：manifest 模式下后者可能为空。
# 非 Windows 平台不需要：WinPR 走 pkg-config 或动态链接自行解析依赖。
if(WIN32 AND VCPKG_TARGET_TRIPLET)
    link_directories(
        "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/lib"
        "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/debug/lib"
    )
endif()

add_subdirectory(src/qtermwidget)   # terminal emulator widget
add_subdirectory(src/core)          # business logic (ssh bridge, sessions, ai, ...)
add_subdirectory(src/ui)            # main window + dialogs
add_subdirectory(src/app)           # application entry point
add_subdirectory(tests)             # headless smoke tests

# ---------------------------------------------------------------------------
# Install / packaging
# ---------------------------------------------------------------------------
# 对应Python: 打包侧的 build-exe.bat / build-linux.sh / pysidedeploy.spec +
# conf/macos_url_scheme.plist。这里只声明 CMake 侧的安装规则与 bundle 元数据，
# 真正的安装器制作（Inno Setup / AppImage / deb）仍由下面注释指向的现有脚本完成。

if(APPLE)
    # macOS: .app bundle。Info.plist 由 packaging/Info.plist.in 在配置期展开，
    # 其中的 CFBundleURLTypes 声明 jms/ssh/cubeshell —— 这就是 macOS 的
    # URL Scheme 注册途径（系统扫描 .app 后自动登记到 Launch Services）。
    # 对应Python: core/url_dispatch/url_scheme_register.py 的 Darwin 分支
    #（"由 Info.plist 处理，此模块不做额外操作"）。
    # bundle 元数据变量在上面 add_subdirectory 之前已设置。
    set_target_properties(cube-shell PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/packaging/Info.plist.in
        OUTPUT_NAME cube-shell
    )

    # 图标：使用 cpp 项目自带的 resources/icons/logo.icns（不再依赖 Python 版目录）。
    set(CUBESHELL_ICNS ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/logo.icns)
    if(EXISTS ${CUBESHELL_ICNS})
        target_sources(cube-shell PRIVATE ${CUBESHELL_ICNS})
        # 目标定义在 src/app/，源文件属性默认只对本目录可见，
        # 必须用 TARGET_DIRECTORY 才能真正拷进 bundle 的 Resources/。
        set_source_files_properties(${CUBESHELL_ICNS}
            TARGET_DIRECTORY cube-shell
            PROPERTIES MACOSX_PACKAGE_LOCATION Resources
        )
    endif()

    # bundle 整体装到 <prefix>/（即 <prefix>/cube-shell.app）。
    install(TARGETS cube-shell BUNDLE DESTINATION .)
elseif(CUBESHELL_PLATFORM_OHOS)
    # 鸿蒙：qt-cmake 把 qt_add_executable 建成 MODULE library（由 HAP 壳加载，
    # 不是独立可执行文件）。补 LIBRARY DESTINATION；HAP 打包由 DevEco/
    # harmonydeployqt 负责（见 docs/鸿蒙PC适配方案.md 阶段 4），install 仅作占位。
    install(TARGETS cube-shell
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
    )
else()
    # Linux / Windows: 裸可执行文件装到 bin/。
    install(TARGETS cube-shell RUNTIME DESTINATION bin)
endif()

# ---------------------------------------------------------------------------
# 运行期资源部署（→ bundle Resources / 可执行文件旁 resources/）
# ---------------------------------------------------------------------------
# C++ 版自带资源分两处（不再依赖 Python 版 i18n/、icons/ 目录）：
#   * cpp/resources/           —— icons / translations（非 qtermwidget 资源）
#   * cpp/src/qtermwidget/     —— color-schemes、kb-layouts、根级 default.keytab
#     （与 Python 版 qtermwidget/ 包内布局一致）
# 运行期探测顺序：
#   1. .app bundle 的 Contents/Resources/（macOS）
#   2. 可执行文件旁的 resources/（Linux / Windows）
#   3. 编译期宏指向的源码树目录（开发期兜底）
# 见 src/qtermwidget/tools.cpp 与 src/ui/LanguageManager.cpp。
# POST_BUILD 拷贝在 src/app/CMakeLists.txt（add_custom_command(TARGET) 必须与
# 目标同目录）；这里只声明 install 规则。
set(CUBESHELL_RESOURCES_SRC ${CMAKE_CURRENT_SOURCE_DIR}/resources)
set(CUBESHELL_QTERMWIDGET_RES_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/qtermwidget)

# 运行期共享配置（主题/命令库/LLM 供应商表/常用容器清单）随项目自带一份 conf/。
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/conf/
    DESTINATION share/cube-shell/conf
    FILES_MATCHING PATTERN "*.json" PATTERN "*.yml"
)
# install 侧同样部署 C++ 自带资源：
#   * macOS: bundle 在 POST_BUILD 已含 Resources/，install(TARGETS BUNDLE) 整体拷走；
#   * Linux/Windows: 装到 bin/resources/，与运行期「可执行文件旁 resources/」探测一致。
if(NOT APPLE)
    install(DIRECTORY ${CUBESHELL_RESOURCES_SRC}/
        DESTINATION bin/resources
    )
    install(DIRECTORY ${CUBESHELL_QTERMWIDGET_RES_SRC}/color-schemes
                      ${CUBESHELL_QTERMWIDGET_RES_SRC}/kb-layouts
        DESTINATION bin/resources
    )
    install(FILES ${CUBESHELL_QTERMWIDGET_RES_SRC}/default.keytab
        DESTINATION bin/resources
    )
endif()

# --- Windows installer 模板说明（不在 CMake 内执行）-------------------------
# Windows 安装器沿用工程根的 Inno Setup 脚本 installer.iss：
#   1. cmake --install build --prefix dist/  → dist/bin/cube-shell.exe
#   2. windeployqt dist/bin/cube-shell.exe   → 拷入 Qt 运行时/插件
#   3. iscc installer.iss                    → 生成安装器
# URL Scheme / 右键菜单在**首次运行时**由 UrlSchemeRegistrar::ensureRegistered()
# 与 WindowsIntegration::install() 写 HKCU 注册表，无需安装器代劳（HKCU 也不需
# 管理员权限）。对应Python: core/url_dispatch/url_scheme_register.py::_register_windows

# --- Linux AppImage / deb 元数据模板说明（不在 CMake 内执行）----------------
# Linux 打包沿用工程根的 build-linux.sh。若要做 AppImage/deb，所需元数据为：
#   * cube-shell.desktop  —— Exec=cube-shell %u,
#                            MimeType=x-scheme-handler/jms;x-scheme-handler/cubeshell;
#     该文件的运行期等价物已由 UrlSchemeRegistrar 写到
#     ~/.local/share/applications/cube-shell-url-handler.desktop
#    （对应Python: _register_linux），系统级安装时改装到
#     <prefix>/share/applications/ 并调用 update-desktop-database。
#   * icons/logo.png → <prefix>/share/icons/hicolor/256x256/apps/cube-shell.png
#   * deb control: Depends: libqt6widgets6, libssh2-1, libssl3
# 这些模板文件按需新增到 cpp/packaging/；当前 Phase 6 只落地 macOS 的
# Info.plist.in（URL Scheme 注册的唯一强依赖项）。

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "cube-shell configuration summary")
message(STATUS "  Qt version      : ${Qt6_VERSION}")
message(STATUS "  Build type      : ${CMAKE_BUILD_TYPE}")
if(CUBESHELL_PLATFORM_OHOS)
    message(STATUS "  Target platform : HarmonyOS PC (OpenHarmony)")
else()
    message(STATUS "  Target platform : Desktop (macOS / Windows / Linux)")
endif()
message(STATUS "  SSH (libssh2)   : ${CUBESHELL_WITH_SSH}")
message(STATUS "  RDP (FreeRDP)   : ${CUBESHELL_WITH_RDP}")
message(STATUS "  Serial (QtSerialPort): ${CUBESHELL_WITH_SERIAL}")
message(STATUS "  Local PTY       : ${CUBESHELL_WITH_LOCALPTY}")
message(STATUS "  Voice input     : ${CUBESHELL_WITH_VOICE}")
message(STATUS "  Local subproc   : ${CUBESHELL_WITH_LOCALPROC}")
if(CUBESHELL_WITH_RDP)
    # 诊断用：cjson 目标存在时 CMake 会把 WinPR 传递过来的裸名 cjson 解析成绝对
    # 路径；不存在时就只能靠上面的 link_directories 兜底。
    if(TARGET cjson)
        message(STATUS "  cJSON target    : cjson (裸名依赖将解析为绝对路径)")
    else()
        message(STATUS "  cJSON target    : 未找到，依赖 link_directories 兜底")
    endif()
endif()
message(STATUS "  Install prefix  : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
