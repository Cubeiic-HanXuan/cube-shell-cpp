# Windows ARM64「静态库 + 动态 CRT」的 overlay triplet（覆盖 vcpkg 社区 triplet
# arm64-windows-static-md）。
#
# 与 arm64-windows-static 的唯一区别是 CRT 链接方式：
#   * arm64-windows-static     → VCPKG_CRT_LINKAGE static  (/MT，静态 CRT)
#   * arm64-windows-static-md  → VCPKG_CRT_LINKAGE dynamic (/MD，动态 CRT，本文件)
#
# 为什么必须用 -md（动态 CRT）版本：
#   Qt 官方预编译包（win64_msvc2022_arm64，CI 用 install-qt-action 安装）是
#   /MD（动态 CRT）构建的。若 vcpkg 用静态 CRT（/MT），则同时链接 Qt(/MD) 与
#   vcpkg 静态库(/MT) 的目标会在链接期碰撞：
#       error LNK2038: mismatch detected for 'RuntimeLibrary':
#       value 'MT_StaticRelease' doesn't match value 'MD_DynamicRelease'
#       error LNK2005: ... already defined in msvcprt.lib
#   整个项目并未统一设置 CMAKE_MSVC_RUNTIME_LIBRARY，故必须让 vcpkg 侧与 Qt 对齐
#   到 /MD。库本身仍为静态（VCPKG_LIBRARY_LINKAGE static），只有 CRT 改动态。
#
# 仍需配合 vcpkg-ports/openssl 的 overlay port，修掉 OpenSSL 在 ARM64 Release
# 下的两处 MSVC 误编译（OpenSSL issue #26239 / #27030）：
#   1) interlocked 内建（InterlockedOr64 等）生成错误代码，SSL_CTX_new() 内
#      破坏调用栈即崩 —— 由本 triplet 的 NO_INTERLOCKEDOR64 规避；
#   2) Release 优化（/O2）在 TLS 握手路径上破坏栈/寄存器，随后在 libssl
#      （如 tls_parse_all_extensions）解引用野指针 0xC0000005 —— 仅关掉
#      优化（/Od）可规避，由 overlay port 只对 OpenSSL 降优化完成。
# 症状都是 FreeRDP 连 RDP 的 TLS/NLA 握手时进程无声消失。
#
# 用法（与 -DVCPKG_TARGET_TRIPLET=arm64-windows-static-md 同时给出）：
#   cmake -B build ... -DVCPKG_OVERLAY_TRIPLETS=vcpkg-triplets -DVCPKG_OVERLAY_PORTS=vcpkg-ports
# 注意：改 triplet/port 会改变 ABI 哈希，已装好的包需重新 vcpkg install（或删掉
# build/vcpkg_installed 重新配置），否则拿到的仍是旧二进制。
#
# 部署提示：动态 CRT 下目标机器需装 VC++ 运行库（vc_redist.arm64.exe），或在
# 打包时随应用附带（见 BUILD-Windows-ARM64.md 第 6 节）。

set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

# OpenSSL ARM64 缺陷绕过（之一：interlocked 内建）
set(VCPKG_C_FLAGS "${VCPKG_C_FLAGS} /DNO_INTERLOCKEDOR64")
set(VCPKG_CXX_FLAGS "${VCPKG_CXX_FLAGS} /DNO_INTERLOCKEDOR64")
