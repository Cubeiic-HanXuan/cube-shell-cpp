// NginxLogViewer.cpp — see NginxLogViewer.h.

#include "NginxLogViewer.h"

#include <QCloseEvent>
#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace cubeshell {

NginxLogViewer::NginxLogViewer(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Nginx 日志"));
    resize(760, 480);

    auto *layout = new QVBoxLayout(this);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setText(QString());
    layout->addWidget(m_statusLabel);

    m_view = new QPlainTextEdit(this);
    m_view->setReadOnly(true);
    m_view->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_view->setFont(mono);
    layout->addWidget(m_view, 1);

    auto *buttons = new QHBoxLayout();
    m_clearBtn = new QPushButton(tr("清空"), this);
    m_stopBtn = new QPushButton(tr("停止跟随"), this);
    auto *closeBtn = new QPushButton(tr("关闭"), this);
    buttons->addWidget(m_clearBtn);
    buttons->addStretch(1);
    buttons->addWidget(m_stopBtn);
    buttons->addWidget(closeBtn);
    layout->addLayout(buttons);

    connect(m_clearBtn, &QPushButton::clicked, this, [this]() { m_view->clear(); });
    connect(m_stopBtn, &QPushButton::clicked, this, [this]() { emit stopRequested(); });
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);

    updateButtons();
}

void NginxLogViewer::showText(const QString &title, const QString &text)
{
    m_streaming = false;
    setWindowTitle(tr("Nginx 日志 - %1").arg(title));
    m_statusLabel->setText(tr("（静态快照）"));
    m_view->setPlainText(text);
    updateButtons();
}

void NginxLogViewer::beginStream(const QString &title)
{
    m_streaming = true;
    setWindowTitle(tr("Nginx 日志 - %1").arg(title));
    m_statusLabel->setText(tr("跟随中…"));
    m_view->clear();
    updateButtons();
}

void NginxLogViewer::appendChunk(const QString &text)
{
    if (text.isEmpty())
        return;
    m_view->moveCursor(QTextCursor::End);
    m_view->insertPlainText(text);
    m_view->moveCursor(QTextCursor::End);
}

void NginxLogViewer::finishStream(int exitCode)
{
    m_streaming = false;
    m_statusLabel->setText(tr("已结束（退出码 %1）").arg(exitCode));
    updateButtons();
}

void NginxLogViewer::closeEvent(QCloseEvent *event)
{
    if (m_streaming)
        emit stopRequested();
    QDialog::closeEvent(event);
}

void NginxLogViewer::updateButtons()
{
    m_stopBtn->setEnabled(m_streaming);
}

} // namespace cubeshell
