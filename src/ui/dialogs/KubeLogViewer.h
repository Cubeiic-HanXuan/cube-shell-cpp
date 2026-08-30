#pragma once

// KubeLogViewer.h — Kubernetes 日志 / 一次性文本查看窗口（非模态）。
//
// 对应 docs/Kubernetes功能实现方案.md §5.4：
//   - Pod 日志流：beginStream → appendChunk* → finishStream；
//   - describe / rollout status 一次性内容：showText。
// 复用只读 QPlainTextEdit，不上 QTermWidget（日志不需要 pty，可暂停、关窗即停）。
// 流模式下关闭窗口发 stopRequested，由 KubeManagerDialog 转发 KubeManager::stopPodLogs。

#include <QDialog>
#include <QString>

class QPlainTextEdit;
class QLabel;

namespace cubeshell {

class KubeLogViewer : public QDialog {
    Q_OBJECT
public:
    explicit KubeLogViewer(QWidget *parent = nullptr);

    // 一次性内容（describe / rollout status）。
    void showText(const QString &title, const QString &text);

    // 流模式三件套。
    void beginStream(const QString &title);
    void appendChunk(const QString &text);
    void finishStream(int exitCode);

    bool isStreaming() const { return m_streaming; }

signals:
    // 流模式下用户关闭窗口 / 点"停止"。
    void stopRequested();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    QPlainTextEdit *m_view = nullptr;
    QLabel *m_statusLabel = nullptr;
    bool m_streaming = false;
};

} // namespace cubeshell
