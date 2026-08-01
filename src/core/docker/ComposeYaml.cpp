// ComposeYaml.cpp — Docker Compose YAML 解析/序列化（yaml-cpp）。见 ComposeYaml.h。

#include "ComposeYaml.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QVariantList>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <limits>

namespace cubeshell {

namespace ComposeYaml {

namespace {

// ---------------------------------------------------------------------------
// YAML::Node -> QVariant（对应 yaml.safe_load 的类型推断）
// ---------------------------------------------------------------------------

// PyYAML (YAML 1.1) 的标量隐式类型正则。yaml-cpp 的 as<bool>() 只认
// true/false，as<long long>() 会把 "yes" 当失败但把 "0x10" 当 0——都与
// safe_load 不一致，所以这里按 Tag() + 词法自行判定。
const QRegularExpression &reBoolTrue()
{
    static const QRegularExpression re(
        QStringLiteral("^(?:yes|Yes|YES|true|True|TRUE|on|On|ON)$"));
    return re;
}

const QRegularExpression &reBoolFalse()
{
    static const QRegularExpression re(
        QStringLiteral("^(?:no|No|NO|false|False|FALSE|off|Off|OFF)$"));
    return re;
}

const QRegularExpression &reNull()
{
    static const QRegularExpression re(
        QStringLiteral("^(?:~|null|Null|NULL|)$"));
    return re;
}

const QRegularExpression &reInt()
{
    // 十进制 + 0x 十六进制（PyYAML 还认 0b/0o/60 进制，compose 用不到）。
    static const QRegularExpression re(
        QStringLiteral("^[-+]?(?:[0-9]+|0x[0-9a-fA-F]+)$"));
    return re;
}

const QRegularExpression &reFloat()
{
    // PyYAML 的 float 需要小数点（"1e5" 是字符串），另认 .inf/.nan。
    static const QRegularExpression re(QStringLiteral(
        "^[-+]?(?:[0-9]+\\.[0-9]*(?:[eE][-+]?[0-9]+)?"
        "|\\.[0-9]+(?:[eE][-+]?[0-9]+)?"
        "|\\.(?:inf|Inf|INF))$|^\\.(?:nan|NaN|NAN)$"));
    return re;
}

// 未加引号的纯标量（Tag() == "?"）按 YAML 1.1 隐式规则推断类型。
QVariant resolvePlainScalar(const QString &text)
{
    if (reNull().match(text).hasMatch())
        return QVariant(); // None
    if (reBoolTrue().match(text).hasMatch())
        return QVariant(true);
    if (reBoolFalse().match(text).hasMatch())
        return QVariant(false);
    if (reInt().match(text).hasMatch()) {
        bool ok = false;
        // toLongLong(base=0) 自动识别 0x 前缀；十进制显式禁用八进制歧义。
        const qlonglong v = text.startsWith(QLatin1String("0x")) ||
                                    text.startsWith(QLatin1String("-0x")) ||
                                    text.startsWith(QLatin1String("+0x"))
                                ? text.toLongLong(&ok, 16)
                                : text.toLongLong(&ok, 10);
        if (ok)
            return QVariant(v);
    }
    if (reFloat().match(text).hasMatch()) {
        const QString lowered = text.toLower();
        if (lowered.endsWith(QLatin1String(".inf")))
            return QVariant(lowered.startsWith(QLatin1Char('-'))
                                ? -std::numeric_limits<double>::infinity()
                                : std::numeric_limits<double>::infinity());
        if (lowered == QLatin1String(".nan"))
            return QVariant(std::numeric_limits<double>::quiet_NaN());
        bool ok = false;
        const double v = text.toDouble(&ok);
        if (ok)
            return QVariant(v);
    }
    return QVariant(text);
}

QVariant nodeToVariant(const YAML::Node &node)
{
    switch (node.Type()) {
    case YAML::NodeType::Null:
    case YAML::NodeType::Undefined:
        return QVariant();
    case YAML::NodeType::Scalar: {
        const QString text = QString::fromStdString(node.Scalar());
        const std::string tag = node.Tag();
        // "!" = 加过引号/显式非特定标签 -> 一律字符串（safe_load 同）。
        if (tag == "!")
            return QVariant(text);
        // 显式 tag:yaml.org,2002:xxx 按 tag 转换。
        if (tag.size() > 1 && tag != "?") {
            if (tag.rfind(":str") != std::string::npos)
                return QVariant(text);
            if (tag.rfind(":bool") != std::string::npos)
                return QVariant(reBoolTrue().match(text).hasMatch());
            if (tag.rfind(":int") != std::string::npos)
                return QVariant(text.toLongLong());
            if (tag.rfind(":float") != std::string::npos)
                return QVariant(text.toDouble());
            if (tag.rfind(":null") != std::string::npos)
                return QVariant();
            return QVariant(text);
        }
        return resolvePlainScalar(text);
    }
    case YAML::NodeType::Sequence: {
        QVariantList list;
        list.reserve(static_cast<int>(node.size()));
        for (const YAML::Node &item : node)
            list.append(nodeToVariant(item));
        return list;
    }
    case YAML::NodeType::Map: {
        QVariantMap map;
        for (auto it = node.begin(); it != node.end(); ++it)
            map.insert(QString::fromStdString(it->first.Scalar()),
                       nodeToVariant(it->second));
        return map;
    }
    }
    return QVariant();
}

// ---------------------------------------------------------------------------
// QVariant -> YAML::Emitter（对应 yaml.dump(default_flow_style=False)）
// ---------------------------------------------------------------------------

// dump 时的字段优先级（见 ComposeYaml.h 文件头说明）。
const QStringList &topLevelOrder()
{
    static const QStringList order = {
        QStringLiteral("version"), QStringLiteral("services"),
        QStringLiteral("networks"), QStringLiteral("volumes"),
    };
    return order;
}

const QStringList &serviceFieldOrder()
{
    static const QStringList order = {
        QStringLiteral("image"),       QStringLiteral("container_name"),
        QStringLiteral("restart"),     QStringLiteral("command"),
        QStringLiteral("build"),       QStringLiteral("ports"),
        QStringLiteral("environment"), QStringLiteral("volumes"),
        QStringLiteral("depends_on"),  QStringLiteral("networks"),
    };
    return order;
}

// 优先级表内按表序，表外按字母序排在其后。
QStringList orderedKeys(const QVariantMap &map, const QStringList &priority)
{
    QStringList keys = map.keys(); // QVariantMap 已按字母序
    std::stable_sort(keys.begin(), keys.end(),
                     [&priority](const QString &a, const QString &b) {
        const int ia = static_cast<int>(priority.indexOf(a));
        const int ib = static_cast<int>(priority.indexOf(b));
        if (ia >= 0 && ib >= 0)
            return ia < ib;
        if (ia != ib)
            return ia >= 0; // 优先级字段排前
        return a < b;
    });
    return keys;
}

// 纯标量输出会被回读成 bool/int/... 的字符串必须加引号，才能往返保真
// （PyYAML 的 dump 同样会给 "yes"、"123" 加引号）。
bool needsQuoting(const QString &text)
{
    return reNull().match(text).hasMatch()
        || reBoolTrue().match(text).hasMatch()
        || reBoolFalse().match(text).hasMatch()
        || reInt().match(text).hasMatch()
        || reFloat().match(text).hasMatch();
}

void emitVariant(YAML::Emitter &out, const QVariant &value, bool topLevel)
{
    switch (value.typeId()) {
    case QMetaType::QVariantMap: {
        const QVariantMap map = value.toMap();
        out << YAML::BeginMap;
        const QStringList keys =
            orderedKeys(map, topLevel ? topLevelOrder() : serviceFieldOrder());
        for (const QString &key : keys) {
            out << YAML::Key << key.toStdString();
            out << YAML::Value;
            emitVariant(out, map.value(key), false);
        }
        out << YAML::EndMap;
        return;
    }
    case QMetaType::QVariantList: {
        out << YAML::BeginSeq;
        const QVariantList list = value.toList();
        for (const QVariant &item : list)
            emitVariant(out, item, false);
        out << YAML::EndSeq;
        return;
    }
    case QMetaType::Bool:
        out << value.toBool();
        return;
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        out << value.toLongLong();
        return;
    case QMetaType::Double:
    case QMetaType::Float:
        out << value.toDouble();
        return;
    default:
        break;
    }
    if (!value.isValid() || value.isNull()) {
        out << YAML::Null;
        return;
    }
    const QString text = value.toString();
    if (needsQuoting(text))
        out << YAML::DoubleQuoted;
    out << text.toStdString();
}

} // namespace

// ---------------------------------------------------------------------------
// 公开 API
// ---------------------------------------------------------------------------

// 对应Python: yaml.safe_load(yaml_text) or {}
QVariantMap parseCompose(const QString &yamlText, QString *errorOut)
{
    YAML::Node root;
    try {
        root = YAML::Load(yamlText.toStdString());
    } catch (const YAML::Exception &e) {
        if (errorOut)
            *errorOut = QStringLiteral("YAML 解析失败: %1")
                            .arg(QString::fromStdString(e.what()));
        return {};
    }
    if (root.IsNull() || !root.IsDefined())
        return {}; // 空文档 -> 空 map（Python 侧的 `or {}`）
    if (!root.IsMap()) {
        if (errorOut)
            *errorOut = QStringLiteral("YAML 根节点不是映射");
        return {};
    }
    return nodeToVariant(root).toMap();
}

// 对应Python: yaml.dump(config, default_flow_style=False, sort_keys=False,
//             allow_unicode=True)。字段顺序策略见头文件。
QString dumpCompose(const QVariantMap &config)
{
    YAML::Emitter out;
    // yaml-cpp 默认不转义非 ASCII，UTF-8 原样输出 == allow_unicode=True。
    emitVariant(out, QVariant(config), true);
    QString text = QString::fromUtf8(out.c_str());
    if (!text.endsWith(QLatin1Char('\n')))
        text.append(QLatin1Char('\n')); // PyYAML dump 以换行结尾
    return text;
}

// 对应Python: docker_compose_editor.py::load_predefined_services
QList<ComposeServiceInfo> loadComposeServices(const QString &filePath,
                                              QString *errorOut)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorOut)
            *errorOut = QStringLiteral("无法打开 %1").arg(filePath);
        return {};
    }
    // Python 侧显式 encoding='utf-8'（避免 Windows GBK），这里同样按 UTF-8 读。
    const QString yamlText = QString::fromUtf8(f.readAll());

    // 直接遍历 YAML::Node 以保留文件中的服务书写顺序
    //（QVariantMap 会按 key 重排，UI 列表顺序会乱）。
    YAML::Node root;
    try {
        root = YAML::Load(yamlText.toStdString());
    } catch (const YAML::Exception &e) {
        if (errorOut)
            *errorOut = QStringLiteral("YAML 解析失败: %1")
                            .arg(QString::fromStdString(e.what()));
        return {};
    }
    if (!root.IsMap())
        return {};
    const YAML::Node services = root["services"];
    if (!services.IsDefined() || !services.IsMap())
        return {};

    QList<ComposeServiceInfo> result;
    for (auto it = services.begin(); it != services.end(); ++it) {
        ComposeServiceInfo info;
        info.name = QString::fromStdString(it->first.Scalar());
        info.config = nodeToVariant(it->second).toMap();

        // description 取 labels.description；labels 可能是 map，也可能是
        // "k=v" 字符串列表（compose 两种写法都合法），缺失时留空串。
        const QVariant labels = info.config.value(QStringLiteral("labels"));
        if (labels.typeId() == QMetaType::QVariantMap) {
            info.description =
                labels.toMap().value(QStringLiteral("description")).toString();
        } else if (labels.typeId() == QMetaType::QVariantList) {
            const QVariantList items = labels.toList();
            for (const QVariant &item : items) {
                const QString entry = item.toString();
                const qsizetype eq = entry.indexOf(QLatin1Char('='));
                if (eq > 0 && entry.left(eq) == QLatin1String("description")) {
                    info.description = entry.mid(eq + 1);
                    break;
                }
            }
        }
        result.append(info);
    }
    return result;
}

// 对应Python: load_predefined_services 中 project_root/conf/docker-compose-full.yml
// 探测顺序仿照 AiPreferences::locateProvidersConfig。
QString defaultComposeFullPath()
{
    const QString fileName = QStringLiteral("docker-compose-full.yml");
    QStringList candidates;
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        candidates
            << appDir + QStringLiteral("/conf/") + fileName
            << appDir + QStringLiteral("/../conf/") + fileName
            // macOS bundle: Contents/MacOS -> Contents/Resources/conf/
            << appDir + QStringLiteral("/../Resources/conf/") + fileName
            // 安装布局: <prefix>/bin -> <prefix>/share/cube-shell/conf/
            << appDir + QStringLiteral("/../share/cube-shell/conf/") + fileName
            << appDir + QStringLiteral("/../../conf/") + fileName;
    }
    candidates << QDir::currentPath() + QStringLiteral("/conf/") + fileName;
#ifdef CUBESHELL_SOURCE_CONF_DIR
    // Dev tree: cpp/build/... -> repo root conf/（CMake 注入的源码树路径）。
    candidates << QStringLiteral(CUBESHELL_SOURCE_CONF_DIR "/") + fileName;
#endif

    for (const QString &path : candidates) {
        if (QFileInfo::exists(path))
            return QFileInfo(path).absoluteFilePath();
    }
    return QString();
}

} // namespace ComposeYaml

} // namespace cubeshell
