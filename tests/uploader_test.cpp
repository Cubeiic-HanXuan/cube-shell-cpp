// uploader_test.cpp — SftpUploaderCore 测试。
//
// 分两段：
//  1) 纯逻辑（不依赖网络）：分片切分计算、断点续传元数据读写往返 +
//     Python 格式断言（字段名/类型；若系统有 python3，直接用 CPython
//     json + os.path.getmtime 交叉验证互认）、取消标志。
//  2) 真实 SFTP 上传：依赖 127.0.0.1:2222 的测试 sshd；无服务时 exit 2
//     记 SKIP（与 sftp_integration_test.cpp 一致）。
// Env overrides: CUBESSH_HOST, CUBESSH_PORT, CUBESSH_USER, CUBESSH_PASS,
//                CUBESSH_REMOTE_BASE（远端可写目录，默认 /home/testuser/sftp_dir）。

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include "ssh/SftpClient.h"
#include "ssh/SftpUploaderCore.h"
#include "ssh/SshClient.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// 处理事件循环若干毫秒（让 QueuedConnection 的信号真正投递到本线程）。
static void spin(int msecs)
{
    QEventLoop loop;
    QTimer::singleShot(msecs, &loop, &QEventLoop::quit);
    loop.exec();
}

// 生成指定大小的本地测试文件（内容可校验：按偏移填充可预测字节）。
static bool writeTestFile(const QString &path, qint64 size)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const int blockSize = 64 * 1024;
    QByteArray block(blockSize, Qt::Uninitialized);
    for (int i = 0; i < blockSize; ++i)
        block[i] = char('A' + (i % 26));
    qint64 written = 0;
    while (written < size) {
        const qint64 n = qMin<qint64>(blockSize, size - written);
        if (f.write(block.constData(), n) != n)
            return false;
        written += n;
    }
    f.close();
    return true;
}

// --------------------------------------------------------------------------
// 1) 纯逻辑测试
// --------------------------------------------------------------------------

static void testPlanChunks()
{
    const qint64 chunk = SftpUploaderCore::kChunkSize;
    CHECK(chunk == 4 * 1024 * 1024); // 对应Python: CHUNK_SIZE = 4 * 1024 * 1024

    // 空文件：无分片（Python 的 while offset < total_size 同样不进循环）。
    CHECK(SftpUploaderCore::planChunks(0).isEmpty());

    // 小于一个分片。
    auto c = SftpUploaderCore::planChunks(100);
    CHECK(c.size() == 1);
    CHECK(c[0].first == 0 && c[0].second == 100);

    // 恰好整数个分片。
    c = SftpUploaderCore::planChunks(chunk * 2);
    CHECK(c.size() == 2);
    CHECK(c[0].first == 0 && c[0].second == chunk);
    CHECK(c[1].first == chunk && c[1].second == chunk);

    // 非整数倍：10MB -> 4MB + 4MB + 2MB。
    c = SftpUploaderCore::planChunks(10 * 1024 * 1024);
    CHECK(c.size() == 3);
    CHECK(c[2].second == 2 * 1024 * 1024);

    // 断点续传：从 startOffset 起继续切。
    c = SftpUploaderCore::planChunks(10 * 1024 * 1024, chunk);
    CHECK(c.size() == 2);
    CHECK(c[0].first == chunk);
    CHECK(c[1].second == 2 * 1024 * 1024);

    // 已传完 / 越界：无分片。
    CHECK(SftpUploaderCore::planChunks(chunk, chunk).isEmpty());
    CHECK(SftpUploaderCore::planChunks(chunk, chunk * 2).isEmpty());

    // 负 offset 视为 0（防御性，与 Python 的 offset=metadata 值语义无冲突）。
    CHECK(SftpUploaderCore::planChunks(10, -5).size() == 1);
}

// 元数据往返 + Python 侧格式断言。
// 对应Python: sftp_uploader_core.py::_save_metadata / _load_metadata
static void testMetadataRoundTrip(const QString &tmpDir)
{
    const QString localPath = QDir(tmpDir).filePath(QStringLiteral("meta_src.bin"));
    CHECK(writeTestFile(localPath, 1024));

    SftpUploaderCore uploader;
    uploader.setMetadataDir(QDir(tmpDir).filePath(QStringLiteral("meta")));
    const QString fileId = QStringLiteral("test_file_id_001");
    const QString remotePath = QStringLiteral("/home/testuser/sftp_dir/meta_src.bin");

    // 保存 -> 文件名与 Python 一致：{metadata_dir}/{file_id}.json
    CHECK(uploader.saveMetadata(fileId, localPath, remotePath, 4 * 1024 * 1024));
    const QString metaPath = uploader.metadataPath(fileId);
    CHECK(metaPath.endsWith(fileId + QStringLiteral(".json")));
    CHECK(QFile::exists(metaPath));

    // 读回 -> 6 个字段齐全、值一致。
    QJsonObject meta;
    CHECK(uploader.loadMetadata(fileId, meta));
    CHECK(meta.value(QStringLiteral("file_id")).toString() == fileId);
    CHECK(meta.value(QStringLiteral("local_path")).toString() == localPath);
    CHECK(meta.value(QStringLiteral("remote_path")).toString() == remotePath);
    CHECK(qint64(meta.value(QStringLiteral("uploaded_size")).toDouble()) == 4 * 1024 * 1024);
    CHECK(meta.contains(QStringLiteral("last_modified")));
    CHECK(meta.contains(QStringLiteral("timestamp")));
    CHECK(meta.size() == 6); // 不多不少，与 Python dump 的字段集一致

    // last_modified 必须等于 CPython os.path.getmtime 的算法结果。
    const double mtime = SftpUploaderCore::pythonMtime(localPath);
    CHECK(mtime > 0.0);
    CHECK(meta.value(QStringLiteral("last_modified")).toDouble() == mtime);
    // timestamp 是秒级 epoch 浮点（对应Python: time.time()）。
    CHECK(meta.value(QStringLiteral("timestamp")).toDouble() > 1.5e9);

    // 本地文件被修改后元数据作废（Python 用精确 != 比较 mtime）。
    spin(50);
    {
        QFile f(localPath);
        CHECK(f.open(QIODevice::Append));
        f.write("x");
        f.close();
    }
    QJsonObject stale;
    const double newMtime = SftpUploaderCore::pythonMtime(localPath);
    if (newMtime != mtime) { // 文件系统时间戳精度足够时才有意义
        CHECK(!uploader.loadMetadata(fileId, stale));
    }

    // 本地文件不存在 -> 元数据无效（对应Python: if not os.path.exists(local_path)）。
    CHECK(uploader.saveMetadata(fileId, localPath, remotePath, 123));
    CHECK(QFile::remove(localPath));
    CHECK(!uploader.loadMetadata(fileId, stale));

    // 删除元数据。对应Python: _delete_metadata
    uploader.deleteMetadata(fileId);
    CHECK(!QFile::exists(metaPath));

    // 损坏的 JSON -> 视为无效（对应Python: except -> return None）。
    CHECK(writeTestFile(localPath, 16));
    {
        QFile f(metaPath);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write("{not json");
        f.close();
    }
    CHECK(!uploader.loadMetadata(fileId, stale));
    QFile::remove(metaPath);
}

// 用 CPython 交叉验证元数据互认：字段名/类型 + os.path.getmtime 精确相等。
// 无 python3 时跳过（不计失败）。
static void testPythonInterop(const QString &tmpDir)
{
    const QString py = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (py.isEmpty()) {
        qInfo() << "python3 not found — skip metadata interop cross-check";
        return;
    }

    const QString localPath = QDir(tmpDir).filePath(QStringLiteral("interop_src.bin"));
    CHECK(writeTestFile(localPath, 2048));

    SftpUploaderCore uploader;
    uploader.setMetadataDir(QDir(tmpDir).filePath(QStringLiteral("meta_interop")));
    const QString fileId = QStringLiteral("interop_id");
    CHECK(uploader.saveMetadata(fileId, localPath, QStringLiteral("/tmp/remote.bin"), 8192));

    // Python 读 C++ 写的元数据：字段集/类型/mtime 精确相等。
    const QString script = QStringLiteral(
        "import json,os,sys\n"
        "m=json.load(open(sys.argv[1]))\n"
        "assert set(m)=={'file_id','local_path','remote_path','uploaded_size',"
        "'last_modified','timestamp'}, m\n"
        "assert isinstance(m['file_id'],str) and isinstance(m['local_path'],str)\n"
        "assert isinstance(m['remote_path'],str)\n"
        "assert int(m['uploaded_size'])==8192\n"
        "assert isinstance(m['last_modified'],float) or isinstance(m['last_modified'],int)\n"
        "assert m['last_modified']==os.path.getmtime(m['local_path']), "
        "(m['last_modified'],os.path.getmtime(m['local_path']))\n"
        "print('PY_OK')\n");

    QProcess p;
    p.start(py, { QStringLiteral("-c"), script, uploader.metadataPath(fileId) });
    if (!p.waitForFinished(15000)) {
        qWarning() << "python3 interop check timed out";
        ++failures;
        return;
    }
    const QByteArray out = p.readAllStandardOutput();
    const QByteArray errOut = p.readAllStandardError();
    if (!out.contains("PY_OK"))
        qWarning() << "python interop stderr:" << errOut;
    CHECK(out.contains("PY_OK"));

    // 反向：Python 写元数据 -> C++ loadMetadata 认（含 mtime 精确相等）。
    const QString script2 = QStringLiteral(
        "import json,os,sys,time\n"
        "local=sys.argv[2]\n"
        "m={'file_id':'py_id','local_path':local,'remote_path':'/tmp/remote.bin',\n"
        "   'uploaded_size':4194304,'last_modified':os.path.getmtime(local),\n"
        "   'timestamp':time.time()}\n"
        "json.dump(m,open(sys.argv[1],'w'))\n"
        "print('PY_WROTE')\n");
    QProcess p2;
    p2.start(py, { QStringLiteral("-c"), script2,
                   uploader.metadataPath(QStringLiteral("py_id")), localPath });
    CHECK(p2.waitForFinished(15000));
    CHECK(p2.readAllStandardOutput().contains("PY_WROTE"));

    QJsonObject fromPy;
    CHECK(uploader.loadMetadata(QStringLiteral("py_id"), fromPy));
    CHECK(fromPy.value(QStringLiteral("file_id")).toString() == QStringLiteral("py_id"));
    CHECK(qint64(fromPy.value(QStringLiteral("uploaded_size")).toDouble()) == 4194304);
}

// 取消标志（不依赖网络：未设置 SshClient 时任务会立刻失败退出）。
// 对应Python: cancel_upload / stop_events
//
// 这里同时守住一条不变量：**每个上传任务都必须以恰好一个终态信号收场**
// （uploadCompleted 或 uploadFailed）。取消路径曾经静默 break 掉、什么都不发，
// 于是 SftpBrowserWidget 的 m_activeUploads / 进度条 / 取消按钮永远不复位，
// 表现为"点了取消没反应"。
static void testCancelFlag(const QString &tmpDir)
{
    const QString localPath = QDir(tmpDir).filePath(QStringLiteral("cancel_src.bin"));
    CHECK(writeTestFile(localPath, 1024));

    SftpUploaderCore probe; // 无 SshClient
    probe.setMetadataDir(QDir(tmpDir).filePath(QStringLiteral("meta_cancel")));
    // 未知 fileId -> 未请求取消。
    CHECK(!probe.isCancelRequested(QStringLiteral("nope")));

    // 多跑几轮：取消是在"任务入队后、工作线程读取消标志前"这个窗口里发出的，
    // 谁先到没有保证。轮数多了两种落点都会被覆盖到，而不变量对两种都成立。
    int cancelledRounds = 0;
    for (int round = 0; round < 8; ++round) {
        SftpUploaderCore uploader; // 无 SshClient
        uploader.setMetadataDir(QDir(tmpDir).filePath(QStringLiteral("meta_cancel")));

        int completedCount = 0;
        int failedCount = 0;
        QString failMsg;
        QObject::connect(&uploader, &SftpUploaderCore::uploadCompleted,
                         [&completedCount](const QString &, const QString &) {
                             ++completedCount;
                         });
        QObject::connect(&uploader, &SftpUploaderCore::uploadFailed,
                         [&failedCount, &failMsg](const QString &, const QString &,
                                                  const QString &e) {
                             ++failedCount;
                             failMsg = e;
                         });

        const QString fileId = QStringLiteral("cancel_id");
        uploader.uploadFile(fileId, localPath, QStringLiteral("/tmp/cancel_dst.bin"));
        uploader.cancelUpload(fileId); // 立即取消（可能在任务开始前/后）
        CHECK(uploader.waitForFinished(10000));
        spin(200); // 让 QueuedConnection 的信号落地

        // 取消或"未设置客户端"都不应产生 completed。
        CHECK(completedCount == 0);
        // 核心不变量：必须收到恰好一个终态信号。取消时一声不响地退出，
        // 就是这个 bug 的根（上层据此收尾在传记账与取消按钮）。
        CHECK(failedCount == 1);
        // 任务结束后取消标志已被清理。
        CHECK(!uploader.isCancelRequested(fileId));

        // 取消先到时，消息必须带"已取消"——上层就是靠这个子串把用户主动取消
        // 与真正的传输失败区分开（见 SftpBrowserWidget 的 uploadFailed 接线），
        // 措辞改了会让取消被报成"上传失败"。
        if (failMsg.contains(QStringLiteral("取消"))) {
            ++cancelledRounds;
            CHECK(failMsg.contains(QStringLiteral("已取消")));
        }
    }
    qInfo() << "cancel path: cancel-won rounds =" << cancelledRounds << "/ 8";
}

// --------------------------------------------------------------------------
// 2) 真实 SFTP 上传（需要 127.0.0.1:2222）
// --------------------------------------------------------------------------

static int testRealUpload(const QString &tmpDir)
{
    const QString host = qEnvironmentVariable("CUBESSH_HOST", "127.0.0.1");
    const quint16 port = quint16(qEnvironmentVariable("CUBESSH_PORT", "2222").toUShort());
    const QString user = qEnvironmentVariable("CUBESSH_USER", "testuser");
    const QString pass = qEnvironmentVariable("CUBESSH_PASS", "testpass123");

    SshClient client;
    client.setHost(host, port);
    client.setUsername(user);
    client.setPassword(pass);

    SshError err;
    if (!client.connectToHost(nullptr, err)) {
        qWarning() << "connect failed (upload tests SKIPPED):" << err.message;
        return 2; // SKIP
    }
    qInfo() << "connected — running real SFTP upload tests";

    const QString base = qEnvironmentVariable("CUBESSH_REMOTE_BASE",
                                              "/home/testuser/sftp_dir");
    const qint64 totalSize = 10 * 1024 * 1024; // 3 个分片：4MB + 4MB + 2MB
    const QString localPath = QDir(tmpDir).filePath(QStringLiteral("upload_10m.bin"));
    CHECK(writeTestFile(localPath, totalSize));
    const QString remotePath = base + QStringLiteral("/upload_10m.bin");

    SftpUploaderCore uploader(&client);
    uploader.setMetadataDir(QDir(tmpDir).filePath(QStringLiteral("meta_upload")));

    qint64 lastDone = 0;
    qint64 reportedTotal = -1;
    int progressCount = 0;
    bool started = false;
    bool completed = false;
    QString failure;
    QObject::connect(&uploader, &SftpUploaderCore::uploadStarted,
                     [&](const QString &, const QString &, qint64 size) {
                         started = true;
                         reportedTotal = size;
                     });
    QObject::connect(&uploader, &SftpUploaderCore::progressChanged,
                     [&](const QString &, qint64 done, qint64 total) {
                         CHECK(done > lastDone); // 单调递增
                         lastDone = done;
                         CHECK(total == totalSize);
                         ++progressCount;
                     });
    QObject::connect(&uploader, &SftpUploaderCore::uploadCompleted,
                     [&](const QString &, const QString &) { completed = true; });
    QObject::connect(&uploader, &SftpUploaderCore::uploadFailed,
                     [&](const QString &, const QString &, const QString &e) { failure = e; });

    const QString fileId = QStringLiteral("upload_10m");
    uploader.uploadFile(fileId, localPath, remotePath);

    // 等任务结束 + 事件循环把信号投递完。
    CHECK(uploader.waitForFinished(180000));
    spin(500);

    if (!failure.isEmpty())
        qWarning() << "upload failed:" << failure;
    CHECK(failure.isEmpty());
    CHECK(started);
    CHECK(reportedTotal == totalSize);
    CHECK(completed);
    CHECK(progressCount == 3); // 每分片一次进度
    CHECK(lastDone == totalSize);

    // 远端大小校验（只调用 SftpClient，不修改它）。
    SftpClient sftp(&client);
    if (sftp.open(err)) {
        SftpFileInfo info;
        CHECK(sftp.stat(remotePath, info, err));
        CHECK(info.size == totalSize);
    } else {
        qWarning() << "sftp open for verification failed:" << err.message;
        ++failures;
    }

    // 完成后元数据应被删除。对应Python: _delete_metadata
    CHECK(!QFile::exists(uploader.metadataPath(fileId)));

    // 断点续传：伪造"已传 4MB"的元数据，重传应只跑后两个分片且结果完整。
    lastDone = 0;
    progressCount = 0;
    completed = false;
    failure.clear();
    CHECK(uploader.saveMetadata(fileId, localPath, remotePath, SftpUploaderCore::kChunkSize));
    uploader.uploadFile(fileId, localPath, remotePath);
    CHECK(uploader.waitForFinished(180000));
    spin(500);
    CHECK(failure.isEmpty());
    CHECK(completed);
    CHECK(progressCount == 2); // 只剩 4MB + 2MB 两片
    CHECK(lastDone == totalSize);
    if (sftp.isOpen()) {
        SftpFileInfo info;
        CHECK(sftp.stat(remotePath, info, err));
        CHECK(info.size == totalSize);
    }

    // 清理远端文件。
    if (sftp.isOpen())
        sftp.remove(remotePath, &err);

    return 0;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        qWarning() << "cannot create temp dir";
        return 1;
    }

    // --- 纯逻辑（永远执行） ---
    testPlanChunks();
    testMetadataRoundTrip(tmp.path());
    testPythonInterop(tmp.path());
    testCancelFlag(tmp.path());

    if (failures != 0) {
        qWarning() << "UPLOADER LOGIC FAILURES" << failures;
        return 1;
    }
    qInfo() << "UPLOADER LOGIC PASS";

    // --- 真实上传（无 sshd 则 SKIP） ---
    const int rc = testRealUpload(tmp.path());
    if (rc == 2)
        return 2; // SKIP：纯逻辑已全部通过
    if (rc != 0)
        return rc;

    qInfo() << (failures == 0 ? "UPLOADER ALL PASS" : "UPLOADER FAILURES") << failures;
    return failures == 0 ? 0 : 1;
}
