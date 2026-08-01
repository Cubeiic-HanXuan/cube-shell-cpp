#pragma once

// ServerProfileBuilder.h — Async server profile probe (system context injection).
//
// 对应Python: core/ai/server_profile.py::ServerProfile + ServerProfileBuilder
//
// 异步通过 CommandExecutor 执行一组探测命令，构建服务器画像字符串
// 注入 AI 系统提示词，使 LLM 了解目标服务器的运行环境。
// 结果缓存 5 分钟，过期后下次触发自动刷新。
//
// 线程模型：CommandExecutor::exec() 为阻塞调用，因此探测在 QtConcurrent
// 线程池中执行；结果经 QMetaObject::invokeMethod 排队回本对象所属线程，
// 成员变量与 profileReady 的发射均发生在该线程，无需加锁。

#include <QElapsedTimer>
#include <QFuture>
#include <QObject>
#include <QString>

namespace cubeshell {

class CommandExecutor;

class ServerProfileBuilder : public QObject {
    Q_OBJECT
public:
    // 缓存有效期 5 分钟
    // 对应Python: ServerProfileBuilder.TTL_SECONDS = 300
    static constexpr qint64 kProfileTtlMs = 300000;

    // 单次探测的超时（复合命令，含 systemctl / ss 等较慢的子命令）
    static constexpr int kProbeTimeoutMs = 15000;

    explicit ServerProfileBuilder(CommandExecutor *executor,
                                   QObject *parent = nullptr);
    // 等待在途探测结束，避免线程池回调触到已析构对象。
    ~ServerProfileBuilder() override;

    // 异步执行探测命令，完成后发射 profileReady。
    // 缓存未过期时复用已有结果，不再访问远端。
    // profileReady 永远异步送达（不会在本调用栈内回调），因此槽函数中
    // 再次调用 buildAsync 不会递归爆栈。
    // 对应Python: ServerProfile.build_async
    void buildAsync();

    // 返回构建好的 profile（空表示尚未完成或探测失败）
    QString profile() const { return m_profile; }

    // 是否已构建且未过期
    bool isReady() const;

    // 是否正在构建中
    bool isBuilding() const { return m_building; }

signals:
    // 对应Python: profile_ready
    void profileReady(const QString &profile);

private:
    void onProbeFinished(const QString &result);
    // 把 profileReady 推迟到下一次事件循环，保证发射永不发生在
    // buildAsync 的调用栈内（避免槽内重入 buildAsync 造成递归）。
    void emitProfileDeferred();

    CommandExecutor *m_executor;
    QString m_profile;
    QElapsedTimer m_buildTime;
    QFuture<void> m_probe;
    bool m_building = false;
    bool m_hasBuilt = false;
};

} // namespace cubeshell
