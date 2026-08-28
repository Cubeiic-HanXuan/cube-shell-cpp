#pragma once

// ProxyCommandDialer.h — 「代理命令」这一种代理类型的落地实现
// （OpenSSH 的 ProxyCommand：起一个本地进程，它的 stdin/stdout 就是通往目标的
//  字节流。典型命令行 `nc -X 5 -x proxy:1080 %h %p`、`corkscrew proxy 8080 %h %p`）。
//
// 为什么单独一个文件、且**条件编译**：
//   net/ 这一层其余部分是无条件编译的（协议无关、鸿蒙上也要跑）。起子进程不是
//   ——鸿蒙沙箱禁 exec，而且那上面压根没有 nc/corkscrew 这类载体二进制。所以本
//   文件只在 CUBESHELL_WITH_LOCALPROC 为真时进 CORE_SOURCES（见 core/CMakeLists.txt），
//   与 frp / docker CLI 同一个门。关掉时 ProxyConnector 拿不到拨号器，「代理命令」
//   这一类型会给出一句可读的错误而不是崩，UI 侧也摘掉那一项。
//
// 放在 net/ 而不是 ssh/ 是因为它与 SSH 无关：载体进程搬的是裸字节，以后
// Telnet/TCP 想走代理命令可以直接复用。
//
// 实现要点（细节见 .cpp）：进程的 stdin/stdout 不是 socket，没法交给
// libssh2_session_handshake，于是用 socket_util::makeSocketPair 造一对已连接的
// 本地 socket——一端交给 libssh2（它看到的就是个普通可 select 的 fd，
// SshClient::socketFd() 那三处承重逻辑一行都不用改），另一端由一个跑着事件循环
// 的泵线程与子进程对搬字节。

#include "ProxyConnector.h"

namespace cubeshell {

// 造一个「代理命令」拨号器，直接喂给 SshClient::setCommandDialer 或
// ProxyDialRequest::commandDialer。返回的对象无状态、可重复使用、线程安全
// （每次调用各自起一套进程 + 泵线程 + socket pair）。
CommandDialer makeProxyCommandDialer();

} // namespace cubeshell
