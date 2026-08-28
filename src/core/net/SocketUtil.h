#pragma once

// SocketUtil.h — 原始 BSD socket 的跨平台小工具集（协议无关）。
//
// 这些函数原本是 src/core/ssh/PortForwarder.cpp 匿名 namespace 里的私有件。
// SSH 代理（ProxyConnector）要用同一套东西在真 socket 上就地完成
// HTTP CONNECT / SOCKS5 握手，于是提取到 net/ 层共用。net/ 刻意不依赖 ssh/
// ——这一层定位为协议无关、无条件编译（见 src/core/CMakeLists.txt 的 net/ 段），
// 以后 Telnet/TCP 走代理也能直接复用。
//
// 为什么用裸 fd 而不是 QTcpSocket：交给 libssh2 handshake 的必须是一个真的、
// 可 select 的 fd。SshClient::socketFd() 在应用里是承重的——SshBridge 读循环的
// select、shutdownSocket() 这个全应用取消机制、以及 openShell() 之后把 fd 本身
// 翻成 O_NONBLOCK，三处都直接摸它。QTcpSocket 虽然能给出 socketDescriptor()，
// 但它同时把 fd 挂在自己的读通知器上，会和 libssh2 抢同一份可读事件。
//
// 约定：所有 fd 一律用 qintptr 装、按 < 0 判无效（沿用 PortForwarder/SshClient
// 的既有写法）。Win32 的 SOCKET 是无符号的，本项目不区分——INVALID_SOCKET 转成
// qintptr 是 -1，判定仍然成立。

#include <QString>
#include <QtGlobal>

#include <atomic>

namespace cubeshell {
namespace socket_util {

// 无效 fd 哨兵。
inline constexpr qintptr kInvalidSocket = -1;

// 默认 TCP 建连超时（毫秒）。与设置页「SSH 连接超时」的默认值 15 秒对齐
// （见 SettingsDialog::loadCurrentSettings 里的 toInt(15)）。
inline constexpr int kDefaultConnectTimeoutMs = 15000;

// select 的单次等待切片。每片结束都回头看一眼取消标志，所以「连接中途关标签页」
// 最多多等一个切片，而不是等满整条超时。
inline constexpr int kSelectSliceMs = 200;

// socket pair 两端的收发缓冲目标值。默认值在 macOS 上偏小（回环 TCP 常见
// 几十 KB），SSH-over-SSH 跑 SFTP 批量传输时两个方向同时打满，缓冲太小会让
// 泵线程频繁在 send 上打转。
inline constexpr int kPairSocketBufferBytes = 256 * 1024;

// --- 关闭 ---

// 关闭 fd 并置为 kInvalidSocket（引用版，给成员变量用）。
void closeSocket(qintptr &sock);

// 关闭 fd（传值版，给 const 捕获的 lambda 用）。
void closeFd(qintptr sock);

// shutdown(SHUT_RDWR)，不 close：让阻塞在 select()/recv() 上的线程立刻拿到
// EOF/错误返回，从而能回头检查自己的取消标志。fd 的回收仍归原持有者。
// 与 SshClient::shutdownSocket() 同一手法（那里的注释讲清了为什么必须只
// shutdown 不 close），提取出来是为了代理链能对每一跳的 fd 做同样的事。
void shutdownFd(qintptr sock);

// --- socket 选项 ---

// 切换 fd 的阻塞/非阻塞。libssh2 在 session_handshake 之后会把 socket 恢复成
// 原来的（阻塞）状态，所以想让读循环看到 EAGAIN，除了 session 层还必须翻 fd 本身
// ——这正是 SshClient::openShell() 里那段 fcntl/ioctlsocket 的由来。
bool setNonBlocking(qintptr sock, bool on);

// macOS：SO_NOSIGPIPE。往对端已关的 socket 上 send 会抛 SIGPIPE，默认处置是
// 杀进程。代理握手和泵线程都会 send，所以每个我们自己建出来的 fd 都要设。
void setNoSigPipe(qintptr sock);

// TCP_NODELAY。终端是交互式的，攒包等 40ms 会被直接感知成卡顿。
void setNoDelay(qintptr sock);

// 尽力把 SO_SNDBUF/SO_RCVBUF 抬到 bytes（内核可能只给一部分，不算失败）。
void setSocketBuffers(qintptr sock, int bytes);

// --- 建连 ---

// 超时参数的统一约定（connectTcp / recvExact / sendAll 三者一致）：
//   > 0  预算毫秒数
//   == 0 预算已耗尽，立即以超时失败
//   < 0  用 kDefaultConnectTimeoutMs
//
// 0 必须是「已耗尽」而不是「用默认值」：代理握手是一段一段切同一份总预算的
// （连代理 → 发 CONNECT → 读应答），传下去的都是 remainingTime()。若 0 被当成
// 默认值，预算刚好耗尽的那一刻反而又续了整整 15 秒，总耗时可以是配置值的好几倍。

// 连接 host:port，总耗时不超过 timeoutMs。返回 fd，失败返回 kInvalidSocket
// 并填 errMsg（若非空）。
//
// 与它取代的那个裸 `::connect` 的关键区别：
//   * 有超时。原来是纯阻塞调用，只能靠内核 SYN 重传兜底（macOS 约 75 秒）。
//     代理链把这个问题按跳数放大，所以超时是必要基础设施而不是优化。
//   * 可取消。cancelled 非空时每个 select 切片都查一次，置位即放弃。建代理链
//     途中用户关掉标签页要能立刻打断——那会儿目标 SshClient 还没有自己的
//     socket，shutdownSocket() 无从下手。
//
// timeoutMs 是**整个调用**的预算，不是每个候选地址一份：主机同时有 AAAA 和 A
// 记录、而 IPv6 出口被黑洞时，逐地址计时会让总耗时翻倍。调用方（尤其是逐跳
// 建链的那条路）关心的是墙上时钟上界。
qintptr connectTcp(const QString &host, quint16 port,
                   int timeoutMs = kDefaultConnectTimeoutMs,
                   QString *errMsg = nullptr,
                   const std::atomic<bool> *cancelled = nullptr);

// 创建 + bind + listen 一个 TCP 服务端 socket。返回 fd，失败返回 -1 并填 errMsg
// （"端口已被占用" / "无权限绑定" 分开报，沿用 forwarder.py 的措辞）。
// port 传 0 表示由内核分配，之后可用 localPort() 取回实际端口。
qintptr makeListenSocket(const QString &host, quint16 port, QString &errMsg);

// 取 sock 自己那一端的端口（getsockname）。失败返回 0。
quint16 localPort(qintptr sock);

// 建一对**已经连接好**的本地 socket：clientEnd ↔ serverEnd，字节双向可通。
//
// 用途：代理命令的载体是一对管道、跳转服务器的载体是一条 libssh2 channel，
// 两者都不是 socket，没法直接喂给 libssh2_session_handshake。于是把 socket pair
// 的一端交给 libssh2（它看到的就是个普通 socket，上面说的三处承重逻辑一行都
// 不用改），另一端由泵线程与真实载体对搬字节。
//
// 实现走 loopback TCP（listen(127.0.0.1:0) → connect → accept）而不是 POSIX
// socketpair(AF_UNIX)，为的是四个平台只有一条代码路径，也不必担心 libssh2 对
// socket family 做过 TCP 特定假设（比如无条件设 TCP_NODELAY）。
//
// 代价是 listen 那一瞬间本机任何进程都能来连，所以 accept 出来的连接要核对
// 对端地址是回环、且对端端口正好等于 clientEnd 自己的本地端口——不匹配就关掉
// 继续 accept。这把劫持窗口关死。
bool makeSocketPair(qintptr &clientEnd, qintptr &serverEnd, QString *errMsg = nullptr);

// --- 收发 ---

// 单次 recv，带本次调用的超时。返回实际读到的字节数（>0）、0 表示对端正常
// 关闭、-1 表示出错或超时。
qint64 recvSome(qintptr sock, char *buf, int maxBytes, int timeoutMs);

// 单次 recv，不等待（要求 sock 已经设成非阻塞）。返回值分四态：
//   > 0  读到的字节数
//   == 0 对端正常关闭（EOF）
//   -1   这一刻没有数据（EAGAIN/EWOULDBLOCK/EINTR），稍后再来
//   -2   真的出错，填 errMsg
//
// 为什么不用 recvSome：它把「超时」和「出错」都压成 -1。中继循环必须能分辨
// 「对端关了」和「select 伪唤醒但没数据」——分不清的话一次伪唤醒就会被当成
// 连接断开，表现是终端莫名其妙掉线。
qint64 recvNonBlocking(qintptr sock, char *buf, int maxBytes, QString *errMsg = nullptr);

// 读满 size 字节才算成功（少一个字节都算失败）。timeoutMs 是整个调用的预算，
// 约定见上面 connectTcp 之前那段（0 = 已耗尽，不是默认值）。
// 供 SOCKS5 这类定长报文用：它的每个字段长度都由前一个字段决定，读少了就是
// 把下一段报文当成本段解析。
bool recvExact(qintptr sock, char *buf, qint64 size, int timeoutMs,
               QString *errMsg = nullptr,
               const std::atomic<bool> *cancelled = nullptr);

// 把 size 字节全部写出去才算成功。仅供握手用的小报文（几十到几百字节）：
// 实现是 select 可写 + 阻塞 send，单片报文不会真的卡在 send 里。
// timeoutMs 约定同上。
bool sendAll(qintptr sock, const char *data, qint64 size, int timeoutMs,
             QString *errMsg = nullptr,
             const std::atomic<bool> *cancelled = nullptr);

// 单次 send，不等待（要求 sock 已经设成非阻塞）。返回值三态：
//   >= 0 实际写出的字节数（**可能小于 size**，剩下的要调用方自己留着下次再写）
//   -1   这一刻写不进去（EAGAIN/EWOULDBLOCK/EINTR），稍后再来
//   -2   真的出错，填 errMsg
//
// 为什么不用 sendAll：它会一直循环到全部写完（或超时）才返回。双向中继在**同一个
// 线程**里搬两个方向，一旦某个方向的对端不读，sendAll 会把这个线程按在原地，
// 另一个方向就活活饿死——表现是 SFTP 大文件传到一半整条连接卡住。
qint64 sendNonBlocking(qintptr sock, const char *data, qint64 size,
                       QString *errMsg = nullptr);

} // namespace socket_util
} // namespace cubeshell
