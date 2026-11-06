#pragma once

// ClaudeSessionStore.h — 已保存会话的本地 / SSH 双后端存储访问。
//
// 直接扫描 projects 下的 JSONL，不依赖 Claude CLI 是否安装或正在运行。
// 列表只采样日志首尾并缓存摘要；详情按字节分页；导出单独读取完整日志。
// 阻塞读取由 ClaudeCodeBackend 调度到工作线程，界面不直接执行文件或 SSH 操作。

#include "claude_code/ClaudeSessionData.h"
#include <QMutex>
#include <atomic>
#include <functional>
#include <memory>

namespace cubeshell {
class SshClient;

// 一次范围读取的结果；分页单位是字节，不保证落在 JSONL 行边界。
struct ClaudeSessionPage {
    QByteArray bytes;
    qint64 nextOffset = 0; // 下一页从此处继续，不按解析条目数计算
    qint64 totalBytes = 0; // 列表枚举时的文件大小，不追踪读取期间新增的字节
    bool endOfFile = false;
    QString error;
};

// 每个实例绑定不可变的数据源；切换模式时另建实例，旧任务通过共享所有权安全收尾。
// cancel() 只取消本存储的读取，不中断其他功能正在使用的 SSH 命令。
class ClaudeSessionStore {
public:
    // 可注入命令函数供测试模拟远程读取；实际运行使用持有 SSH 客户端的构造方式。
    using RemoteCommand = std::function<QByteArray(const QString &, QString *)>;
    explicit ClaudeSessionStore(const QString &claudeHome);
    explicit ClaudeSessionStore(RemoteCommand command);
    explicit ClaudeSessionStore(std::shared_ptr<SshClient> client);
    // 按文件最近修改时间降序返回摘要；error 必须非空，可同时返回列表与部分读取失败提示。
    QList<ClaudeSessionInfo> list(QString *error);
    // 读取枚举时文件大小范围内的一页；文件缩短或字节不足时要求调用方刷新。
    ClaudeSessionPage read(const ClaudeSessionInfo &session, qint64 offset);
    // 导出完整文件快照：JSONL 原字节保留，Markdown 通过解析器转换；失败不提交目标文件。
    bool exportSession(const ClaudeSessionInfo &session, const QString &destination,
                       bool markdown, QString *error);
    // 取消后不再复用此实例；已有任务在检查点退出，远程命令使用本实例的取消标记。
    void cancel() { m_cancelled.store(true); }
    bool isRemote() const { return m_remote; }
    static constexpr qint64 chunkSize = 512 * 1024;

private:
    // 仅采样缓存失效的远程文件，返回每个文件的首段 / 尾段字节。
    QHash<QString, QPair<QByteArray, QByteArray>> remoteSummaries(
        const QList<ClaudeSessionInfo> &sessions, QString *error);
    QList<ClaudeSessionInfo> manifest(QString *error);
    QByteArray readRange(const ClaudeSessionInfo &session, qint64 offset, qint64 size, QString *error);
    // 校验相对路径及真实路径，拒绝越出 projects 或经过符号链接的文件。
    QString localPath(const QString &relativePath) const;
    QString remoteFile(const QString &relativePath) const;
    QByteArray remoteCommand(const QString &command, QString *error);
    bool stopped(QString *error) const;
    RemoteCommand m_command;
    QString m_home;
    bool m_remote = false;
    std::shared_ptr<SshClient> m_client; // 保证在途命令执行期间客户端对象仍然存活
    std::atomic<bool> m_cancelled{false};
    QMutex m_mutex; // 串行化同一存储实例的列表 / 分页读取及摘要缓存访问
    QHash<QString, ClaudeSessionInfo> m_cache; // 相对路径 → 摘要；文件大小或修改时间变化即失效
};
} // namespace cubeshell
Q_DECLARE_METATYPE(cubeshell::ClaudeSessionPage)
