// LineEndings.cpp — 换行转换。见 LineEndings.h。
//
// 实现原样来自 SerialBridge（本次重构抽出，逻辑未改动，由 tests/serial_test.cpp
// 与 tests/telnet_test.cpp 双向守着）。

#include "net/LineEndings.h"

namespace cubeshell {

QByteArray applyNewlineMode(const QByteArray &input, NewlineMode mode)
{
    // Cr 模式即原样透传，是最常见的默认。
    if (mode == NewlineMode::Cr)
        return input;

    QByteArray out;
    out.reserve(input.size() + 8);   // CrLf 最坏情况每个 \r 多一字节

    for (int i = 0; i < input.size(); ++i) {
        const char c = input.at(i);
        if (c != '\r') {
            out.append(c);
            continue;
        }
        // 已经是 CRLF 的（终端一般不会产生，但粘贴的文本里会有）不再重复加 LF。
        const bool nextIsLf = (i + 1 < input.size() && input.at(i + 1) == '\n');
        switch (mode) {
        case NewlineMode::Lf:
            out.append('\n');
            if (nextIsLf)
                ++i;          // 吃掉原有的 \n，避免变成 \n\n
            break;
        case NewlineMode::CrLf:
            out.append('\r');
            out.append('\n');
            if (nextIsLf)
                ++i;          // 原本就是 \r\n，别写成 \r\n\n
            break;
        case NewlineMode::Cr:
            out.append('\r'); // 上面已提前返回，这里只为编译器穷尽 switch
            break;
        }
    }
    return out;
}

QByteArray applyRxImplicitCr(const QByteArray &input, bool &prevWasCr)
{
    if (input.isEmpty())
        return input;   // 空块不该改动 prevWasCr

    QByteArray out;
    out.reserve(input.size() + 8);

    bool sawCr = prevWasCr;
    for (int i = 0; i < input.size(); ++i) {
        const char c = input.at(i);
        if (c == '\n' && !sawCr)
            out.append('\r');   // 孤立 LF：补 CR，让光标回到行首
        out.append(c);
        sawCr = (c == '\r');
    }

    // 只有最后一个字节是 \r 才需要跨块记住 —— 下一块若以 \n 开头，那是同一个
    // \r\n 被读取边界拆开了，不能再补 \r。
    prevWasCr = sawCr;
    return out;
}

} // namespace cubeshell
