#pragma once

// LineEndings.h — 终端字节流的换行转换（协议无关）。
//
// 这两个函数最早写在 SerialBridge 里，但它们处理的是"终端产生 \r、对端期望
// 什么"这一类纯字节问题，与底下是串口还是 TCP 无关：Telnet 的 RFC 854 要求
// 行尾是 CR LF，裸 TCP 对端各家不一，与串口面对的是同一道题。故抽到这里由
// SerialBridge / TcpBridge 共用。
//
// 无条件编译：不依赖 Qt6::SerialPort，也不依赖任何协议模块。

#include <QByteArray>

namespace cubeshell {

// 发送方向的换行形态。各协议的 *Settings 里各自有一份同义 enum（避免协议
// 头文件互相 include），在调用处映射到本 enum。
enum class NewlineMode { Cr, Lf, CrLf };

// 终端产生的 \r 按 mode 转换成实际写入链路的字节序列。
//
// 终端的回车键产生的是 \r（CR），Cr 模式即原样透传。已经是 \r\n 的
//（终端一般不产生，但粘贴的文本里会有）不重复补 LF。
QByteArray applyNewlineMode(const QByteArray &input, NewlineMode mode);

// 接收方向：给孤立的 LF 补上 CR，已经是 \r\n 的不动。
//
// prevWasCr 是**跨调用的状态**，必须由调用方持有并原样传回来：\r 和 \n 完全
// 可能被拆到两次读取里（串口低波特率、TCP 分段都会），若每次调用都从零开始
// 判断，边界上的 \n 会被误当成孤立 LF 而多补一个 \r，屏幕上多出一个空行。
// 函数返回前会把它更新为「本块最后一个字节是否为 \r」。
//
// 为什么需要补 CR：VT 规范里 LF 只下移一行、不回行首（回行首是 CR 的职责），
// 对端发裸 \n 时屏幕上会出现阶梯状输出。SSH 里见不到这个现象，因为 pty 的
// 行规程（termios ONLCR）已经在内核里把 \n 展开成 \r\n 了；串口对面是裸设备、
// Telnet 对面可能是不守规矩的嵌入式实现，没人做这个转换。
QByteArray applyRxImplicitCr(const QByteArray &input, bool &prevWasCr);

} // namespace cubeshell
