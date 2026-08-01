// StringUtil.cpp — 字符串/格式化/路径通用工具实现。
// 对应Python: function/util.py

#include "StringUtil.h"

#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QStringList>

namespace cubeshell {
namespace StringUtil {

// 对应Python: function/util.py::APP_NAME
QString appName()
{
    return QStringLiteral("cube-shell");
}

// 对应Python: function/util.py::BANNER
QString banner()
{
    return QStringLiteral(
        "\n"
        "               _                         _            _  _ \n"
        "              | |                       | |          | || |\n"
        "   ___  _   _ | |__    ___  ______  ___ | |__    ___ | || |\n"
        "  / __|| | | || '_ \\  / _ \\|______|/ __|| '_ \\  / _ \\| || |\n"
        " | (__ | |_| || |_) ||  __/        \\__ \\| | | ||  __/| || |\n"
        "  \\___| \\__,_||_.__/  \\___|        |___/|_| |_| \\___||_||_|\n \n"
        "欢迎使用 cube-shell SSH 服务器远程管理工具 如有疑问请在项目主页联系作者\n                                            \n");
}

// 对应Python: function/util.py::format_file_size
QString formatFileSize(qint64 sizeInBytes)
{
    if (sizeInBytes < MaxBytesSize)
        return QStringLiteral("%1 字节").arg(sizeInBytes);
    if (sizeInBytes < MaxKbSize)
        return QStringLiteral("%1 KB").arg(double(sizeInBytes) / 1024.0, 0, 'f', 2);
    if (sizeInBytes < MaxMbSize)
        return QStringLiteral("%1 MB").arg(double(sizeInBytes) / (1024.0 * 1024.0), 0, 'f', 2);
    // Python 侧 GB 用 3 位小数
    return QStringLiteral("%1 GB").arg(double(sizeInBytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 3);
}

// 对应Python: function/util.py::format_speed
QString formatSpeed(double bytesPerSecond)
{
    if (bytesPerSecond >= 1024.0 * 1024.0)
        return QStringLiteral("%1 MB/s").arg(bytesPerSecond / (1024.0 * 1024.0), 0, 'f', 2);
    if (bytesPerSecond >= 1024.0)
        return QStringLiteral("%1 KB/s").arg(bytesPerSecond / 1024.0, 0, 'f', 2);
    return QStringLiteral("%1 B/s").arg(bytesPerSecond, 0, 'f', 0);
}

// 对应Python: function/util.py::has_valid_suffix
bool hasValidSuffix(const QString &filename)
{
    // 与 Python 元组逐项一致（str.endswith 为大小写敏感，这里保持一致）
    static const QStringList suffixes = {
        QStringLiteral(".db"),   QStringLiteral(".exe"),  QStringLiteral(".bin"),
        QStringLiteral(".jar"),  QStringLiteral(".pdf"),  QStringLiteral(".doc"),
        QStringLiteral(".docx"), QStringLiteral(".xls"),  QStringLiteral(".xlsx"),
        QStringLiteral(".ppt"),  QStringLiteral(".pptx"), QStringLiteral(".zip"),
        QStringLiteral(".rar"),  QStringLiteral(".7z"),   QStringLiteral(".tar"),
        QStringLiteral(".gz"),   QStringLiteral(".bz2"),  QStringLiteral(".iso"),
        QStringLiteral(".img"),  QStringLiteral(".dmg"),  QStringLiteral(".apk"),
        QStringLiteral(".ipa"),  QStringLiteral(".deb"),  QStringLiteral(".rpm"),
        QStringLiteral(".msi"),  QStringLiteral(".war"),  QStringLiteral(".ear"),
        QStringLiteral(".dmp"),  QStringLiteral(".phd"),  QStringLiteral(".trc"),
        QStringLiteral(".Xauthority"),
    };
    for (const QString &suffix : suffixes) {
        if (filename.endsWith(suffix, Qt::CaseSensitive))
            return true;
    }
    return false;
}

// 对应Python: function/util.py::remove_special_lines
QString removeSpecialLines(const QString &text)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    QStringList filtered;
    for (const QString &line : lines) {
        const QString stripped = line.trimmed();
        if (stripped.isEmpty())
            continue;
        // 如果行不只包含波浪号，则保留（trimmed 后已无空白字符）
        bool hasNonTilde = false;
        for (const QChar &ch : stripped) {
            if (ch != QLatin1Char('~')) {
                hasNonTilde = true;
                break;
            }
        }
        if (hasNonTilde)
            filtered.append(line);
    }
    return filtered.join(QLatin1Char('\n'));
}

// 对应Python: function/util.py::is_ipv6_address
bool isIpv6Address(const QString &address)
{
    // Python 用 strip('[]') 去除方括号后 inet_pton(AF_INET6) 校验；
    // 这里用 QHostAddress 解析并判断协议族
    QString clean = address.trimmed();
    clean.remove(QLatin1Char('[')).remove(QLatin1Char(']'));
    QHostAddress addr;
    if (!addr.setAddress(clean))
        return false;
    return addr.protocol() == QAbstractSocket::IPv6Protocol;
}

// 对应Python: function/util.py::symbolic_to_octal
int symbolicToOctal(const QString &symbolic)
{
    // Python 侧对 9 位权限串（不含首位文件类型字符）按 rwx 三组求值
    auto calcPermission = [](const QString &perm) {
        int value = 0;
        for (const QChar &ch : perm) {
            if (ch == QLatin1Char('r'))
                value += 4;
            else if (ch == QLatin1Char('w'))
                value += 2;
            else if (ch == QLatin1Char('x'))
                value += 1;
        }
        return value;
    };
    const QString user = symbolic.mid(0, 3);
    const QString group = symbolic.mid(3, 3);
    const QString others = symbolic.mid(6, 3);
    return calcPermission(user) * 100 + calcPermission(group) * 10 + calcPermission(others);
}

// 对应Python: function/util.py::copy_config_to_conf 中 os.path.join 用法
QString joinPath(const QString &dir, const QString &name)
{
    if (QDir::isAbsolutePath(name))
        return QDir::cleanPath(name);
    if (dir.isEmpty())
        return name;
    return QDir::cleanPath(dir + QLatin1Char('/') + name);
}

// 对应Python: function/util.py::copy_config_to_conf 中 os.path.basename 用法
QString baseName(const QString &path)
{
    return QFileInfo(path).fileName();
}

} // namespace StringUtil
} // namespace cubeshell
