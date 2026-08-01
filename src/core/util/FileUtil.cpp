// FileUtil.cpp — 本地文件遍历、压缩/解压、frp 配置模板实现。
// 对应Python: core/compressor.py + function/traversal.py

#include "FileUtil.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace cubeshell {
namespace FileUtil {

namespace {

// 运行外部命令并等待结束（无超时上限交给调用方线程管理；这里给一个
// 足够宽松的上限防止永久阻塞）。返回 exit code，-1 表示无法启动。
int runProcess(const QString &program, const QStringList &args,
               const QString &workingDir, QString *stdErrOut,
               QString *stdOutOut = nullptr)
{
    QProcess proc;
    if (!workingDir.isEmpty())
        proc.setWorkingDirectory(workingDir);
    proc.start(program, args);
    if (!proc.waitForStarted(5000)) {
        if (stdErrOut)
            *stdErrOut = QStringLiteral("failed to start: %1").arg(program);
        return -1;
    }
    // 大归档可能耗时较长，与 Python 侧"无超时轮询"语义保持宽松一致
    if (!proc.waitForFinished(30 * 60 * 1000)) {
        proc.kill();
        proc.waitForFinished(3000);
        if (stdErrOut)
            *stdErrOut = QStringLiteral("%1 timed out").arg(program);
        return -1;
    }
    if (stdErrOut)
        *stdErrOut = QString::fromUtf8(proc.readAllStandardError());
    if (stdOutOut)
        *stdOutOut = QString::fromUtf8(proc.readAllStandardOutput());
    return proc.exitStatus() == QProcess::NormalExit ? proc.exitCode() : -1;
}

// 对应Python: core/compressor.py 中 "command -v zip >/dev/null 2>&1" 检查
bool commandExists(const QString &name)
{
    return !QStandardPaths::findExecutable(name).isEmpty();
}

} // namespace

// 对应Python: core/compressor.py::CompressThread.run 中 os.walk 逐文件收集逻辑
QStringList listFilesRecursive(const QString &baseDir)
{
    QStringList out;
    const QDir base(baseDir);
    QDirIterator it(baseDir, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        // arcname 使用相对 base_dir 的路径，保证解压后目录结构一致
        out.append(base.relativeFilePath(it.filePath()));
    }
    out.sort();
    return out;
}

// 对应Python: core/compressor.py::DecompressThread.run::_ensure_within_base
bool isPathWithinBase(const QString &base, const QString &target)
{
    const QString baseAbs = QDir::cleanPath(QDir(base).absolutePath());
    const QString targetAbs = QDir::cleanPath(QDir(base).absoluteFilePath(target));
    return targetAbs == baseAbs || targetAbs.startsWith(baseAbs + QLatin1Char('/'));
}

// 对应Python: core/compressor.py::CompressThread.run
ArchiveResult compress(const QString &baseDir, const QStringList &files,
                       const QString &outputName, ArchiveFormat format)
{
    ArchiveResult result;
    if (files.isEmpty()) {
        result.message = QStringLiteral("No files to compress");
        return result;
    }
    const QDir base(baseDir);
    if (!base.exists()) {
        result.message = QStringLiteral("Directory not found: %1").arg(baseDir);
        return result;
    }

    QString program;
    QStringList args;
    if (format == ArchiveFormat::TarGz) {
        // 对应Python: cmd = f"cd {pwd} && tar -czf {output} {files}"
        program = QStringLiteral("tar");
        args << QStringLiteral("-czf") << outputName;
        args << files;
    } else {
        // 对应Python: cmd = f"cd {pwd} && zip -r {output} {files}"
        if (!commandExists(QStringLiteral("zip"))) {
            result.message = QStringLiteral(
                "zip command not found. Please install zip (e.g., 'apt install zip' or 'yum install zip').");
            return result;
        }
        program = QStringLiteral("zip");
        args << QStringLiteral("-r") << outputName;
        args << files;
    }

    QString errOut;
    const int code = runProcess(program, args, base.absolutePath(), &errOut);
    if (code == 0) {
        result.success = true;
        result.message = QStringLiteral("Compression task finished");
    } else {
        result.message = errOut.isEmpty() ? QStringLiteral("Unknown error") : errOut;
    }
    return result;
}

// 列出归档内条目路径，用于目录穿越校验。
// zip 用 `unzip -Z1`，tar 用 `tar -tf`。
static QStringList listArchiveEntries(const QString &archivePath, bool isZip,
                                      bool gzipped, QString *errorOut)
{
    QString program;
    QStringList args;
    if (isZip) {
        program = QStringLiteral("unzip");
        args << QStringLiteral("-Z1") << archivePath;
    } else {
        program = QStringLiteral("tar");
        args << (gzipped ? QStringLiteral("-tzf") : QStringLiteral("-tf")) << archivePath;
    }
    QString errOut, stdOut;
    const int code = runProcess(program, args, QString(), &errOut, &stdOut);
    if (code != 0) {
        if (errorOut)
            *errorOut = errOut;
        return {};
    }
    QStringList entries = stdOut.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    return entries;
}

// 对应Python: core/compressor.py::DecompressThread.run
ArchiveResult decompress(const QString &archivePath, const QString &destDir)
{
    ArchiveResult result;
    const QFileInfo fi(archivePath);
    if (!fi.exists()) {
        result.message = QStringLiteral("Archive not found: %1").arg(archivePath);
        return result;
    }
    const QString name = fi.fileName();
    const bool isZip = name.endsWith(QStringLiteral(".zip"));
    const bool isTarGz = name.endsWith(QStringLiteral(".tar.gz")) || name.endsWith(QStringLiteral(".tgz"));
    const bool isTar = !isTarGz && name.endsWith(QStringLiteral(".tar"));
    if (!isZip && !isTarGz && !isTar) {
        // 对应Python: "Unsupported archive format: {filename}"
        result.message = QStringLiteral("Unsupported archive format: %1").arg(name);
        return result;
    }
    if (isZip && !commandExists(QStringLiteral("unzip"))) {
        // 对应Python: "unzip command not found. Please install unzip on the server."
        result.message = QStringLiteral("unzip command not found. Please install unzip.");
        return result;
    }

    // zip-slip/tar-slip 防护：任何条目解出来不在 destDir 内则拒绝执行
    // 对应Python: DecompressThread.run 中 extractall 前的路径校验
    QString listErr;
    const QStringList entries = listArchiveEntries(fi.absoluteFilePath(), isZip, isTarGz, &listErr);
    if (entries.isEmpty() && !listErr.isEmpty()) {
        result.message = listErr;
        return result;
    }
    for (const QString &entry : entries) {
        if (!isPathWithinBase(destDir, entry)) {
            result.message = isZip
                ? QStringLiteral("Unsafe zip entry: %1").arg(entry)
                : QStringLiteral("Unsafe tar entry: %1").arg(entry);
            return result;
        }
    }

    QString program;
    QStringList args;
    if (isZip) {
        // 对应Python: cmd = f"unzip -o {file} -d {pwd}"
        program = QStringLiteral("unzip");
        args << QStringLiteral("-o") << fi.absoluteFilePath()
             << QStringLiteral("-d") << destDir;
    } else if (isTarGz) {
        // 对应Python: cmd = f"tar -xzvf {file} -C {pwd}"
        program = QStringLiteral("tar");
        args << QStringLiteral("-xzf") << fi.absoluteFilePath()
             << QStringLiteral("-C") << destDir;
    } else {
        // 对应Python: cmd = f"tar -xvf {file} -C {pwd}"
        program = QStringLiteral("tar");
        args << QStringLiteral("-xf") << fi.absoluteFilePath()
             << QStringLiteral("-C") << destDir;
    }

    QString errOut;
    const int code = runProcess(program, args, QString(), &errOut);
    if (code == 0) {
        result.success = true;
        result.message = QStringLiteral("Decompression task finished");
    } else {
        result.message = QStringLiteral("Failed to extract %1: %2").arg(name, errOut);
    }
    return result;
}

// 对应Python: core/compressor.py::DecompressThread.run（files 循环，遇错即停）
ArchiveResult decompressAll(const QStringList &archivePaths, const QString &destDir)
{
    for (const QString &path : archivePaths) {
        const ArchiveResult r = decompress(path, destDir);
        if (!r.success)
            return r;
    }
    ArchiveResult ok;
    ok.success = true;
    ok.message = QStringLiteral("Decompression task finished");
    return ok;
}

// 对应Python: core/compressor.py 中 shlex.quote 用法
QString shellQuote(const QString &arg)
{
    if (arg.isEmpty())
        return QStringLiteral("''");
    // shlex.quote：只含安全字符时原样返回，否则整体单引号包裹，
    // 内部单引号替换为 '"'"'
    static const QString safeChars = QStringLiteral(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@%+=:,./-_");
    bool safe = true;
    for (const QChar &ch : arg) {
        if (!safeChars.contains(ch)) {
            safe = false;
            break;
        }
    }
    if (safe)
        return arg;
    QString quoted = arg;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\"'\"'"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

// 对应Python: function/traversal.py::frpc
QString frpcConfig(const QString &serverAddr, const QString &token,
                   const QString &antType, int localPort, int remotePort)
{
    return QStringLiteral(
               "serverAddr = \"%1\"\n"
               "serverPort = 7000\n"
               "auth.token = \"%2\"\n"
               "\n"
               "%3\n")
        .arg(serverAddr, token, proxyConfig(antType, localPort, remotePort, serverAddr));
}

// 对应Python: function/traversal.py::proxy_config
QString proxyConfig(const QString &antType, int localPort, int remotePort,
                    const QString &serverAddr)
{
    const QString proxyType = antType.toLower();
    if (proxyType == QStringLiteral("http")) {
        // HTTP 类型：用于域名绑定的 HTTP 服务
        return QStringLiteral(
                   "[[proxies]]\n"
                   "name = \"http_proxy\"\n"
                   "type = \"http\"\n"
                   "localIP = \"127.0.0.1\"\n"
                   "localPort = %1\n"
                   "customDomains = [\"%2\"]\n")
            .arg(localPort)
            .arg(serverAddr);
    }
    if (proxyType == QStringLiteral("udp")) {
        // UDP 类型：用于 DNS、游戏服务器等 UDP 协议服务
        return QStringLiteral(
                   "[[proxies]]\n"
                   "name = \"udp_proxy\"\n"
                   "type = \"udp\"\n"
                   "localIP = \"127.0.0.1\"\n"
                   "localPort = %1\n"
                   "remotePort = %2\n")
            .arg(localPort)
            .arg(remotePort);
    }
    // TCP 类型：最通用，支持任何 TCP 协议
    return QStringLiteral(
               "[[proxies]]\n"
               "name = \"tcp_proxy\"\n"
               "type = \"tcp\"\n"
               "localIP = \"127.0.0.1\"\n"
               "localPort = %1\n"
               "remotePort = %2\n")
        .arg(localPort)
        .arg(remotePort);
}

// 对应Python: function/traversal.py::frps
QString frpsConfig(const QString &token, const QString &antType, int httpPort)
{
    QString config = QStringLiteral(
                         "bindPort = 7000\n"
                         "auth.token = \"%1\"\n")
                         .arg(token);
    // HTTP 类型需要配置 vhostHTTPPort
    if (antType.toLower() == QStringLiteral("http") && httpPort > 0)
        config += QStringLiteral("vhostHTTPPort = %1\n").arg(httpPort);
    return config;
}

} // namespace FileUtil
} // namespace cubeshell
