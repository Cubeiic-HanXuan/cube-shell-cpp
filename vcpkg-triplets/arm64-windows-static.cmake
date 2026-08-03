# Windows ARM64 静态构建的 overlay triplet（覆盖 vcpkg 自带的 arm64-windows-static）。
#
# 目的：配合 vcpkg-ports/openssl 的 overlay port，修掉 OpenSSL 在 ARM64 Release
# 下的两处 MSVC 误编译（OpenSSL issue #26239 / #27030）：
#   1) interlocked 内建（InterlockedOr64 等）生成错误代码，SSL_CTX_new() 内
#      破坏调用栈即崩 —— 由本 triplet 的 NO_INTERLOCKEDOR64 规避；
#   2) Release 优化（/O2）在 TLS 握手路径上破坏栈/寄存器，随后在 libssl
#      （如 tls_parse_all_extensions）解引用野指针 0xC0000005 —— 仅关掉
#      优化（/Od）可规避，由 overlay port 只对 OpenSSL 降优化完成。
# 症状都是 FreeRDP 连 RDP 的 TLS/NLA 握手时进程无声消失。
#
# 用法（配置时任选一种，需与 -DVCPKG_TARGET_TRIPLET=arm64-windows-static 同时给出）：
#   cmake -B build ... -DVCPKG_OVERLAY_TRIPLETS=vcpkg-triplets -DVCPKG_OVERLAY_PORTS=vcpkg-ports
# 注意：改 triplet/port 会改变 ABI 哈希，已装好的包需重新 vcpkg install（或删掉
# build/vcpkg_installed 重新配置），否则拿到的仍是旧二进制。

set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

# OpenSSL ARM64 缺陷绕过（之一：interlocked 内建）
set(VCPKG_C_FLAGS "${VCPKG_C_FLAGS} /DNO_INTERLOCKEDOR64")
set(VCPKG_CXX_FLAGS "${VCPKG_CXX_FLAGS} /DNO_INTERLOCKEDOR64")
