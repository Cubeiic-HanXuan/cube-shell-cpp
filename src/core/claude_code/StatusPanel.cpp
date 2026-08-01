// StatusPanel.cpp — see StatusPanel.h for the port map.
// 对应Python: core/claude_code/status_widget.py

#include "claude_code/StatusPanel.h"

#include "claude_code/ClaudeCodeBackend.h"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

namespace cubeshell {

// 对应Python: _bold_font
static QFont boldFont()
{
    QFont font;
    font.setBold(true);
    return font;
}

StatusPanel::StatusPanel(ClaudeCodeBackend *backend, QWidget *parent)
    : QWidget(parent)
    , m_backend(backend)
{
    buildUi();
    // backend 信号来自工作线程，必须 QueuedConnection 切回 UI 线程
    connect(m_backend, &ClaudeCodeBackend::statusLoaded,
            this, &StatusPanel::onStatusLoaded, Qt::QueuedConnection);
    connect(m_backend, &ClaudeCodeBackend::updateFinished,
            this, &StatusPanel::onUpdateFinished, Qt::QueuedConnection);
}

// 对应Python: StatusWidget._init_ui（行 86-145）
void StatusPanel::buildUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(12);

    // ─── 状态卡片区域 (2x2 网格) ───
    auto *cardsLayout = new QGridLayout;
    cardsLayout->setSpacing(10);

    m_cardVersion = createCard(tr("版本信息"), QStringLiteral("--"));
    m_cardAuth = createCard(tr("认证状态"), tr("检测中..."));
    m_cardDaemon = createCard(tr("守护进程"), tr("检测中..."));
    m_cardPath = createCard(tr("安装路径"), tr("检测中..."));

    cardsLayout->addWidget(m_cardVersion.frame, 0, 0);
    cardsLayout->addWidget(m_cardAuth.frame, 0, 1);
    cardsLayout->addWidget(m_cardDaemon.frame, 1, 0);
    cardsLayout->addWidget(m_cardPath.frame, 1, 1);

    mainLayout->addLayout(cardsLayout);

    // ─── 快速操作按钮区域 ───
    auto *btnLayout = new QHBoxLayout;
    btnLayout->setSpacing(8);

    m_btnRefresh = new QPushButton(tr("刷新状态"), this);
    m_btnOpenTerminal = new QPushButton(tr("打开终端"), this);
    m_btnAgentView = new QPushButton(tr("Agent View"), this);
    m_btnUpdate = new QPushButton(tr("更新 Claude"), this);
    // 安装按钮：仅在检测到未安装时显示（见 updateInstallButtonVisibility）
    m_btnInstall = new QPushButton(tr("安装 Claude Code"), this);
    m_btnInstall->setVisible(false);

    connect(m_btnRefresh, &QPushButton::clicked, this, &StatusPanel::refresh);
    connect(m_btnOpenTerminal, &QPushButton::clicked,
            this, &StatusPanel::onOpenTerminal);
    connect(m_btnAgentView, &QPushButton::clicked,
            this, &StatusPanel::onAgentView);
    connect(m_btnUpdate, &QPushButton::clicked, this, &StatusPanel::onUpdate);
    connect(m_btnInstall, &QPushButton::clicked, this, &StatusPanel::onInstall);

    btnLayout->addWidget(m_btnRefresh);
    btnLayout->addWidget(m_btnOpenTerminal);
    btnLayout->addWidget(m_btnAgentView);
    btnLayout->addWidget(m_btnUpdate);
    btnLayout->addWidget(m_btnInstall);
    btnLayout->addStretch();

    mainLayout->addLayout(btnLayout);

    // ─── 日志输出区域 ───
    auto *logLabel = new QLabel(tr("操作日志"), this);
    logLabel->setFont(boldFont());
    mainLayout->addWidget(logLabel);

    m_logOutput = new QTextEdit(this);
    m_logOutput->setReadOnly(true);
    m_logOutput->setMaximumHeight(200);
    m_logOutput->setPlaceholderText(tr("状态信息将显示在此处..."));
    mainLayout->addWidget(m_logOutput);

    mainLayout->addStretch();
}

// 对应Python: StatusWidget.refresh（行 153-166）
void StatusPanel::refresh()
{
    if (!m_backend)
        return;
    if (m_statusPending)   // 避免重复启动
        return;
    m_statusPending = true;
    setButtonsEnabled(false);
    logAppend(tr("正在获取 Claude Code 状态..."));
    m_backend->refreshStatus();
}

// 对应Python: _on_open_terminal（先选项目文件夹再运行 claude）
void StatusPanel::onOpenTerminal()
{
    openInSelectedDir(QStringLiteral("claude"), tr("选择项目文件夹"));
}

// 对应Python: _on_agent_view
void StatusPanel::onAgentView()
{
    openInSelectedDir(QStringLiteral("claude agents"), tr("选择项目文件夹"));
}

// 对应Python: _open_in_selected_dir（行 178-188）
void StatusPanel::openInSelectedDir(const QString &claudeCmd,
                                    const QString &caption)
{
    const QString directory = QFileDialog::getExistingDirectory(
        this, caption, m_lastDir.isEmpty() ? QDir::homePath() : m_lastDir);
    if (directory.isEmpty())
        return; // 用户取消
    m_lastDir = directory;
    emit openTerminalRequested(
        ClaudeCodeBackend::buildCdCommand(directory, claudeCmd));
}

// 对应Python: _on_update（行 190-202）
void StatusPanel::onUpdate()
{
    if (!m_backend || m_updatePending)
        return;
    m_updatePending = true;
    setButtonsEnabled(false);
    logAppend(tr("正在更新 Claude Code..."));
    m_backend->refreshUpdate();
}

// 对应Python: _on_install（行 204-213）：安装走真实终端
void StatusPanel::onInstall()
{
    logAppend(tr("正在打开终端安装 Claude Code，安装完成后请点击\"刷新状态\"..."));
    emit openTerminalRequested(ClaudeCodeBackend::buildInstallCommand());
}

// 对应Python: _on_update_finished（行 215-223）
void StatusPanel::onUpdateFinished(const QString &output)
{
    m_updatePending = false;
    const QString timestamp =
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    logAppend(QStringLiteral("[%1] %2").arg(timestamp, tr("更新完成")));
    if (!output.isEmpty())
        logAppend(output);
    setButtonsEnabled(true);
    // 更新完成后自动刷新状态
    refresh();
}

// 对应Python: _on_status_loaded（行 233-300）
void StatusPanel::onStatusLoaded(const QString &version,
                                 const QJsonObject &auth,
                                 const QJsonObject &daemon,
                                 const QString &binPath)
{
    m_statusPending = false;
    const QString timestamp =
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));

    // 版本信息
    if (!version.isEmpty()) {
        setCardStatus(m_cardVersion, version, QStringLiteral("#27ae60"));
    } else {
        setCardStatus(m_cardVersion, tr("未安装"), QStringLiteral("#e74c3c"));
    }
    // 未安装（version 为空）时显示"安装 Claude Code"按钮，已安装则隐藏
    updateInstallButtonVisibility(!version.isEmpty());

    // 认证状态
    // 真实 JSON 形如：{"loggedIn": true, "authMethod": "oauth_token", ...}
    if (!auth.isEmpty()) {
        const bool isAuthed =
            auth.value(QStringLiteral("loggedIn"))
                .toBool(auth.value(QStringLiteral("authenticated"))
                            .toBool(auth.value(QStringLiteral("logged_in"))
                                        .toBool(false)));
        QString account = auth.value(QStringLiteral("account")).toString();
        if (account.isEmpty())
            account = auth.value(QStringLiteral("email")).toString();
        if (account.isEmpty())
            account = auth.value(QStringLiteral("organization")).toString();
        QString method = auth.value(QStringLiteral("authMethod")).toString();
        if (method.isEmpty())
            method = auth.value(QStringLiteral("auth_method")).toString();
        if (isAuthed) {
            QString statusText;
            if (!account.isEmpty())
                statusText = account;
            else if (!method.isEmpty())
                statusText = QStringLiteral("%1 (%2)").arg(tr("已登录"), method);
            else
                statusText = tr("已登录");
            setCardStatus(m_cardAuth, statusText, QStringLiteral("#27ae60"));
        } else if (!account.isEmpty()) {
            setCardStatus(m_cardAuth, account, QStringLiteral("#27ae60"));
        } else {
            setCardStatus(m_cardAuth, tr("未登录"), QStringLiteral("#e74c3c"));
        }
    } else {
        setCardStatus(m_cardAuth, tr("未知"), QStringLiteral("#7f8c8d"));
    }

    // 守护进程状态
    if (daemon.value(QStringLiteral("running")).toBool(false)) {
        QJsonValue workers = daemon.value(QStringLiteral("workers"));
        if (workers.isUndefined())
            workers = daemon.value(QStringLiteral("num_workers"));
        QString statusText = tr("运行中");
        // Python 真值判断：workers 为 0/空串时不显示括号后缀
        QString workersText;
        if (workers.isDouble()) {
            if (workers.toDouble() != 0.0)
                workersText = QString::number(workers.toInt());
        } else {
            workersText = workers.toString();
        }
        if (!workersText.isEmpty())
            statusText += QStringLiteral(" (%1 workers)").arg(workersText);
        setCardStatus(m_cardDaemon, statusText, QStringLiteral("#27ae60"));
    } else {
        setCardStatus(m_cardDaemon, tr("未运行"), QStringLiteral("#e74c3c"));
    }

    // 安装路径（12px 绿色，区别于其它卡片的 14px 粗体）
    if (!binPath.isEmpty()) {
        m_cardPath.value->setText(binPath);
        m_cardPath.value->setStyleSheet(
            QStringLiteral("font-size: 12px; border: none; color: #27ae60;"));
    } else {
        setCardStatus(m_cardPath, tr("未找到"), QStringLiteral("#e74c3c"));
    }

    logAppend(QStringLiteral("[%1] %2").arg(timestamp, tr("状态刷新完成")));
    setButtonsEnabled(true);
}

// ─── UI 辅助方法 ───

// 对应Python: _create_card（行 316-336）
StatusPanel::Card StatusPanel::createCard(const QString &titleText,
                                          const QString &valueText)
{
    Card card;
    card.frame = new QFrame(this);
    card.frame->setFrameShape(QFrame::StyledPanel);
    auto *cardLayout = new QVBoxLayout(card.frame);
    cardLayout->setContentsMargins(12, 10, 12, 10);
    cardLayout->setSpacing(6);

    card.title = new QLabel(titleText, card.frame);
    card.title->setFont(boldFont());

    card.value = new QLabel(valueText, card.frame);
    card.value->setStyleSheet(QStringLiteral("font-size: 14px; border: none;"));
    card.value->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    card.value->setWordWrap(true);

    cardLayout->addWidget(card.title);
    cardLayout->addWidget(card.value);
    cardLayout->addStretch();
    return card;
}

// 对应Python: _set_card_status（行 338-343）
void StatusPanel::setCardStatus(const Card &card, const QString &text,
                                const QString &color)
{
    card.value->setText(text);
    card.value->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px; font-weight: bold; "
                       "border: none;").arg(color));
}

// 对应Python: _set_buttons_enabled
void StatusPanel::setButtonsEnabled(bool enabled)
{
    m_btnRefresh->setEnabled(enabled);
    m_btnOpenTerminal->setEnabled(enabled);
    m_btnAgentView->setEnabled(enabled);
    m_btnUpdate->setEnabled(enabled);
    m_btnInstall->setEnabled(enabled);
}

// 对应Python: _update_install_button_visibility（行 359-366）
// 已安装：隐藏"安装"、显示"更新"；未安装：显示"安装"、隐藏"更新"。
void StatusPanel::updateInstallButtonVisibility(bool installed)
{
    m_btnInstall->setVisible(!installed);
    m_btnUpdate->setVisible(installed);
}

// 对应Python: _log_append
void StatusPanel::logAppend(const QString &message)
{
    m_logOutput->append(message);
}

} // namespace cubeshell
