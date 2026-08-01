// PickleReader.cpp — minimal Python-pickle reader. See PickleReader.h.
//
// Supported opcodes (the subset CPython emits for a dict of lists of strings,
// protocol 2 and 4):
//   PROTO, EMPTY_DICT, EMPTY_LIST, MARK, SETITEMS, APPENDS,
//   BINUNICODE, SHORT_BINUNICODE, BINUNICODE8, UNICODE,
//   BINPUT, LONG_BINPUT, BINGET, LONG_BINGET, STOP
// Anything else aborts with an error (caller falls back).

#include "PickleReader.h"

namespace cubeshell {

// Opcode bytes (CPython pickletools).
static constexpr char OP_PROTO            = '\x80';
static constexpr char OP_EMPTY_DICT       = '}';
static constexpr char OP_EMPTY_LIST       = ']';
static constexpr char OP_MARK             = '(';
static constexpr char OP_SETITEMS         = 'u';
static constexpr char OP_APPENDS          = 'e';
static constexpr char OP_BINUNICODE       = 'X';
static constexpr char OP_SHORT_BINUNICODE = '\x8c';
static constexpr char OP_BINUNICODE8      = '\x8d';
static constexpr char OP_UNICODE          = 'V';
static constexpr char OP_BINPUT           = 'q';
static constexpr char OP_LONG_BINPUT      = 'r';
static constexpr char OP_BINGET           = 'h';
static constexpr char OP_LONG_BINGET      = 'j';
static constexpr char OP_STOP             = '.';
static constexpr char OP_NONE             = 'N';
static constexpr char OP_SETITEM          = 's';
static constexpr char OP_MEMOIZE          = '\x94';
static constexpr char OP_FRAME            = '\x95';

using V = PickleReader::Value;

QString PickleReader::Parser::readUtf8(quint32 len)
{
    if (pos + int(len) > d.size())
        return QString();
    QString s = QString::fromUtf8(d.constData() + pos, int(len));
    pos += int(len);
    return s;
}

V PickleReader::Parser::pop(QString *err)
{
    if (stack.isEmpty()) {
        if (err) *err = QStringLiteral("stack underflow");
        return V{};
    }
    V v = stack.takeLast();
    return v;
}

bool PickleReader::Parser::run(V &out, QString *err)
{
    while (pos < d.size()) {
        if (!parseOne(err))
            return false;
        // STOP leaves the top of stack as the result.
        if (!stack.isEmpty() && pos >= d.size())
            break;
        if (!stack.isEmpty() && stack.constLast().type == V::Mark)
            continue;
    }
    if (stack.isEmpty()) {
        if (err) *err = QStringLiteral("empty pickle");
        return false;
    }
    out = stack.takeLast();
    return true;
}

bool PickleReader::Parser::parseOne(QString *err)
{
    if (pos >= d.size()) {
        if (err) *err = QStringLiteral("unexpected end of data");
        return false;
    }
    const char op = d.at(pos++);

    auto need = [&](int n) -> bool {
        if (pos + n > d.size()) { if (err) *err = QStringLiteral("truncated"); return false; }
        return true;
    };
    auto rdU8 = [&]() { return quint8(d.at(pos++)); };
    auto rdU32 = [&]() {
        quint32 v = quint8(d.at(pos)) | (quint8(d.at(pos+1)) << 8)
                  | (quint8(d.at(pos+2)) << 16) | (quint32(quint8(d.at(pos+3))) << 24);
        pos += 4;
        return v;
    };

    switch (op) {
    case OP_PROTO: {
        if (!need(1)) return false;
        ++pos; // protocol byte
        return true;
    }
    case OP_FRAME: {
        // protocol 4 framing: 8-byte little-endian frame length; we just skip it.
        if (!need(8)) return false;
        pos += 8;
        return true;
    }
    case OP_STOP:
        return true;

    case OP_EMPTY_DICT: { V v; v.type = V::Dict; push(v); return true; }
    case OP_EMPTY_LIST: { V v; v.type = V::List; push(v); return true; }
    case OP_NONE:       { V v; v.type = V::None; push(v); return true; }

    case OP_MEMOIZE: {
        // protocol 4: memoize top-of-stack into the next memo slot.
        if (!stack.isEmpty()) memo.append(stack.constLast());
        return true;
    }

    case OP_MARK:
        markStack.append(stack.size());
        return true;

    case OP_BINUNICODE: {
        if (!need(4)) return false;
        quint32 len = rdU32();
        V v; v.type = V::Str; v.str = readUtf8(len);
        if (v.str.isNull() && len > 0) { if (err) *err = QStringLiteral("bad unicode"); return false; }
        push(v);
        return true;
    }
    case OP_SHORT_BINUNICODE: {
        if (!need(1)) return false;
        quint32 len = rdU8();
        V v; v.type = V::Str; v.str = readUtf8(len);
        push(v);
        return true;
    }
    case OP_BINUNICODE8: {
        if (!need(8)) { if (err) *err = QStringLiteral("truncated binunicode8"); return false; }
        // 8-byte little-endian length; we only handle lengths that fit in u32.
        quint32 lo = rdU32();
        quint32 hi = rdU32();
        if (hi != 0) { if (err) *err = QStringLiteral("string too long"); return false; }
        V v; v.type = V::Str; v.str = readUtf8(lo);
        push(v);
        return true;
    }
    case OP_UNICODE: {
        // Raw-unicode-escape until newline.
        int nl = d.indexOf('\n', pos);
        if (nl < 0) { if (err) *err = QStringLiteral("unterminated UNICODE"); return false; }
        V v; v.type = V::Str;
        v.str = QString::fromUtf8(d.constData() + pos, nl - pos);
        pos = nl + 1;
        push(v);
        return true;
    }

    case OP_BINPUT: {
        if (!need(1)) return false;
        int idx = rdU8();
        while (memo.size() <= idx) memo.append(V{});
        if (!stack.isEmpty()) memo[idx] = stack.constLast();
        return true;
    }
    case OP_LONG_BINPUT: {
        if (!need(4)) return false;
        quint32 idx = rdU32();
        while (memo.size() <= int(idx)) memo.append(V{});
        if (!stack.isEmpty()) memo[idx] = stack.constLast();
        return true;
    }
    case OP_BINGET: {
        if (!need(1)) return false;
        int idx = rdU8();
        if (idx >= memo.size()) { if (err) *err = QStringLiteral("bad memo get"); return false; }
        push(memo[idx]);
        return true;
    }
    case OP_LONG_BINGET: {
        if (!need(4)) return false;
        quint32 idx = rdU32();
        if (int(idx) >= memo.size()) { if (err) *err = QStringLiteral("bad memo get"); return false; }
        push(memo[int(idx)]);
        return true;
    }

    case OP_SETITEM: {
        // Single key/value pair, no MARK: pop value then key, append to the dict below.
        if (stack.size() < 3) { if (err) *err = QStringLiteral("SETITEM stack too small"); return false; }
        V value = stack.takeLast();
        V key   = stack.takeLast();
        if (stack.constLast().type != V::Dict) { if (err) *err = QStringLiteral("SETITEM target not a dict"); return false; }
        stack.last().dict.append({key, value});
        return true;
    }
    case OP_SETITEMS: {
        if (markStack.isEmpty()) { if (err) *err = QStringLiteral("SETITEMS without MARK"); return false; }
        int mark = markStack.takeLast();
        // Find the dict (the item just below the mark).
        if (mark < 1 || stack[mark - 1].type != V::Dict) { if (err) *err = QStringLiteral("SETITEMS target not a dict"); return false; }
        V dictVal = stack[mark - 1];
        // Everything from mark..top is key,value,key,value...
        for (int i = mark; i + 1 < stack.size(); i += 2)
            dictVal.dict.append({stack[i], stack[i + 1]});
        stack.resize(mark - 1);
        push(dictVal);
        return true;
    }
    case OP_APPENDS: {
        if (markStack.isEmpty()) { if (err) *err = QStringLiteral("APPENDS without MARK"); return false; }
        int mark = markStack.takeLast();
        if (mark < 1 || stack[mark - 1].type != V::List) { if (err) *err = QStringLiteral("APPENDS target not a list"); return false; }
        V listVal = stack[mark - 1];
        for (int i = mark; i < stack.size(); ++i)
            listVal.list.append(stack[i]);
        stack.resize(mark - 1);
        push(listVal);
        return true;
    }

    default:
        if (err) *err = QStringLiteral("unsupported pickle opcode 0x%1").arg(QString::number(quint8(op), 16));
        return false;
    }
}

bool PickleReader::parseRoot(const QByteArray &data, Value &out, QString *errorOut)
{
    Parser p(data);
    QString err;
    if (!p.run(out, &err)) {
        if (errorOut) *errorOut = err;
        return false;
    }
    if (out.type != Value::Dict) {
        if (errorOut) *errorOut = QStringLiteral("root is not a dict");
        return false;
    }
    return true;
}

bool PickleReader::parseDeviceConfig(const QByteArray &data,
                                     QHash<QString, QStringList> &out,
                                     QString *errorOut)
{
    V root;
    if (!parseRoot(data, root, errorOut))
        return false;

    for (const auto &kv : root.dict) {
        if (kv.first.type != V::Str)
            continue;
        QStringList fields;
        if (kv.second.type == V::List) {
            for (const V &item : kv.second.list)
                fields << (item.type == V::Str ? item.str : QString());
        }
        out.insert(kv.first.str, fields);
    }
    return true;
}

} // namespace cubeshell
