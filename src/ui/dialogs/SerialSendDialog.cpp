#include "SerialSendDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "serial/SerialLineSender.h"

namespace cubeshell {

SerialSendDialog::SerialSendDialog(SerialClient *client, SerialBridge *bridge, QWidget *parent)
    : QDialog(parent), m_sender(new SerialLineSender(client, bridge, this))
{
    setObjectName(QStringLiteral("serialSendDialog"));
    setWindowTitle(tr("串口多行发送"));
    setModal(false);
    resize(760, 520);

    auto *layout = new QVBoxLayout(this);
    m_connection = new QLabel(this);
    m_connection->setWordWrap(true);
    layout->addWidget(m_connection);

    auto *inputRow = new QHBoxLayout;
    m_import = new QPushButton(tr("从剪贴板载入"), this);
    m_import->setObjectName(QStringLiteral("serialSendImport"));
    m_edit = new QPushButton(tr("编辑文本…"), this);
    m_edit->setObjectName(QStringLiteral("serialSendEdit"));
    inputRow->addWidget(m_import);
    inputRow->addWidget(m_edit);
    inputRow->addStretch();
    inputRow->addWidget(new QLabel(tr("空行也会发送回车；双击命令可编辑"), this));
    layout->addLayout(inputRow);

    m_table = new QTableWidget(0, 4, this);
    m_table->setObjectName(QStringLiteral("serialSendLines"));
    m_table->setHorizontalHeaderLabels({tr("行"), tr("命令"), tr("状态"), tr("发送")});
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setWordWrap(false);
    layout->addWidget(m_table, 1);

    auto *sendRow = new QHBoxLayout;
    m_next = new QPushButton(tr("发送下一行"), this);
    m_next->setObjectName(QStringLiteral("serialSendNext"));
    const QKeySequence nextKey(Qt::CTRL | Qt::Key_Return);
    m_next->setToolTip(tr("发送下一条待发送命令（%1）").arg(nextKey.toString(QKeySequence::NativeText)));
    auto *nextShortcut = new QShortcut(nextKey, this);
    nextShortcut->setContext(Qt::WindowShortcut);
    connect(nextShortcut, &QShortcut::activated, m_next, &QPushButton::click);
    m_auto = new QPushButton(tr("自动发送"), this);
    m_auto->setObjectName(QStringLiteral("serialSendAuto"));
    m_pause = new QPushButton(tr("暂停"), this);
    m_pause->setObjectName(QStringLiteral("serialSendPause"));
    m_stop = new QPushButton(tr("停止"), this);
    m_stop->setObjectName(QStringLiteral("serialSendStop"));
    m_interval = new QSpinBox(this);
    m_interval->setObjectName(QStringLiteral("serialSendInterval"));
    m_interval->setRange(10, 60000);
    m_interval->setValue(200);
    m_interval->setSuffix(tr(" ms"));
    m_interval->setToolTip(tr("本机发送缓冲排空后，等待此间隔再发送下一行。不会等待设备响应。"));
    sendRow->addWidget(m_next);
    sendRow->addWidget(m_auto);
    sendRow->addWidget(new QLabel(tr("行间隔："), this));
    sendRow->addWidget(m_interval);
    sendRow->addStretch();
    sendRow->addWidget(m_pause);
    sendRow->addWidget(m_stop);
    layout->addLayout(sendRow);

    m_progress = new QLabel(this);
    m_progress->setObjectName(QStringLiteral("serialSendProgress"));
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("serialSendStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    layout->addWidget(m_progress);
    layout->addWidget(m_status);
    auto *notice = new QLabel(tr("每行自动补回车，采用本会话的发送换行设置。已发送不代表设备执行成功。\n"
                                "停止或关闭窗口只取消后续发送，无法撤回已提交的数据；断线后不会自动续发。"), this);
    notice->setWordWrap(true);
    layout->addWidget(notice);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // Enter 不能意外触发默认按钮发送命令；发送下一行使用显式快捷键。
    for (QPushButton *button : findChildren<QPushButton *>())
        button->setAutoDefault(false);
    connect(m_import, &QPushButton::clicked, this, &SerialSendDialog::loadClipboard);
    connect(m_edit, &QPushButton::clicked, this, &SerialSendDialog::editText);
    connect(m_next, &QPushButton::clicked, m_sender, &SerialLineSender::sendNext);
    connect(m_auto, &QPushButton::clicked, this, [this]() {
        m_sender->startAutomatic(m_interval->value());
    });
    connect(m_pause, &QPushButton::clicked, this, [this]() {
        if (m_sender->state() == SerialLineSender::State::Paused)
            m_sender->resume();
        else
            m_sender->pause();
    });
    connect(m_stop, &QPushButton::clicked, m_sender, &SerialLineSender::stop);
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (item->column() == 1 && !m_sender->setLineText(item->row(), item->text()))
            updateUi();
    });
    connect(m_sender, &SerialLineSender::changed, this, &SerialSendDialog::updateUi);
    updateUi();
}

SerialSendDialog::~SerialSendDialog()
{
    // 销毁子控件前断开刷新，避免 sender 的停止通知访问正在析构的 UI。
    disconnect(m_sender, nullptr, this, nullptr);
    m_sender->stop();
}

void SerialSendDialog::setConnectionInfo(const QString &portName, SerialSettings::NewlineMode mode)
{
    const QString name = portName.isEmpty() ? tr("未选择端口") : portName;
    QString ending = QStringLiteral("CR");
    if (mode == SerialSettings::NewlineMode::Lf)
        ending = QStringLiteral("LF");
    else if (mode == SerialSettings::NewlineMode::CrLf)
        ending = QStringLiteral("CRLF");
    setWindowTitle(tr("串口多行发送 — %1").arg(name));
    m_connection->setTextFormat(Qt::PlainText);
    m_connection->setText(tr("目标串口：%1　发送换行：%2").arg(name, ending));
}

void SerialSendDialog::loadClipboard()
{
    if (m_sender->isBusy())
        return;
    const QString text = QApplication::clipboard()->text();
    if (text.isEmpty()) {
        QMessageBox::information(this, tr("逐行粘贴"), tr("剪贴板中没有文本。"));
        return;
    }
    replaceText(text, true);
}

bool SerialSendDialog::replaceText(const QString &text, bool confirmExisting)
{
    if (m_sender->isBusy())
        return false;
    if (confirmExisting && !m_sender->lines().isEmpty()
        && QMessageBox::question(this, tr("替换命令列表"),
                                 tr("替换当前命令列表并重置发送进度？已有内容不会自动保存。"),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
        return false;
    return m_sender->setText(text);
}

void SerialSendDialog::editText()
{
    if (m_sender->isBusy())
        return;
    QString text;
    bool hasProgress = false;
    for (const auto &line : m_sender->lines()) {
        text += line.text + QLatin1Char('\n');
        hasProgress |= line.state != SerialLineSender::LineState::Pending;
    }
    bool ok = false;
    const QString edited = QInputDialog::getMultiLineText(
        this, tr("编辑待发送文本"), tr("每行一条命令，空行会发送一次回车："), text, &ok);
    if (ok)
        replaceText(edited, hasProgress);
}

void SerialSendDialog::done(int result)
{
    m_sender->stop();
    QDialog::done(result);
}

void SerialSendDialog::updateUi()
{
    using State = SerialLineSender::State;
    using LineState = SerialLineSender::LineState;
    const bool busy = m_sender->isBusy();
    const bool canSend = m_sender->canSendManually();
    const bool hasPending = m_sender->nextPendingLine() >= 0;
    const State state = m_sender->state();
    const auto &lines = m_sender->lines();
    const QSignalBlocker blocker(m_table);
    m_table->setRowCount(lines.size());
    const QFont commandFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    for (int i = 0; i < lines.size(); ++i) {
        if (!m_table->item(i, 0)) {
            for (int column = 0; column < 3; ++column)
                m_table->setItem(i, column, new QTableWidgetItem);
            auto *button = new QToolButton(m_table);
            button->setText(QStringLiteral("▶"));
            button->setToolTip(tr("发送第 %1 行").arg(i + 1));
            connect(button, &QToolButton::clicked, this, [this, i]() { m_sender->sendLine(i); });
            m_table->setCellWidget(i, 3, button);
        }
        auto *number = m_table->item(i, 0);
        number->setText(QString::number(i + 1));
        number->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        auto *command = m_table->item(i, 1);
        if (command->text() != lines[i].text)
            command->setText(lines[i].text);
        command->setFont(commandFont);
        command->setToolTip(lines[i].text.isEmpty() ? tr("空行：发送一次回车") : lines[i].text);
        Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (!busy && lines[i].state == LineState::Pending)
            flags |= Qt::ItemIsEditable;
        command->setFlags(flags);
        auto *status = m_table->item(i, 2);
        status->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        switch (lines[i].state) {
        case LineState::Pending:   status->setText(tr("待发送")); break;
        case LineState::Sending:   status->setText(tr("写出中")); break;
        case LineState::Sent:      status->setText(tr("已发送")); break;
        case LineState::Uncertain: status->setText(tr("未确认")); break;
        }
        status->setToolTip(lines[i].state == LineState::Uncertain
                              ? tr("可能已部分发送。请检查设备；需要重发时显式重新载入文本。") : QString());
        m_table->cellWidget(i, 3)->setEnabled(canSend && lines[i].state == LineState::Pending);
    }
    m_import->setEnabled(!busy);
    m_edit->setEnabled(!busy);
    m_interval->setEnabled(!busy);
    m_next->setEnabled(canSend && hasPending);
    m_auto->setEnabled(!busy && m_sender->isConnected() && hasPending);
    m_pause->setEnabled(state != State::Idle);
    m_pause->setText(state == State::Paused ? tr("继续") : tr("暂停"));
    m_stop->setEnabled(busy);
    m_progress->setText(tr("已发送 %1 / %2 行　%3")
                            .arg(m_sender->sentCount()).arg(lines.size())
                            .arg(m_sender->isConnected() ? tr("串口已连接") : tr("串口未连接")));
    m_status->setText(m_sender->message().isEmpty()
                         ? (state == State::Automatic ? tr("自动发送中…") : tr("先预览命令，再选择手动或自动发送。"))
                         : m_sender->message());
    if (m_wasBusy != busy) {
        m_wasBusy = busy;
        emit busyChanged(busy);
    }
}

} // namespace cubeshell
