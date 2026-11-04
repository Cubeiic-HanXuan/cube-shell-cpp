#pragma once

// NginxLogViewer.h — Nginx 错误/访问日志查看窗口（非模态）。
//
// 对应 docs/Nginx可视化管理实现方案.md §5.3：复用 KubeLogViewer 的流式三件套。
// 一次性 tail 用 showText；跟随模式 beginStream → appendChunk* → finishStream，
// 关闭窗口发 stopRequested，由 NginxManagerDialog 转发 NginxManager::stopLogStream。

#include <QDialog>
#include <QString>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace cubeshell {

class NginxLogViewer : public QDialog {
    Q_OBJECT
public:
    explicit NginxLogViewer(QWidget *parent = nullptr);

    void showText(const QString &title, const QString &text);

    void beginStream(const QString &title);
    void appendChunk(const QString &text);
    void finishStream(int exitCode);

    bool isStreaming() const { return m_streaming; }

signals:
    void stopRequested();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void updateButtons();

    QPlainTextEdit *m_view = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_stopBtn = nullptr;
    QPushButton *m_clearBtn = nullptr;
    bool m_streaming = false;
};

} // namespace cubeshell
