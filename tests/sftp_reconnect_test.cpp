// SFTP 面板换 SSH 连接的回归：旧组件/旧回调不能跨连接复用。
// 默认离屏、无网络；--integration 使用 CUBESSH_HOST/PORT/USER/PASS 的测试 sshd。

#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QPointer>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTreeWidget>
#include <QUuid>

#include <functional>

#include "sftp_browser_widget.h"
#include "ssh/SftpUploaderCore.h"
#include "ssh/SshClient.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

static bool waitUntil(const std::function<bool()> &done, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return done();
}

static QString statusText(const SftpBrowserWidget &browser)
{
    // 状态栏将完整文本放在 tooltip；不依赖布局宽度/省略号。
    QStringList text;
    for (const auto *label : browser.findChildren<QLabel *>())
        text.append(label->toolTip());
    return text.join(QLatin1Char('\n'));
}

static QTreeWidgetItem *findEntry(SftpBrowserWidget &browser, const QString &name)
{
    auto *tree = browser.findChild<QTreeWidget *>();
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        auto *item = tree->topLevelItem(i);
        if (item->text(0) == name)
            return item;
    }
    return nullptr;
}

static void testReplaceClient()
{
    SftpBrowserWidget browser;
    browser.setCurrentPath(QStringLiteral("/home/before-reconnect"));
    auto first = std::make_shared<SshClient>();
    std::weak_ptr<SshClient> oldClient = first;
    browser.setClient(first);
    QPointer<SftpClient> oldSftp = browser.findChild<SftpClient *>();
    QPointer<SftpUploaderCore> oldUploader = browser.findChild<SftpUploaderCore *>();
    CHECK(oldSftp && oldUploader);

    // 同一连接的重复通知应幂等，不中断正在进行的操作。
    browser.setClient(first);
    CHECK(oldSftp == browser.findChild<SftpClient *>());
    CHECK(oldUploader == browser.findChild<SftpUploaderCore *>());
    first.reset(); // 面板是唯一的 SSH 所有者，验证释放次序不靠 terminal/monitor 兜底。

    browser.setClient(std::make_shared<SshClient>());
    CHECK(oldSftp.isNull());
    CHECK(oldUploader.isNull());
    CHECK(oldClient.expired());
    CHECK(browser.findChildren<SftpClient *>().size() == 1);
    CHECK(browser.findChildren<SftpUploaderCore *>().size() == 1);
    CHECK(browser.currentPath() == QStringLiteral("/home/before-reconnect"));
    CHECK(browser.isEnabled());

    // 多次重连，包括目录加载尚在后台时切换。
    for (int i = 0; i < 5; ++i) {
        QPointer<SftpClient> previous = browser.findChild<SftpClient *>();
        browser.setClient(std::make_shared<SshClient>());
        CHECK(previous.isNull());
    }
}

static void testStaleCallbacks()
{
    SftpBrowserWidget browser;
    browser.setClient(std::make_shared<SshClient>());
    auto *sftp = browser.findChild<SftpClient *>();
    auto *uploader = browser.findChild<SftpUploaderCore *>();

    // 让旧目录加载与传输信号入队，但不处理 UI 事件。
    auto *emitter = QThread::create([sftp]() {
        emit sftp->operationFailed(QStringLiteral("OLD"), {}, QStringLiteral("stale error"));
        emit sftp->transferFinished(QStringLiteral("/old"), false, QStringLiteral("stale transfer"));
    });
    emitter->start();
    CHECK(emitter->wait(1000));
    delete emitter;
    emit uploader->uploadFailed(QStringLiteral("old"), QStringLiteral("old"), QStringLiteral("stale upload"));
    emit uploader->uploadCompleted(QStringLiteral("old"), QStringLiteral("old"));
    emit uploader->progressChanged(QStringLiteral("old"), 1, 10);

    browser.setClient(nullptr);
    CHECK(!browser.isEnabled());
    CHECK(!browser.findChild<SftpClient *>());
    CHECK(!browser.findChild<SftpUploaderCore *>());
    const QString disconnectedStatus = statusText(browser);
    CHECK(disconnectedStatus.contains(QStringLiteral("连接已断开")));
    QCoreApplication::processEvents();
    CHECK(statusText(browser) == disconnectedStatus);

    browser.setClient(std::make_shared<SshClient>());
    CHECK(browser.isEnabled());
    CHECK(browser.findChild<SftpClient *>());
}

static void testTeardownStopsNewOperations()
{
    SshClient client;
    SftpClient sftp(&client);
    sftp.beginTeardown();
    SshError error;
    CHECK(!sftp.open(error));
    CHECK(error.message.contains(QStringLiteral("closing")));
    error = {};
    CHECK(sftp.listdirAttr(QStringLiteral("/"), error).isEmpty());
    CHECK(error.message.contains(QStringLiteral("closing")));
}

static std::shared_ptr<SshClient> connectClient()
{
    auto client = std::make_shared<SshClient>();
    client->setHost(qEnvironmentVariable("CUBESSH_HOST", "127.0.0.1"),
                    quint16(qEnvironmentVariable("CUBESSH_PORT", "2222").toUShort()));
    client->setUsername(qEnvironmentVariable("CUBESSH_USER", "testuser"));
    client->setPassword(qEnvironmentVariable("CUBESSH_PASS", "testpass123"));
    client->setConnectTimeoutMs(3000);
    SshError error;
    if (!client->connectToHost(nullptr, error)) {
        qWarning() << "SSH test connection failed:" << error.message;
        return {};
    }
    return client;
}

static int testLiveReconnect()
{
    auto first = connectClient();
    if (!first)
        return 2;
    const QString base = QStringLiteral("/tmp/cubeshell-reconnect-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QByteArray payload = QByteArrayLiteral("reconnected SFTP\n").repeated(8192);
    SshError error;
    {
        SftpClient setup(first.get());
        CHECK(setup.mkdir(base, 0700, &error));
        CHECK(setup.mkdir(base + QStringLiteral("/child"), 0700, &error));
        CHECK(setup.writeFile(base + QStringLiteral("/child/hello.txt"), payload, error));
    }
    // 走终端实际使用的非阻塞 session 模式，而不只是纯 SFTP 阻塞连接。
    CHECK(first->openShell("xterm-256color", 80, 24, error));
    SftpBrowserWidget browser;
    browser.setCurrentPath(base);
    browser.setClient(first);
    CHECK(waitUntil([&]() { return findEntry(browser, QStringLiteral("child")); }));
    QPointer<SftpClient> oldSftp = browser.findChild<SftpClient *>();
    QPointer<SftpUploaderCore> oldUploader = browser.findChild<SftpUploaderCore *>();

    // 旧目录请求在途时断线，终端另建 SSH 后再次调用同一个面板的 setClient。
    CHECK(QMetaObject::invokeMethod(&browser, "refresh", Qt::DirectConnection));
    first->shutdownSocket();
    auto second = connectClient();
    CHECK(bool(second));
    if (!second)
        return 1;
    CHECK(second->openShell("xterm-256color", 80, 24, error));
    browser.setClient(second);
    first.reset();
    CHECK(oldSftp.isNull());
    CHECK(oldUploader.isNull());
    CHECK(waitUntil([&]() { return statusText(browser).contains(QStringLiteral("项")); }));
    CHECK(browser.currentPath() == base);

    auto *child = findEntry(browser, QStringLiteral("child"));
    CHECK(child);
    if (child) {
        CHECK(QMetaObject::invokeMethod(&browser, "onItemDoubleClicked", Qt::DirectConnection,
                                       Q_ARG(QTreeWidgetItem *, child), Q_ARG(int, 0)));
        CHECK(waitUntil([&]() { return browser.currentPath() == base + QStringLiteral("/child"); }));
        CHECK(findEntry(browser, QStringLiteral("hello.txt")));
    }

    QTemporaryDir local;
    CHECK(local.isValid());
    auto *sftp = browser.findChild<SftpClient *>();
    bool downloadDone = false, downloadOk = false;
    QObject::connect(sftp, &SftpClient::transferFinished, &browser,
                     [&](const QString &, bool ok, const QString &) {
        downloadDone = true;
        downloadOk = ok;
    });
    const QString downloaded = local.filePath(QStringLiteral("download.txt"));
    sftp->download(base + QStringLiteral("/child/hello.txt"), downloaded);
    CHECK(waitUntil([&]() { return downloadDone; }));
    CHECK(downloadOk);
    QFile file(downloaded);
    CHECK(file.open(QIODevice::ReadOnly));
    CHECK(file.readAll() == payload);
    file.close();

    auto *uploader = browser.findChild<SftpUploaderCore *>();
    uploader->setMetadataDir(local.filePath(QStringLiteral("metadata")));
    bool uploadDone = false, uploadOk = false;
    QObject::connect(uploader, &SftpUploaderCore::uploadCompleted, &browser,
                     [&](const QString &, const QString &) { uploadDone = uploadOk = true; });
    QObject::connect(uploader, &SftpUploaderCore::uploadFailed, &browser,
                     [&](const QString &, const QString &, const QString &) { uploadDone = true; });
    const QString uploaded = base + QStringLiteral("/child/uploaded.txt");
    uploader->uploadFile(QStringLiteral("reconnect-test"), downloaded, uploaded);
    CHECK(waitUntil([&]() { return uploadDone; }));
    CHECK(uploadOk);
    CHECK(sftp->readFile(uploaded, error) == payload);
    CHECK(waitUntil([&]() { return findEntry(browser, QStringLiteral("uploaded.txt")); }));
    CHECK(sftp->removeRecursive(base, &error)); // 仅删除本测试创建的唯一临时目录
    browser.setClient(nullptr);
    qInfo() << "live reconnect:" << failures << "failure(s)";
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    // 上传元数据不碰真实的 ~/.sftp_uploader。
    QTemporaryDir home;
    qputenv("HOME", home.path().toUtf8());
    QApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--integration")))
        return testLiveReconnect();
    testReplaceClient();
    testStaleCallbacks();
    testTeardownStopsNewOperations();
    qInfo() << "sftp_reconnect_test:" << failures << "failure(s)";
    return failures ? 1 : 0;
}
