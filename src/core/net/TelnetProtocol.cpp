// TelnetProtocol.cpp — Telnet IAC 状态机。见 TelnetProtocol.h。

#include "net/TelnetProtocol.h"

#include <QDebug>

namespace cubeshell {

namespace {

// 子协商载荷上限。正常的 TTYPE/NAWS 载荷只有几个到几十字节；对端实现有 bug
// 或链路上有脏数据时（比如一个孤立的 IAC SB 后面永远等不到 IAC SE），不设限
// 会让缓冲无限长大。超限即丢弃该段并回到 Data 态，宁可漏一次协商也不吃内存。
constexpr int kMaxSubNegotiationBytes = 4096;

inline unsigned char byteAt(const QByteArray &a, int i)
{
    return static_cast<unsigned char>(a.at(i));
}

} // namespace

TelnetProtocol::TelnetProtocol(QObject *parent)
    : QObject(parent)
{
}

void TelnetProtocol::reset()
{
    m_phase = Phase::Data;
    m_pendingCommand = 0;
    m_subOption = 0;
    m_subBuffer.clear();
    m_agreedWill.clear();
    m_agreedDo.clear();
    m_refusedWill.clear();
    m_refusedDo.clear();
    m_nawsAgreed = false;
    // 窗口尺寸不清：终端尺寸与连接无关，重连后仍是同一个 QTermWidget。
    if (m_serverEcho) {
        m_serverEcho = false;
        emit serverEchoChanged(false);
    }
}

QByteArray TelnetProtocol::processIncoming(const QByteArray &in)
{
    QByteArray out;
    out.reserve(in.size());

    for (int i = 0; i < in.size(); ++i) {
        const unsigned char c = byteAt(in, i);

        switch (m_phase) {
        case Phase::Data:
            if (c == IAC)
                m_phase = Phase::Iac;
            else
                out.append(char(c));
            break;

        case Phase::Iac:
            switch (c) {
            case IAC:
                // IAC IAC = 字面量 0xFF，还原成一个字节交给终端。
                out.append(char(IAC));
                m_phase = Phase::Data;
                break;
            case WILL: case WONT: case DO: case DONT:
                m_pendingCommand = c;
                m_phase = Phase::Negotiate;
                break;
            case SB:
                m_subBuffer.clear();
                m_subOption = 0;
                m_phase = Phase::SubOptionCode;
                break;
            default:
                // NOP / GA / DM / AYT / IP / BRK / AO / EC / EL 等双字节命令。
                // 都对终端显示没有影响，直接丢弃。
                // （AYT 理论上该回一句 "yes"，但那会把字符打进用户的输入行，
                //   实践中所有终端仿真器都选择忽略。）
                m_phase = Phase::Data;
                break;
            }
            break;

        case Phase::Negotiate:
            switch (m_pendingCommand) {
            case WILL: handleWill(c); break;
            case WONT: handleWont(c); break;
            case DO:   handleDo(c);   break;
            case DONT: handleDont(c); break;
            default: break;
            }
            m_pendingCommand = 0;
            m_phase = Phase::Data;
            break;

        case Phase::SubOptionCode:
            // IAC SB 之后的第一个字节是选项码。
            // 边界：`IAC SB IAC SE` 这种空子协商是畸形的，把 IAC 当选项码
            // 收下，随后的 SE 会在 SubIac 里以 option=255 结束——handleSubNegotiation
            // 认不出 255，直接忽略，符合预期。
            m_subOption = c;
            m_phase = Phase::SubData;
            break;

        case Phase::SubData:
            if (c == IAC) {
                m_phase = Phase::SubIac;
                break;
            }
            if (m_subBuffer.size() >= kMaxSubNegotiationBytes) {
                qWarning() << "TelnetProtocol: subnegotiation option"
                           << int(m_subOption) << "exceeded"
                           << kMaxSubNegotiationBytes << "bytes, dropping";
                m_subBuffer.clear();
                m_phase = Phase::Data;
                break;
            }
            m_subBuffer.append(char(c));
            break;

        case Phase::SubIac:
            if (c == SE) {
                handleSubNegotiation();
                m_subBuffer.clear();
                m_phase = Phase::Data;
            } else if (c == IAC) {
                // 子协商载荷里的字面量 0xFF。
                m_subBuffer.append(char(IAC));
                m_phase = Phase::SubData;
            } else {
                // IAC 后跟了非 SE 非 IAC 的字节：对端实现有问题。按 RFC 855
                // 的容错建议放弃这段子协商，把该字节当新命令重新解释——不这样
                // 做的话一个坏包会让后面所有数据都卡在子协商态里出不来。
                qWarning() << "TelnetProtocol: malformed subnegotiation for option"
                           << int(m_subOption) << "— unexpected byte" << int(c);
                m_subBuffer.clear();
                m_phase = Phase::Iac;
                --i;   // 重新处理该字节
            }
            break;
        }
    }

    return out;
}

QByteArray TelnetProtocol::escapeOutgoing(const QByteArray &in)
{
    // 绝大多数输入里没有 0xFF，先扫一遍避免无谓的拷贝。
    if (!in.contains(char(IAC)))
        return in;

    QByteArray out;
    out.reserve(in.size() + 8);
    for (int i = 0; i < in.size(); ++i) {
        const char c = in.at(i);
        out.append(c);
        if (static_cast<unsigned char>(c) == IAC)
            out.append(char(IAC));   // 加倍
    }
    return out;
}

// --- 协商应答 ---
//
// 命名视角统一为"我方"：WILL/WONT 描述我方要不要启用某选项，
// DO/DONT 描述我方要不要对端启用某选项。

void TelnetProtocol::handleDo(unsigned char option)
{
    // 对端要我启用 option。
    switch (option) {
    case OPT_TTYPE:
        // 同意上报终端类型；具体类型等对端的 SB TTYPE SEND 再给。
        respond(WILL, option);
        break;
    case OPT_NAWS:
        respond(WILL, option);
        if (!m_nawsAgreed) {
            m_nawsAgreed = true;
            // 协商成功即刻补发一次当前尺寸：终端在建连之前就已经有尺寸了，
            // 不补的话对端会一直用它的默认 80x24，直到用户手动拉一次窗口。
            emitWindowSize();
        }
        break;
    case OPT_SGA:
    case OPT_BINARY:
        // SGA 是字符模式（逐字符发送）的前提；BINARY 让 UTF-8 字节能原样通过。
        respond(WILL, option);
        break;
    default:
        // 认识的之外一律拒绝，不留半开状态。
        respond(WONT, option);
        break;
    }
}

void TelnetProtocol::handleDont(unsigned char option)
{
    // 对端要我停用 option。无条件服从（RFC 855：DONT 必须以 WONT 应答）。
    respond(WONT, option);
    if (option == OPT_NAWS)
        m_nawsAgreed = false;
}

void TelnetProtocol::handleWill(unsigned char option)
{
    // 对端声明它要启用 option。
    switch (option) {
    case OPT_ECHO:
        respond(DO, option);
        if (!m_serverEcho) {
            m_serverEcho = true;
            emit serverEchoChanged(true);
        }
        break;
    case OPT_SGA:
    case OPT_BINARY:
        respond(DO, option);
        break;
    default:
        respond(DONT, option);
        break;
    }
}

void TelnetProtocol::handleWont(unsigned char option)
{
    // 对端声明它不启用/要停用 option。无条件服从。
    respond(DONT, option);
    if (option == OPT_ECHO && m_serverEcho) {
        m_serverEcho = false;
        emit serverEchoChanged(false);
    }
}

void TelnetProtocol::respond(unsigned char command, unsigned char option)
{
    if (!m_negotiate)
        return;

    // 抑制重复应答（RFC 855）：状态没变化时不得回复，否则两端会来回
    // DO/WILL 到天荒地老。四张表分别记我方已表态的四种组合。
    QSet<int> *sameSide = nullptr;
    QSet<int> *otherSide = nullptr;
    switch (command) {
    case WILL: sameSide = &m_agreedWill;  otherSide = &m_refusedWill; break;
    case WONT: sameSide = &m_refusedWill; otherSide = &m_agreedWill;  break;
    case DO:   sameSide = &m_agreedDo;    otherSide = &m_refusedDo;   break;
    case DONT: sameSide = &m_refusedDo;   otherSide = &m_agreedDo;    break;
    default: return;
    }
    if (sameSide->contains(option))
        return;                     // 已经这么表过态，闭嘴
    sameSide->insert(option);
    otherSide->remove(option);      // 反向表态作废，允许后续改主意

    const char bytes[3] = { char(IAC), char(command), char(option) };
    emit sendResponse(QByteArray(bytes, 3));
}

// --- 子协商 ---

void TelnetProtocol::handleSubNegotiation()
{
    switch (m_subOption) {
    case OPT_TTYPE:
        // 对端问终端类型：IAC SB TTYPE SEND IAC SE
        if (!m_subBuffer.isEmpty() && byteAt(m_subBuffer, 0) == TTYPE_SEND)
            sendTerminalType();
        break;
    case OPT_NAWS:
        // 客户端方向的 NAWS 只上报不接收，对端不该往这边发。忽略。
        break;
    default:
        // 未协商同意的选项的子协商，忽略即可（我们已经 WONT/DONT 过了）。
        break;
    }
}

void TelnetProtocol::sendTerminalType()
{
    if (!m_negotiate)
        return;

    // IAC SB TTYPE IS <type> IAC SE
    QByteArray msg;
    msg.append(char(IAC));
    msg.append(char(SB));
    msg.append(char(OPT_TTYPE));
    msg.append(char(TTYPE_IS));
    // 终端类型是 ASCII，理论上不含 0xFF；仍走一遍转义以防配置里填了怪东西。
    msg.append(escapeOutgoing(m_termType));
    msg.append(char(IAC));
    msg.append(char(SE));
    emit sendResponse(msg);
}

void TelnetProtocol::sendWindowSize(int columns, int rows)
{
    if (columns <= 0 || rows <= 0)
        return;
    if (columns == m_columns && rows == m_rows)
        return;   // 尺寸没变，不刷屏似地重发

    m_columns = columns;
    m_rows = rows;
    emitWindowSize();
}

void TelnetProtocol::emitWindowSize()
{
    // 对端还没 DO NAWS 时只记不发；协商成功后 handleDo 会补发一次。
    if (!m_negotiate || !m_nawsAgreed || m_columns <= 0 || m_rows <= 0)
        return;

    // IAC SB NAWS <width:16be> <height:16be> IAC SE（RFC 1073）
    // 尺寸字节里出现 255 时必须按 IAC IAC 转义 —— 一个 255 列宽的终端不常见
    // 但完全合法，不转义会让对端把它当成命令前导，整段子协商就废了。
    QByteArray payload;
    payload.append(char((m_columns >> 8) & 0xFF));
    payload.append(char(m_columns & 0xFF));
    payload.append(char((m_rows >> 8) & 0xFF));
    payload.append(char(m_rows & 0xFF));

    QByteArray msg;
    msg.append(char(IAC));
    msg.append(char(SB));
    msg.append(char(OPT_NAWS));
    msg.append(escapeOutgoing(payload));
    msg.append(char(IAC));
    msg.append(char(SE));
    emit sendResponse(msg);
}

} // namespace cubeshell
