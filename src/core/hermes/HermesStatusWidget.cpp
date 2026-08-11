// HermesStatusWidget.cpp — see HermesStatusWidget.h for the port map.
// 对应Python: core/hermes/status_widget.py

#include "hermes/HermesStatusWidget.h"

#include "hermes/HermesBackend.h"

#include <QDateTime>
#include <QDir>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
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

// 官方一键安装脚本(hermes-agent README)——Hermes 不发布到 PyPI
const QLatin1String kInstallShell(
    "curl -fsSL https://hermes-agent.nousresearch.com/install.sh | bash");
const QLatin1String kInstallPowerShell(
    "iex (irm https://hermes-agent.nousresearch.com/install.ps1)");

// 远程模式下 `hermes update` 要跑 git pull + 依赖重装,默认 30s 远远不够
constexpr int kUpdateTimeoutMs = 15 * 60 * 1000;

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
    // 更新/安装可能跑很久：析构时终止它，不阻塞退出
    if (m_taskProcess && m_taskProcess->state() != QProcess::NotRunning) {
        m_taskProcess->disconnect(this);
        m_taskProcess->kill();
        m_taskProcess->waitForFinished(3000);
    }
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
// Hermes 是 git 检出而非 PyPI 包,升级入口是 `hermes update`(见头文件说明);
// 原先的 `pip install --upgrade hermes-agent` 必然失败。
void HermesStatusWidget::onUpdateHermes()
{
    if (!m_backend)
        return;

    const QStringList cliArgs{QStringLiteral("update"), QStringLiteral("--yes")};

    // 远程模式没有本地进程可流式输出，退回阻塞式 CLI（放大超时）
    if (m_backend->isRemote()) {
        setButtonsEnabled(false);
        appendStamped(tr("正在更新 Hermes..."));
        runCommand(cliArgs, tr("update --yes"), "handleCommandDone",
                   kUpdateTimeoutMs);
        return;
    }

    const QString bin = m_backend->cliPath();
    startTask(TaskKind::Update, bin, cliArgs,
              bin + QLatin1Char(' ') + cliArgs.join(QLatin1Char(' ')));
}

// 对应Python: _on_install（安装不依赖 backend——未安装时 backend 可能不可用）
// pip 对安装同样无效，改为执行官方一键安装脚本，执行前必须用户确认。
void HermesStatusWidget::onInstallHermes()
{
#ifdef Q_OS_WIN
    const QString command = kInstallPowerShell;
#else
    const QString command = kInstallShell;
#endif

    // 远程模式不代跑安装脚本——目标主机的 shell 环境不可控，给出命令让用户自己执行
    if (m_backend && m_backend->isRemote()) {
        appendStamped(tr("远程主机未安装 Hermes Agent"));
        m_logOutput->append(tr("请在远程主机上执行官方安装命令：\n\n  %1\n\n"
                               "安装完成后点击「刷新状态」。").arg(kInstallShell));
        m_logOutput->append(QString());
        return;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("确认安装 Hermes"));
    box.setText(tr("即将执行：\n\n%1\n\n该脚本来自 NousResearch 官方"
                   "（hermes-agent.nousresearch.com）。").arg(command));
    QPushButton *okBtn = box.addButton(tr("确认执行"), QMessageBox::AcceptRole);
    QPushButton *cancelBtn = box.addButton(tr("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(cancelBtn);
    box.exec();
    if (box.clickedButton() != okBtn)
        return;

#ifdef Q_OS_WIN
    startTask(TaskKind::Install, QStringLiteral("powershell"),
              {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), command},
              command);
#else
    // 登录 shell：GUI 进程继承的环境常常缺少 curl/git/uv 所在的用户目录
    startTask(TaskKind::Install, QStringLiteral("/bin/bash"),
              {QStringLiteral("-lc"), command}, command);
#endif
}

// ---------------------------------------------------------------------------
// long-running task process (update / install)
// ---------------------------------------------------------------------------

// 取代 Python 版的 PipInstallWorker：异步 QProcess + 实时输出 + 真实退出码。
void HermesStatusWidget::startTask(TaskKind kind, const QString &program,
                                   const QStringList &args,
                                   const QString &displayCommand)
{
    if (m_taskProcess && m_taskProcess->state() != QProcess::NotRunning)
        return;
    setButtonsEnabled(false);
    m_taskKind = kind;
    m_taskBuffer.clear();
    m_taskFailed = false;

    appendStamped(kind == TaskKind::Update ? tr("正在更新 Hermes...")
                                           : tr("正在安装 Hermes..."));
    m_logOutput->append(QStringLiteral("$ ") + displayCommand);

    if (!m_taskProcess) {
        m_taskProcess = new QProcess(this);
        m_taskProcess->setProcessChannelMode(QProcess::MergedChannels);
        connect(m_taskProcess, &QProcess::readyReadStandardOutput,
                this, &HermesStatusWidget::onProcessOutput);
        connect(m_taskProcess,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &HermesStatusWidget::onProcessFinished);
        connect(m_taskProcess, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError error) {
                    // Crashed 等错误 finished() 也会到，只在这里处理起不来的情况
                    if (error != QProcess::FailedToStart)
                        return;
                    m_taskFailed = true;
                    appendStamped(m_taskKind == TaskKind::Update
                                      ? tr("更新失败：%1")
                                            .arg(m_taskProcess->errorString())
                                      : tr("安装失败：%1")
                                            .arg(m_taskProcess->errorString()));
                    m_logOutput->append(QString());
                    setButtonsEnabled(true);
                });

#ifdef Q_OS_WIN
        // Windows GUI 应用下防止弹出控制台黑窗口
        m_taskProcess->setCreateProcessArgumentsModifier(
            [](QProcess::CreateProcessArguments *args) {
                args->flags |= 0x08000000; // CREATE_NO_WINDOW
            });
#else
        // GUI 进程（Finder/Dock 启动）拿到的 PATH 通常只有 /usr/bin:/bin，
        // 而 hermes update 内部要调 git / uv / python。补上常见用户目录。
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        QStringList paths = env.value(QStringLiteral("PATH"))
                                .split(QLatin1Char(':'), Qt::SkipEmptyParts);
        const QString home = QDir::homePath();
        const QStringList extra{
            home + QStringLiteral("/.local/bin"),
            QStringLiteral("/opt/homebrew/bin"),
            QStringLiteral("/usr/local/bin"),
            QStringLiteral("/usr/bin"),
            QStringLiteral("/bin"),
        };
        for (const QString &p : extra) {
            if (!paths.contains(p))
                paths << p;
        }
        env.insert(QStringLiteral("PATH"), paths.join(QLatin1Char(':')));
        m_taskProcess->setProcessEnvironment(env);
#endif
    }

    m_taskProcess->start(program, args);
}

// 进程输出按行落到日志：QTextEdit::append 每次都开新段落，
// 所以必须自己按 \n 切分，未完成的尾部留到下次。
void HermesStatusWidget::drainProcessOutput(bool flush)
{
    if (m_taskProcess)
        m_taskBuffer += QString::fromLocal8Bit(
            m_taskProcess->readAllStandardOutput());
    m_taskBuffer.replace(QLatin1String("\r\n"), QLatin1String("\n"));

    int nl;
    while ((nl = m_taskBuffer.indexOf(QLatin1Char('\n'))) >= 0) {
        QString line = m_taskBuffer.left(nl);
        m_taskBuffer.remove(0, nl + 1);
        // git/pip 的进度条用 \r 原地刷新，只保留该行最终状态
        const int cr = line.lastIndexOf(QLatin1Char('\r'));
        if (cr >= 0)
            line = line.mid(cr + 1);
        m_logOutput->append(line);
    }

    if (flush && !m_taskBuffer.isEmpty()) {
        m_logOutput->append(m_taskBuffer);
        m_taskBuffer.clear();
    }
}

void HermesStatusWidget::onProcessOutput()
{
    drainProcessOutput(false);
}

// 取代 Python 版的 _on_update_finished —— 关键区别：退出码真的被用上了
void HermesStatusWidget::onProcessFinished(int exitCode,
                                           QProcess::ExitStatus status)
{
    drainProcessOutput(true); // 先把输出写完，再写结论行
    if (m_taskFailed)
        return; // FailedToStart 已经报过

    const bool ok = status == QProcess::NormalExit && exitCode == 0;
    if (ok) {
        appendStamped(m_taskKind == TaskKind::Update ? tr("更新完成")
                                                     : tr("安装完成"));
    } else if (status == QProcess::CrashExit) {
        appendStamped(m_taskKind == TaskKind::Update ? tr("更新失败：进程异常终止")
                                                     : tr("安装失败：进程异常终止"));
    } else {
        appendStamped(m_taskKind == TaskKind::Update
                          ? tr("更新失败（退出码 %1）").arg(exitCode)
                          : tr("安装失败（退出码 %1）").arg(exitCode));
    }
    m_logOutput->append(QString());
    setButtonsEnabled(true);
    // 完成后自动刷新状态（失败时也刷新，卡片要反映真实状态）
    if (m_backend)
        refreshStatus();
}

// ---------------------------------------------------------------------------
// async CLI execution
// ---------------------------------------------------------------------------

// 对应Python: _run_command + _on_worker_finished
// timeoutMs < 0 表示用 HermesBackend 的默认超时。
void HermesStatusWidget::runCommand(const QStringList &args,
                                    const QString &description,
                                    const char *handler, int timeoutMs)
{
    for (int i = m_futures.size() - 1; i >= 0; --i) {
        if (m_futures.at(i).isFinished())
            m_futures.removeAt(i);
    }
    HermesBackend *backend = m_backend;
    const QByteArray handlerName(handler);
    const int timeout = timeoutMs < 0 ? HermesBackend::kDefaultTimeoutMs
                                      : timeoutMs;
    m_futures.append(QtConcurrent::run(
        [this, backend, args, description, handlerName, timeout]() {
            const QString output = backend->execCli(args, timeout);
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

void HermesStatusWidget::appendStamped(const QString &text)
{
    const QString timestamp = QDateTime::currentDateTime()
                                  .toString(QStringLiteral("HH:mm:ss"));
    m_logOutput->append(QStringLiteral("[%1] %2").arg(timestamp, text));
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
