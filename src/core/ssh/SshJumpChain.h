#pragma once

// SshJumpChain.h — 「跳转服务器」（ProxyType::JumpHost）的链式多跳实现。
//
// 它提供 ProxyConnector 需要的那个 JumpDialer：给定目标 host/port 与一串跳板机
// 设备 id，返回一个已连接、可直接 handshake 的 fd。
//
// 为什么放在 ssh/ 而不是 net/：链路的每一跳都是一条真的 SSH 会话，中转靠
// libssh2 的 direct-tcpip 通道。而 net/ 那一层刻意不依赖 ssh/（协议无关、
// 无条件编译，以后 Telnet/TCP 走代理要复用它）。于是做依赖倒置：net/ 只认识
// 一个 std::function，实现在这里，由 SshClient 注入。
//
// 链路建立（以 hopIds = [a, b]、目标 T 为例）：
//   1. 连 a —— 普通拨号，**递归尊重 a 自己的代理配置**（a 可能在 HTTP 代理后面）
//   2. 在 a 上开 direct-tcpip 到 b:22 → 经 socket pair 变成一个普通 fd →
//      b 用 SshClient::connectOverSocket 在这个 fd 上完成握手 + 认证
//   3. 在 b 上开 direct-tcpip 到 T:port → 同样经 socket pair → 把这一端交回
//      ProxyConnector，由目标 SshClient 在上面做自己的握手
//
// 每一跳「通道 ↔ socket pair」都要一个中继线程，所以 n 跳会起 n 个线程。
// 这是 socket pair 方案的必然代价，换来的是 SshClient::socketFd() 那一套承重
// 逻辑（读循环的 select、shutdownSocket() 取消、O_NONBLOCK 翻转）一行都不用改。

#include <QList>
#include <QString>

#include "config/DeviceConfigStore.h"
#include "net/ProxyConnector.h"
#include "SshClient.h"

namespace cubeshell {

// 造一个跳板拨号器。
//
// catalog 是**已经解析好凭据**的设备快照（password / proxy.password 都填好了），
// 由持有 DeviceConfigStore 的一方在自己的线程上准备，见
// GlobalState::setJumpHostCatalog。这里按值捕获一份：建链跑在工作线程上，而
// DeviceConfigStore 没有锁，让工作线程回头去查它会和 UI 线程的设备编辑撞车。
//
// catalog 里只需包含**被引用为跳板**的设备。查不到某个 id 时给出
// "跳板机 xxx 已不存在（可能已被删除）"——这正是用户删掉被引用设备后该看到的话。
//
// prompt 是目标连接自己的 keyboard-interactive 回调，可为空。链构建器会给每跳
// 包一层，把提示文本前缀成「跳板机 bastion-hk：Verification code:」——原样复用的
// 话用户看到的是一句光秃秃的 "Verification code:"，分不清是哪一跳在要动态码。
JumpDialer makeSshJumpDialer(const QList<DeviceEntry> &catalog,
                             const SshPromptCallback &prompt = {});

} // namespace cubeshell
