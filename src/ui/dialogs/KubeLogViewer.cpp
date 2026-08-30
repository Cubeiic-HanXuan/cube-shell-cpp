// KubeLogViewer.cpp — see KubeLogViewer.h.

#include "KubeLogViewer.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

namespace cubeshell {

KubeLogViewer::KubeLogViewer(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Kubernetes 日志"));
    setMinimumSize(760, 480);
    setModal(false);

    auto *layout = new QVBoxLayout(this);
    m_view = new QPlainTextEdit(this);
    m_view->setReadOnly(true);
    m_view->setMaximumBlockCount(20000); // 长日志流防内存无界增长
    layout->addWidget(m_view);

    auto *bottom = new QHBoxLayout();
    m_statusLabel = new QLabel(this);
    auto *stopBtn = new QPushButton(tr("停止"), this);
    auto *closeBtn = new QPushButton(tr("关闭"), this);
    bottom->addWidget(m_statusLabel);
    bottom->addStretch();
    bottom->addWidget(stopBtn);
    bottom->addWidget(closeBtn);
    layout->addLayout(bottom);

    connect(stopBtn, &QPushButton::clicked, this, [this]() {
        if (m_streaming)
            emit stopRequested();
    });
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
}

void KubeLogViewer::showText(const QString &title, const QString &text)
{
    if (m_streaming) {
        // 一次性内容顶掉进行中的日志流：先让后端停掉。
        emit stopRequested();
        m_streaming = false;
    }
    setWindowTitle(title);
    m_view->setPlainText(text);
    m_statusLabel->clear();
    show();
    raise();
    activateWindow();
}

void KubeLogViewer::beginStream(const QString &title)
{
    setWindowTitle(title);
    m_view->clear();
    m_streaming = true;
    m_statusLabel->setText(tr("流式跟随中（关闭窗口即停止）"));
    show();
    raise();
    activateWindow();
}

void KubeLogViewer::appendChunk(const QString &text)
{
    // 逐块追加并滚到底部；用户上翻阅读时不强行抢滚动条。
    m_view->moveCursor(QTextCursor::End);
    m_view->insertPlainText(text);
    QScrollBar *bar = m_view->verticalScrollBar();
    if (bar && bar->value() >= bar->maximum() - 40)
        bar->setValue(bar->maximum());
}

void KubeLogViewer::finishStream(int exitCode)
{
    m_streaming = false;
    m_statusLabel->setText(tr("流已结束（退出码 %1）").arg(exitCode));
}

void KubeLogViewer::closeEvent(QCloseEvent *event)
{
    if (m_streaming)
        emit stopRequested();
    m_streaming = false;
    QDialog::closeEvent(event);
}

} // namespace cubeshell
