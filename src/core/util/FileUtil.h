#pragma once

// FileUtil.h — 本地文件遍历、压缩/解压、frp 配置模板。
// 对应Python: core/compressor.py（本地压缩/解压分支）+ function/traversal.py
//
// 说明：
// - function/traversal.py 的实际内容是 frp 客户端/服务端配置模板生成
//   （frpc/frps/proxy_config），并非目录遍历；目录遍历逻辑来自
//   core/compressor.py 本地压缩分支的 os.walk，这里用 QDirIterator 实现。
// - 压缩/解压：Python 本地分支用 tarfile/zipfile 标准库，远程分支用
//   tar/zip/unzip 命令。C++ 侧统一用 QProcess 调 tar/zip/unzip 命令
//   （与远程行为一致，不引入 libarchive）。
// - 远程压缩（CompressThread 走 SSH 的分支）依赖 SSH 会话层，
//   不在本工具层实现，由 ssh 模块基于本文件的命令拼装逻辑对接。

#include <QString>
#include <QStringList>

namespace cubeshell {
namespace FileUtil {

// 压缩/解压结果（对应 Python 侧 finished_sig 的 (success, message)）
// 对应Python: core/compressor.py::CompressThread.finished_sig
struct ArchiveResult {
    bool success = false;
    QString message;
};

// 支持的压缩格式（对应 Python 侧 format_type 字符串 ".tar.gz" / ".zip"）
// 对应Python: core/compressor.py::CompressThread.__init__ (format_type)
enum class ArchiveFormat {
    TarGz,
    Zip,
};

// 递归遍历目录，返回相对 baseDir 的文件路径列表（不含目录项，含隐藏文件）。
// 对应Python: core/compressor.py::CompressThread.run 中 os.walk 逐文件收集逻辑
QStringList listFilesRecursive(const QString &baseDir);

// 校验 target 是否落在 base 目录内（zip-slip/tar-slip 防护）。
// 对应Python: core/compressor.py::DecompressThread.run::_ensure_within_base
bool isPathWithinBase(const QString &base, const QString &target);

// 本地压缩：在 baseDir 下把 files（相对路径）打包为 outputName。
// tar.gz 用 `tar -czf`，zip 用 `zip -r`（zip 命令缺失时报错提示安装）。
// 对应Python: core/compressor.py::CompressThread.run（本地分支 + 远程命令拼装）
ArchiveResult compress(const QString &baseDir, const QStringList &files,
                       const QString &outputName, ArchiveFormat format);

// 本地解压：把 archivePath 解压到 destDir。
// .zip 用 `unzip -o -d`，.tar.gz/.tgz 用 `tar -xzf -C`，.tar 用 `tar -xf -C`。
// 解压前先列出条目做目录穿越校验（保持 Python 侧 zip-slip/tar-slip 防护语义）。
// 对应Python: core/compressor.py::DecompressThread.run（本地分支 + 远程命令拼装）
ArchiveResult decompress(const QString &archivePath, const QString &destDir);

// 批量解压（对应 Python 侧 files 列表循环，遇错即停）。
// 对应Python: core/compressor.py::DecompressThread.run
ArchiveResult decompressAll(const QStringList &archivePaths, const QString &destDir);

// POSIX shell 单参数转义（等价于 shlex.quote）。
// 对应Python: core/compressor.py 中 shlex.quote 用法
QString shellQuote(const QString &arg);

// frp 客户端配置文件内容
// 对应Python: function/traversal.py::frpc
QString frpcConfig(const QString &serverAddr, const QString &token,
                   const QString &antType, int localPort, int remotePort);

// frp 代理段配置（TCP/HTTP/UDP）
// 对应Python: function/traversal.py::proxy_config
QString proxyConfig(const QString &antType, int localPort, int remotePort,
                    const QString &serverAddr = QString());

// frp 服务端配置文件内容
// 对应Python: function/traversal.py::frps
QString frpsConfig(const QString &token, const QString &antType = QStringLiteral("tcp"),
                   int httpPort = 0);

} // namespace FileUtil
} // namespace cubeshell
