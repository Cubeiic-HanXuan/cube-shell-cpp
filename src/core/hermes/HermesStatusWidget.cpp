// HermesStatusWidget.cpp — see HermesStatusWidget.h for the port map.
// 对应Python: core/hermes/status_widget.py

#include "hermes/HermesStatusWidget.h"

#include "hermes/HermesBackend.h"

#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace cubeshell {

namespace {
// 状态色与 Python 版一致
const QLatin1String kColorGreen("#27ae60");
const QLatin1String kColorRed("#e74c3c");
const QLatin1String kColorGray("#7f8c8d");

QFont boldFont()
{
    QFont font;
    font.setBold(true);
    return font;
}
} // namespace

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

HermesStatusWidget::HermesStatusWidget(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
}

HermesStatusWidget::~HermesStatusWidget()
{
    for (QFuture<void> &f : m_futures)
        f.waitForFinished();
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

// 对应Python: _init_ui
void HermesStatusWidget::buildUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(12);

    // --- 状态卡片区域 (2x2 网格) --- 对应Python: _create_card x4
    auto *cardsLayout = new QGridLayout();
    cardsLayout->setSpacing(10);

    QWidget *versionFrame = nullptr;
    QWidget *installFrame = nullptr;
    QWidget *gatewayFrame = nullptr;
    QWidget *apiFrame = nullptr;
    m_cardVersion = createCard(tr("版本信息"), QStringLiteral("--"),
                               &versionFrame);
    m_cardInstall = createCard(tr("安装状态"), tr("检测中..."), &installFrame);
    m_cardGateway = createCard(tr("网关状态"), tr("检测中..."), &gatewayFrame);
    m_cardApi = createCard(tr("API Server"), tr("检测中..."), &apiFrame);

    cardsLayout->addWidget(versionFrame, 0, 0);
    cardsLayout->addWidget(installFrame, 0, 1);
    cardsLayout->addWidget(gatewayFrame, 1, 0);
    cardsLayout->addWidget(apiFrame, 1, 1);
    mainLayout->addLayout(cardsLayout);

    // --- 快速操作按钮区域 ---
    auto *btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(8);

    m_refreshBtn = new QPushButton(tr("刷新状态"), this);
    m_startGwBtn = new QPushButton(tr("启动网关"), this);
    m_stopGwBtn = new QPushButton(tr("停止网关"), this);
    m_doctorBtn = new QPushButton(tr("检查修复"), this);
    m_updateBtn = new QPushButton(tr("更新 Hermes"), this);
    // 安装按钮：仅在检测到未安装 Hermes 时显示
    m_installBtn = new QPushButton(tr("安装 Hermes"), this);
    m_installBtn->setVisible(false);

    connect(m_refreshBtn, &QPushButton::clicked,
            this, &HermesStatusWidget::refreshStatus);
    connect(m_startGwBtn, &QPushButton::clicked,
            this, &HermesStatusWidget::onStartGateway);
    connect(m_stopGwBtn, &QPushButton::clicked,
            this, &HermesStatusWidget::onStopGateway);
    connect(m_doctorBtn, &QPushButton::clicked,
            this, &HermesStatusWidget::onRunDoctor);
    connect(m_updateBtn, &QPushButton::clicked,
            this, &HermesStatusWidget::onUpdateHermes);
    connect(m_installBtn, &QPushButton::clicked,
            this, &HermesStatusWidget::onInstallHermes);

    btnLayout->addWidget(m_refreshBtn);
    btnLayout->addWidget(m_startGwBtn);
    btnLayout->addWidget(m_stopGwBtn);
    btnLayout->addWidget(m_doctorBtn);
    btnLayout->addWidget(m_updateBtn);
    btnLayout->addWidget(m_installBtn);
    btnLayout->addStretch();
    mainLayout->addLayout(btnLayout);

    // --- 日志输出区域 ---
    auto *logLabel = new QLabel(tr("操作日志"), this);
    logLabel->setFont(boldFont());
    mainLayout->addWidget(logLabel);

    m_logOutput = new QTextEdit(this);
    m_logOutput->setReadOnly(true);
    m_logOutput->setMaximumHeight(200);
    m_logOutput->setPlaceholderText(tr("命令输出将显示在此处..."));
    mainLayout->addWidget(m_logOutput);

    mainLayout->addStretch();
}

// 对应Python: _create_card
HermesStatusWidget::StatusCard HermesStatusWidget::createCard(
    const QString &titleText, const QString &valueText, QWidget **frameOut)
{
    auto *frame = new QFrame(this);
    frame->setFrameShape(QFrame::StyledPanel);
    auto *cardLayout = new QVBoxLayout(frame);
    cardLayout->setContentsMargins(12, 10, 12, 10);
    cardLayout->setSpacing(6);

    auto *titleLabel = new QLabel(titleText, frame);
    titleLabel->setFont(boldFont());

    auto *valueLabel = new QLabel(valueText, frame);
    valueLabel->setStyleSheet(
        QStringLiteral("font-size: 14px; border: none;"));
    valueLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    cardLayout->addWidget(titleLabel);
    cardLayout->addWidget(valueLabel);
    cardLayout->addStretch();

    *frameOut = frame;
    return {titleLabel, valueLabel};
}

// 对应Python: _set_card_status
void HermesStatusWidget::setCardStatus(const StatusCard &card,
                                       const QString &text,
                                       const QString &color)
{
    card.value->setText(text);
    card.value->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px; font-weight: bold; "
                       "border: none;").arg(color));
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

void HermesStatusWidget::setBackend(HermesBackend *backend)
{
    m_backend = backend;
}

void HermesStatusWidget::refresh()
{
    refreshStatus();
}

// 对应Python: refresh_status
void HermesStatusWidget::refreshStatus()
{
    if (!m_backend)
        return;
    setButtonsEnabled(false);
    runCommand({QStringLiteral("--version")}, tr("获取版本"),
               "handleVersionResult");
    runCommand({QStringLiteral("status")}, tr("获取状态"),
               "handleStatusResult");
}

// ---------------------------------------------------------------------------
// button actions
// ---------------------------------------------------------------------------

void HermesStatusWidget::onStartGateway()
{
    if (!m_backend)
        return;
    runCommand({QStringLiteral("gateway"), QStringLiteral("start")},
               tr("启动网关"), "handleCommandDone");
}

void HermesStatusWidget::onStopGateway()
{
    if (!m_backend)
        return;
    runCommand({QStringLiteral("gateway"), QStringLiteral("stop")},
               tr("停止网关"), "handleCommandDone");
}

void HermesStatusWidget::onRunDoctor()
{
    if (!m_backend)
        return;
    runCommand({QStringLiteral("doctor"), QStringLiteral("--fix")},
               tr("检查修复"), "handleCommandDone");
}

// 对应Python: _on_update
void HermesStatusWidget::onUpdateHermes()
{
    if (!m_backend)
        return;
    startPip(true);
}

// 对应Python: _on_install（安装不依赖 backend——未安装时 backend 可能不可用）
void HermesStatusWidget::onInstallHermes()
{
    startPip(false);
}

// ---------------------------------------------------------------------------
// pip install / upgrade
// ---------------------------------------------------------------------------

// 对应Python: PipInstallWorker.run
void HermesStatusWidget::startPip(bool upgrade)
{
    if (m_pipProcess && m_pipProcess->state() != QProcess::NotRunning)
        return;
    setButtonsEnabled(false);
    m_pipUpgrade = upgrade;

    const QString timestamp = QDateTime::currentDateTime()
                                  .toString(QStringLiteral("HH:mm:ss"));
    m_logOutput->append(QStringLiteral("[%1] %2")
                            .arg(timestamp,
                                 upgrade ? tr("正在更新 Hermes...")
                                         : tr("正在安装 Hermes...")));

    if (!m_pipProcess) {
        m_pipProcess = new QProcess(this);
        m_pipProcess->setProcessChannelMode(QProcess::MergedChannels);
        connect(m_pipProcess,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int exitCode, QProcess::ExitStatus) {
                    onPipFinished(exitCode);
                });
        connect(m_pipProcess, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError) {
                    const QString timestamp =
                        QDateTime::currentDateTime()
                            .toString(QStringLiteral("HH:mm:ss"));
                    m_logOutput->append(QStringLiteral("[%1] %2: %3")
                                            .arg(timestamp, tr("更新失败"),
                                                 m_pipProcess->errorString()));
                    m_logOutput->append(QString());
                    setButtonsEnabled(true);
                });
    }

#ifdef Q_OS_WIN
    // Windows GUI 应用下防止弹出控制台黑窗口
    m_pipProcess->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) {
            args->flags |= 0x08000000; // CREATE_NO_WINDOW
        });
    const QString python = QStringLiteral("python");
#else
    const QString python = QStringLiteral("python3");
#endif

    QStringList args{QStringLiteral("-m"), QStringLiteral("pip"),
                     QStringLiteral("install")};
    if (upgrade)
        args << QStringLiteral("--upgrade");
    args << QStringLiteral("hermes-agent");
    m_pipProcess->start(python, args);
}

// 对应Python: _on_update_finished
void HermesStatusWidget::onPipFinished(int exitCode)
{
    Q_UNUSED(exitCode);
    const QString timestamp = QDateTime::currentDateTime()
                                  .toString(QStringLiteral("HH:mm:ss"));
    m_logOutput->append(QStringLiteral("[%1] %2")
                            .arg(timestamp, tr("更新完成")));
    const QString output = QString::fromLocal8Bit(
        m_pipProcess->readAllStandardOutput()).trimmed();
    if (!output.isEmpty())
        m_logOutput->append(output);
    m_logOutput->append(QString());
    setButtonsEnabled(true);
    // 更新完成后自动刷新状态
    refreshStatus();
}

// ---------------------------------------------------------------------------
// async CLI execution
// ---------------------------------------------------------------------------

// 对应Python: _run_command + _on_worker_finished
void HermesStatusWidget::runCommand(const QStringList &args,
                                    const QString &description,
                                    const char *handler)
{
    for (int i = m_futures.size() - 1; i >= 0; --i) {
        if (m_futures.at(i).isFinished())
            m_futures.removeAt(i);
    }
    HermesBackend *backend = m_backend;
    const QByteArray handlerName(handler);
    m_futures.append(QtConcurrent::run(
        [this, backend, args, description, handlerName]() {
            const QString output = backend->execCli(args);
            // 回 UI 线程：先写日志，再分发给对应的结果回调
            QMetaObject::invokeMethod(this, [this, description, output,
                                             handlerName]() {
                appendLog(description, output);
                QMetaObject::invokeMethod(this, handlerName.constData(),
                                          Qt::DirectConnection,
                                          Q_ARG(QString, description),
                                          Q_ARG(QString, output));
            }, Qt::QueuedConnection);
        }));
}

void HermesStatusWidget::appendLog(const QString &description,
                                   const QString &output)
{
    const QString timestamp = QDateTime::currentDateTime()
                                  .toString(QStringLiteral("HH:mm:ss"));
    m_logOutput->append(QStringLiteral("[%1] hermes %2")
                            .arg(timestamp, description));
    const QString trimmed = output.trimmed();
    if (!trimmed.isEmpty())
        m_logOutput->append(trimmed);
    m_logOutput->append(QString());
}

// ---------------------------------------------------------------------------
// result handlers (UI thread)
// ---------------------------------------------------------------------------

// 对应Python: _on_version_result
void HermesStatusWidget::handleVersionResult(const QString &description,
                                             const QString &output)
{
    Q_UNUSED(description);
    const QString trimmed = output.trimmed();
    m_cardVersion.value->setText(trimmed.isEmpty() ? tr("未安装") : trimmed);
    // 判断安装状态：有输出且不含 "not found" 视为已安装
    const bool installed = !trimmed.isEmpty()
        && !output.toLower().contains(QLatin1String("not found"));
    if (installed)
        setCardStatus(m_cardInstall, tr("已安装"), kColorGreen);
    else
        setCardStatus(m_cardInstall, tr("未安装"), kColorRed);
    updateInstallButtonVisibility(installed);
}

// 对应Python: _on_status_result
void HermesStatusWidget::handleStatusResult(const QString &description,
                                            const QString &output)
{
    Q_UNUSED(description);
    const QString text = output.toLower();

    // 解析网关状态
    if (text.contains(QLatin1String("gateway"))
        && text.contains(QLatin1String("running"))) {
        setCardStatus(m_cardGateway, tr("运行中"), kColorGreen);
    } else if (text.contains(QLatin1String("gateway"))
               && (text.contains(QLatin1String("stopped"))
                   || text.contains(QLatin1String("inactive")))) {
        setCardStatus(m_cardGateway, tr("已停止"), kColorRed);
    } else {
        setCardStatus(m_cardGateway, tr("未知"), kColorGray);
    }

    // 解析 API Server 状态
    if (text.contains(QLatin1String("api"))
        && text.contains(QLatin1String("running"))) {
        setCardStatus(m_cardApi, tr("运行中"), kColorGreen);
    } else if (text.contains(QLatin1String("api"))
               && (text.contains(QLatin1String("stopped"))
                   || text.contains(QLatin1String("inactive")))) {
        setCardStatus(m_cardApi, tr("已停止"), kColorRed);
    } else {
        setCardStatus(m_cardApi, tr("未知"), kColorGray);
    }

    setButtonsEnabled(true);
}

// 对应Python: _on_command_done — 通用命令完成后自动刷新状态
void HermesStatusWidget::handleCommandDone(const QString &description,
                                           const QString &output)
{
    Q_UNUSED(description);
    Q_UNUSED(output);
    refreshStatus();
}

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------

// 对应Python: _set_buttons_enabled
void HermesStatusWidget::setButtonsEnabled(bool enabled)
{
    m_refreshBtn->setEnabled(enabled);
    m_startGwBtn->setEnabled(enabled);
    m_stopGwBtn->setEnabled(enabled);
    m_doctorBtn->setEnabled(enabled);
    m_updateBtn->setEnabled(enabled);
    m_installBtn->setEnabled(enabled);
}

// 对应Python: _update_install_button_visibility
void HermesStatusWidget::updateInstallButtonVisibility(bool installed)
{
    m_installBtn->setVisible(!installed);
    m_updateBtn->setVisible(installed);
}

} // namespace cubeshell
