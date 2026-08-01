#pragma once

// PickleReader.h — minimal Python-pickle reader for cube-shell's config.dat.
//
// config.dat is `pickle.dumps({name: [user, password, host, key_type, key_file]})`.
// That is a tiny, well-defined subset of pickle (protocol 2/4): a dict whose
// values are lists of unicode strings. This reader parses exactly that subset
// (plus the memo machinery CPython emits) — it is NOT a general pickle parser.
//
// On success returns the device map; on any deviation from the expected
// subset it fails cleanly (returns false) so the caller can fall back.

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace cubeshell {

class PickleReader {
public:
    // Parse a config.dat-style pickle into { deviceName -> fields }.
    // fields is a QStringList (typically [user, password, host, keyType, keyFile]).
    static bool parseDeviceConfig(const QByteArray &data,
                                  QHash<QString, QStringList> &out,
                                  QString *errorOut = nullptr);

    // A tiny value model for the subset we support (public: it is a plain
    // data structure used by the parser implementation).
    struct Value {
        enum Type { None, Str, List, Dict, Mark } type = None;
        QString str;
        QList<Value> list;
        // Dict stored as flat key/value pairs (keys are strings in our subset).
        QList<QPair<Value, Value>> dict;
    };

    // Parse the pickle and return the raw root dict Value. Callers that need
    // to distinguish list entries (SSH) from dict entries (RDP, __type__ ==
    // "rdp") use this instead of parseDeviceConfig.
    // 对应Python: util.device_protocol 用 isinstance(list/dict) 判别协议
    static bool parseRoot(const QByteArray &data, Value &out,
                          QString *errorOut = nullptr);

private:
    class Parser {
    public:
        explicit Parser(const QByteArray &data) : d(data) {}
        bool run(Value &out, QString *err);

    private:
        bool parseOne(QString *err);
        Value pop(QString *err);
        void push(const Value &v) { stack.append(v); }
        QString readUtf8(quint32 len);

        const QByteArray &d;
        int pos = 0;
        QList<Value> stack;
        QList<Value> memo;          // BINPUT/LONG_BINPUT slots (index-addressed)
        QList<int> markStack;       // positions of MARK on the value stack
    };
};

} // namespace cubeshell
