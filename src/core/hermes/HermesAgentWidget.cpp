// HermesAgentWidget.cpp — see HermesAgentWidget.h for the port map.
// 对应Python: core/hermes/agent_widget.py + core/hermes/config_highlighter.py

#include "hermes/HermesAgentWidget.h"

#include "hermes/HermesBackend.h"
#include "hermes/HermesHighlighters.h"

#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QStyle>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <yaml-cpp/yaml.h>

namespace cubeshell {

namespace {

// Profile QVariantMap 的键名。对应Python: profile dict 的字段
const QString kKeyName = QStringLiteral("name");
const QString kKeyActive = QStringLiteral("active");
const QString kKeyModel = QStringLiteral("model");
const QString kKeyGateway = QStringLiteral("gateway");
const QString kKeyAlias = QStringLiteral("alias");
const QString kKeyDistribution = QStringLiteral("distribution");

// 空值占位符。对应Python: "—"
const QString kEmptyMark = QStringLiteral("—");

// 一次保存请求中待写入的文件。对应Python: ProfileConfigSaver 的 writes 三元组
struct PendingWrite {
    QString path;
    QString content;
    QString label;
};

bool isGatewayRunning(const QVariantMap &profile)
{
    return profile.value(kKeyGateway).toString().compare(
               QLatin1String("running"), Qt::CaseInsensitive) == 0;
}

} // namespace

// ---------------------------------------------------------------------------
// ProfileItemDelegate
// ---------------------------------------------------------------------------

ProfileItemDelegate::ProfileItemDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

// 对应Python: ProfileItemDelegate.sizeHint
QSize ProfileItemDelegate::sizeHint(const QStyleOptionViewItem &option,
                                   const QModelIndex &index) const
{
    Q_UNUSED(index);
    return QSize(option.rect.width(), kItemHeight);
}

// 对应Python: ProfileItemDelegate.paint
void ProfileItemDelegate::paint(QPainter *painter,
                                const QStyleOptionViewItem &option,
                                const QModelIndex &index) const
{
    const QVariantMap profile = index.data(kProfileRole).toMap();
    if (profile.isEmpty()) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const QRect rect = option.rect;
    const bool isActive = profile.value(kKeyActive).toBool();
    const bool isSelected = option.state & QStyle::State_Selected;
    const bool isHovered = option.state & QStyle::State_MouseOver;

    // ─── 背景绘制 ───
    QColor bgColor(QStringLiteral("#1e1e1e"));
    if (isSelected)
        bgColor = QColor(QStringLiteral("#2a4a6b"));
    else if (isHovered)
        bgColor = QColor(QStringLiteral("#2a2a2a"));
    painter->fillRect(rect, bgColor);

    // ─── 活跃标识:左侧蓝色竖条 ───
    if (isActive) {
        const QRect barRect(rect.left(), rect.top() + 4,
                            kActiveBarWidth, rect.height() - 8);
        painter->fillRect(barRect, QColor(QStringLiteral("#1e90ff")));
    }

    // ─── 文字区域起始 X ───
    const int textX = rect.left() + kPaddingLeft
        + (isActive ? kActiveBarWidth + 4 : 0);
    const int firstLineY = rect.top() + kPaddingTop + 14;

    // ─── 第一行:Profile 名称 ───
    QFont nameFont;
    nameFont.setPixelSize(14);
    nameFont.setBold(true);
    painter->setFont(nameFont);
    painter->setPen(QColor(QStringLiteral("#ffffff")));
    painter->drawText(textX, firstLineY, profile.value(kKeyName).toString());

    // ─── 第一行右侧:运行状态 ───
    const bool isRunning = isGatewayRunning(profile);
    const QString statusText = isRunning ? QStringLiteral("running")
                                         : QStringLiteral("stopped");
    const QColor statusColor(isRunning ? QStringLiteral("#4caf50")
                                       : QStringLiteral("#888888"));

    QFont statusFont;
    statusFont.setPixelSize(11);
    painter->setFont(statusFont);
    const QFontMetrics fm = painter->fontMetrics();
    const int dotDiameter = kStatusDotRadius * 2;
    const int statusTotalWidth =
        dotDiameter + 4 + fm.horizontalAdvance(statusText);
    const int statusX = rect.right() - kPaddingRight - statusTotalWidth;

    // 小圆点
    painter->setPen(Qt::NoPen);
    painter->setBrush(QBrush(statusColor));
    const int dotCenterY = firstLineY - fm.ascent() / 2;
    painter->drawEllipse(statusX, dotCenterY - kStatusDotRadius,
                         dotDiameter, dotDiameter);

    // 状态文字
    painter->setPen(statusColor);
    painter->drawText(statusX + dotDiameter + 4, firstLineY, statusText);

    // ─── 第二行:模型名称 ───
    QFont modelFont;
    modelFont.setPixelSize(12);
    painter->setFont(modelFont);
    painter->setPen(QColor(QStringLiteral("#888888")));
    painter->drawText(textX, firstLineY + 18,
                      profile.value(kKeyModel).toString());

    // ─── 底部分隔线 ───
    painter->setPen(QPen(QColor(QStringLiteral("#333333")), 0.5));
    painter->drawLine(rect.left() + 8, rect.bottom(),
                      rect.right() - 8, rect.bottom());

    painter->restore();
}

// ---------------------------------------------------------------------------
// ProfileConfigDialog
// ---------------------------------------------------------------------------

ProfileConfigDialog::ProfileConfigDialog(HermesBackend *backend,
                                         const QString &profileName,
                                         const QString &configPath,
                                         const QString &envPath,
                                         QWidget *parent)
    : QDialog(parent)
    , m_backend(backend)
    , m_profileName(profileName)
    , m_configPath(configPath)
    , m_envPath(envPath)
{
    buildUi();
    load();
}

ProfileConfigDialog::~ProfileConfigDialog()
{
    // 后台读写还在跑时不能先析构:等它结束(投递到本对象的排队调用会随
    // QObject 析构一并被丢弃)。
    m_task.waitForFinished();
}

// 对应Python: ProfileConfigDialog._init_ui
void ProfileConfigDialog::buildUi()
{
    setWindowTitle(tr("编辑配置 - %1").arg(m_profileName));
    resize(720, 620);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    auto *pathLabel = new QLabel(m_configPath, this);
    pathLabel->setStyleSheet(QStringLiteral("color: #888888;"));
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(pathLabel);

    const QFont mono = monoFont();

    m_tabs = new QTabWidget(this);

    // config.yaml
    m_yamlEdit = new QPlainTextEdit(m_tabs);
    m_yamlEdit->setFont(mono);
    m_yamlEdit->setTabStopDistance(
        m_yamlEdit->fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 2);
    // 高亮器父对象为 document,随文档一同析构
    new YamlHighlighter(m_yamlEdit->document());
    m_tabs->addTab(m_yamlEdit, QStringLiteral("config.yaml"));

    // .env
    m_envEdit = new QPlainTextEdit(m_tabs);
    m_envEdit->setFont(mono);
    new DotenvHighlighter(m_envEdit->document());
    m_tabs->addTab(m_envEdit, QStringLiteral(".env"));

    root->addWidget(m_tabs);

    m_statusLabel = new QLabel(tr("加载中..."), this);
    m_statusLabel->setStyleSheet(QStringLiteral("color: #888888;"));
    root->addWidget(m_statusLabel);

    // 按钮:保存 / 重新加载 / 关闭
    auto *buttonBox = new QDialogButtonBox(this);
    m_saveBtn = buttonBox->addButton(tr("保存"), QDialogButtonBox::AcceptRole);
    m_reloadBtn = buttonBox->addButton(tr("重新加载"),
                                       QDialogButtonBox::ResetRole);
    m_closeBtn = buttonBox->addButton(tr("关闭"),
                                      QDialogButtonBox::RejectRole);
    connect(m_saveBtn, &QPushButton::clicked,
            this, &ProfileConfigDialog::onSaveClicked);
    connect(m_reloadBtn, &QPushButton::clicked,
            this, &ProfileConfigDialog::load);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    root->addWidget(buttonBox);
}

// 对应Python: _set_editing_enabled
void ProfileConfigDialog::setEditingEnabled(bool enabled)
{
    m_yamlEdit->setReadOnly(!enabled);
    m_envEdit->setReadOnly(!enabled);
    m_saveBtn->setEnabled(enabled);
    m_reloadBtn->setEnabled(enabled);
}

// 对应Python: _mono_font
QFont ProfileConfigDialog::monoFont()
{
    QFont font;
    font.setStyleHint(QFont::Monospace);
    font.setFamilies({QStringLiteral("Consolas"), QStringLiteral("Menlo"),
                      QStringLiteral("DejaVu Sans Mono"),
                      QStringLiteral("Courier New"),
                      QStringLiteral("monospace")});
    font.setFixedPitch(true);
    font.setPointSize(15);
    return font;
}

// 对应Python: ProfileConfigDialog._load + ProfileConfigLoader.run
void ProfileConfigDialog::load()
{
    if (!m_backend) {
        m_statusLabel->setText(tr("未连接后端"));
        return;
    }
    if (m_busy)
        return;
    m_busy = true;
    setEditingEnabled(false);
    m_statusLabel->setText(tr("加载中..."));

    HermesBackend *backend = m_backend;
    const QString configPath = m_configPath;
    const QString envPath = m_envPath;
    QPointer<ProfileConfigDialog> guard(this);
    m_task = QtConcurrent::run([guard, backend, configPath, envPath]() {
        // 阻塞读取必须在线程池线程上跑(远程模式走 SSH)
        const QString configText = backend->readFile(configPath);
        QString envText;
        // .env 可能不存在,读取失败不应中断整体加载
        if (backend->fileExists(envPath))
            envText = backend->readFile(envPath);
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard, [guard, configText, envText]() {
            if (guard)
                guard->onLoaded(configText, envText);
        }, Qt::QueuedConnection);
    });
}

// 对应Python: _on_loaded
void ProfileConfigDialog::onLoaded(const QString &configText,
                                   const QString &envText)
{
    m_busy = false;
    m_loadedConfig = configText;
    m_loadedEnv = envText;
    m_envExisted = !envText.isEmpty();
    m_yamlEdit->setPlainText(configText);
    m_envEdit->setPlainText(envText);
    setEditingEnabled(true);
    m_statusLabel->setText(configText.isEmpty()
                               ? tr("未找到 config.yaml，保存后将新建")
                               : tr("就绪"));
}

// 对应Python: ProfileConfigDialog._on_save + ProfileConfigSaver.run
void ProfileConfigDialog::onSaveClicked()
{
    if (!m_backend) {
        QMessageBox::warning(this, tr("警告"), tr("未连接后端"));
        return;
    }
    if (m_busy)
        return;

    const QString configText = m_yamlEdit->toPlainText();
    const QString envText = m_envEdit->toPlainText();

    // 收集有改动的文件
    QVector<PendingWrite> writes;
    if (configText != m_loadedConfig) {
        // YAML 语法校验:解析失败拒绝写入,避免写坏配置
        QString yamlError;
        if (!validateYaml(configText, &yamlError)) {
            m_tabs->setCurrentIndex(0);
            QMessageBox::critical(
                this, tr("YAML 语法错误"),
                tr("config.yaml 无法保存，请修正后重试：\n%1").arg(yamlError));
            return;
        }
        writes.append({m_configPath, configText, QStringLiteral("config.yaml")});
    }
    // .env:原本存在或用户填了内容才写,避免为一个空文件平白创建 .env
    if (envText != m_loadedEnv && (!envText.trimmed().isEmpty() || m_envExisted))
        writes.append({m_envPath, envText, QStringLiteral(".env")});

    if (writes.isEmpty()) {
        QMessageBox::information(this, tr("提示"), tr("没有需要保存的改动"));
        return;
    }

    m_busy = true;
    setEditingEnabled(false);
    m_statusLabel->setText(tr("保存中..."));

    HermesBackend *backend = m_backend;
    QPointer<ProfileConfigDialog> guard(this);
    m_task = QtConcurrent::run([guard, backend, writes]() {
        QStringList saved;
        QString error;
        for (const PendingWrite &w : writes) {
            if (!backend->writeFile(w.path, w.content)) {
                error = QObject::tr("写入 %1 失败").arg(w.path);
                break;
            }
            saved.append(w.label);
        }
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard, [guard, saved, error]() {
            if (guard)
                guard->onSaved(saved, error);
        }, Qt::QueuedConnection);
    });
}

// 对应Python: _on_saved / _on_save_error
void ProfileConfigDialog::onSaved(const QStringList &savedLabels,
                                  const QString &error)
{
    m_busy = false;
    setEditingEnabled(true);
    if (!error.isEmpty()) {
        m_statusLabel->setText(tr("保存失败: %1").arg(error));
        QMessageBox::critical(this, tr("错误"), tr("保存失败: %1").arg(error));
        return;
    }
    // 更新基准值,避免重复写入
    m_loadedConfig = m_yamlEdit->toPlainText();
    m_loadedEnv = m_envEdit->toPlainText();
    if (!m_loadedEnv.trimmed().isEmpty())
        m_envExisted = true;
    const QString labels = savedLabels.join(QStringLiteral("、"));
    m_statusLabel->setText(tr("已保存: %1").arg(labels));
    QMessageBox::information(this, tr("成功"), tr("已保存: %1").arg(labels));
}

// 对应Python: yaml.safe_load 的语法校验用法
bool ProfileConfigDialog::validateYaml(const QString &text, QString *errorOut)
{
    try {
        YAML::Load(text.toStdString());
    } catch (const YAML::Exception &e) {
        if (errorOut)
            *errorOut = QString::fromUtf8(e.what());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// HermesAgentWidget
// ---------------------------------------------------------------------------

HermesAgentWidget::HermesAgentWidget(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
}

HermesAgentWidget::~HermesAgentWidget()
{
    for (QFuture<void> &f : m_futures)
        f.waitForFinished();
}

// 对应Python: AgentWidget.set_backend
void HermesAgentWidget::setBackend(HermesBackend *backend)
{
    m_backend = backend;
}

// 线程池投递,顺带回收已完成的 future。
// 对应Python: self._workers.append(worker) + finished 时移除
void HermesAgentWidget::schedule(std::function<void()> job)
{
    for (int i = m_futures.size() - 1; i >= 0; --i) {
        if (m_futures.at(i).isFinished())
            m_futures.removeAt(i);
    }
    m_futures.append(QtConcurrent::run(std::move(job)));
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

// 对应Python: AgentWidget._init_ui
void HermesAgentWidget::buildUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    // ─── 工具栏 ───
    auto *toolbarLayout = new QHBoxLayout();
    toolbarLayout->setSpacing(6);

    m_btnRefresh = new QPushButton(tr("刷新"), this);
    connect(m_btnRefresh, &QPushButton::clicked, this, &HermesAgentWidget::refresh);
    toolbarLayout->addWidget(m_btnRefresh);

    m_btnCreate = new QPushButton(tr("新建 Profile"), this);
    connect(m_btnCreate, &QPushButton::clicked,
            this, &HermesAgentWidget::onCreateClicked);
    toolbarLayout->addWidget(m_btnCreate);

    m_btnRename = new QPushButton(tr("重命名"), this);
    connect(m_btnRename, &QPushButton::clicked,
            this, &HermesAgentWidget::onRenameClicked);
    toolbarLayout->addWidget(m_btnRename);

    m_btnDelete = new QPushButton(tr("删除"), this);
    connect(m_btnDelete, &QPushButton::clicked,
            this, &HermesAgentWidget::onDeleteClicked);
    toolbarLayout->addWidget(m_btnDelete);

    toolbarLayout->addStretch();
    mainLayout->addLayout(toolbarLayout);

    // ─── 左右分割面板 ───
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    mainLayout->addWidget(splitter);

    // 左侧:搜索框 + Profile 列表
    auto *leftWidget = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(leftWidget);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(4);

    m_searchInput = new QLineEdit(leftWidget);
    m_searchInput->setPlaceholderText(tr("搜索 Profile..."));
    connect(m_searchInput, &QLineEdit::textChanged,
            this, &HermesAgentWidget::onFilterTextChanged);
    leftLayout->addWidget(m_searchInput);

    m_profileList = new QListWidget(leftWidget);
    m_profileList->setMouseTracking(true); // 委托需要 State_MouseOver
    m_profileList->setItemDelegate(new ProfileItemDelegate(m_profileList));
    m_profileList->setStyleSheet(QStringLiteral(
        "QListWidget {"
        "    background-color: #1e1e1e;"
        "    border: 1px solid #333333;"
        "    outline: none;"
        "}"
        "QListWidget::item {"
        "    border: none;"
        "    padding: 0px;"
        "}"));
    connect(m_profileList, &QListWidget::currentItemChanged,
            this, &HermesAgentWidget::onCurrentItemChanged);
    leftLayout->addWidget(m_profileList);

    splitter->addWidget(leftWidget);

    // 右侧:Profile 详情 + 操作按钮
    auto *rightWidget = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(rightWidget);
    rightLayout->setContentsMargins(8, 0, 0, 0);
    rightLayout->setSpacing(10);

    // 信息卡片
    auto *infoFrame = new QFrame(rightWidget);
    infoFrame->setFrameShape(QFrame::StyledPanel);
    auto *infoLayout = new QGridLayout(infoFrame);
    infoLayout->setContentsMargins(12, 12, 12, 12);
    infoLayout->setSpacing(8);

    infoLayout->addWidget(new QLabel(tr("名称："), infoFrame), 0, 0);
    m_lblName = new QLabel(kEmptyMark, infoFrame);
    QFont nameFont = m_lblName->font();
    nameFont.setBold(true);
    m_lblName->setFont(nameFont);
    infoLayout->addWidget(m_lblName, 0, 1);

    infoLayout->addWidget(new QLabel(tr("模型："), infoFrame), 1, 0);
    m_lblModel = new QLabel(kEmptyMark, infoFrame);
    infoLayout->addWidget(m_lblModel, 1, 1);

    infoLayout->addWidget(new QLabel(tr("网关状态："), infoFrame), 2, 0);
    m_lblGateway = new QLabel(kEmptyMark, infoFrame);
    infoLayout->addWidget(m_lblGateway, 2, 1);

    infoLayout->addWidget(new QLabel(tr("Alias："), infoFrame), 3, 0);
    m_lblAlias = new QLabel(kEmptyMark, infoFrame);
    infoLayout->addWidget(m_lblAlias, 3, 1);

    infoLayout->addWidget(new QLabel(tr("Distribution："), infoFrame), 4, 0);
    m_lblDistribution = new QLabel(kEmptyMark, infoFrame);
    infoLayout->addWidget(m_lblDistribution, 4, 1);

    infoLayout->addWidget(new QLabel(tr("状态："), infoFrame), 5, 0);
    m_lblActive = new QLabel(kEmptyMark, infoFrame);
    infoLayout->addWidget(m_lblActive, 5, 1);

    rightLayout->addWidget(infoFrame);

    // 操作按钮区域
    auto *actionsLayout = new QVBoxLayout();
    actionsLayout->setSpacing(8);

    m_btnOpenTerminal = new QPushButton(tr("在终端中打开"), rightWidget);
    m_btnOpenTerminal->setMinimumHeight(36);
    QFont openFont = m_btnOpenTerminal->font();
    openFont.setBold(true);
    m_btnOpenTerminal->setFont(openFont);
    connect(m_btnOpenTerminal, &QPushButton::clicked,
            this, &HermesAgentWidget::onOpenTerminalClicked);
    actionsLayout->addWidget(m_btnOpenTerminal);

    auto *btnRow1 = new QHBoxLayout();
    m_btnSetDefault = new QPushButton(tr("设为默认"), rightWidget);
    connect(m_btnSetDefault, &QPushButton::clicked,
            this, &HermesAgentWidget::onSetDefaultClicked);
    btnRow1->addWidget(m_btnSetDefault);

    m_btnEditConfig = new QPushButton(tr("编辑配置"), rightWidget);
    connect(m_btnEditConfig, &QPushButton::clicked,
            this, &HermesAgentWidget::onEditConfigClicked);
    btnRow1->addWidget(m_btnEditConfig);
    actionsLayout->addLayout(btnRow1);

    auto *btnRow2 = new QHBoxLayout();
    m_btnStartGateway = new QPushButton(tr("启动网关"), rightWidget);
    connect(m_btnStartGateway, &QPushButton::clicked,
            this, &HermesAgentWidget::onStartGatewayClicked);
    btnRow2->addWidget(m_btnStartGateway);

    m_btnStopGateway = new QPushButton(tr("停止网关"), rightWidget);
    connect(m_btnStopGateway, &QPushButton::clicked,
            this, &HermesAgentWidget::onStopGatewayClicked);
    btnRow2->addWidget(m_btnStopGateway);
    actionsLayout->addLayout(btnRow2);

    rightLayout->addLayout(actionsLayout);
    rightLayout->addStretch();

    // 初始无选中:右侧操作按钮全部禁用,待选中 Profile 后按状态启用
    updateActionButtons(QVariantMap());

    splitter->addWidget(rightWidget);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
}

// ---------------------------------------------------------------------------
// profile list
// ---------------------------------------------------------------------------

// 对应Python: ProfileWorker._parse_profile_list
QList<QVariantMap> HermesAgentWidget::parseProfileList(const QString &output)
{
    QList<QVariantMap> profiles;
    const QString trimmedOutput = output.trimmed();
    if (trimmedOutput.isEmpty())
        return profiles;

    static const QRegularExpression spaceRe(QStringLiteral("\\s+"));
    const QStringList lines = trimmedOutput.split(QLatin1Char('\n'));
    for (const QString &rawLine : lines) {
        QString line = rawLine.trimmed();
        // 跳过表格分隔线与表头
        if (line.isEmpty() || line.contains(QStringLiteral("─"))
            || line.startsWith(QStringLiteral("Profile")))
            continue;
        // "◆" 前缀标记当前活跃 Profile
        const bool active = line.startsWith(QStringLiteral("◆"));
        if (active)
            line = line.mid(1).trimmed();

        const QStringList parts = line.split(spaceRe, Qt::SkipEmptyParts);
        if (parts.size() < 3)
            continue;

        QVariantMap profile;
        profile.insert(kKeyName, parts.at(0));
        profile.insert(kKeyActive, active);
        profile.insert(kKeyModel, parts.at(1));
        profile.insert(kKeyGateway, parts.at(2));
        profile.insert(kKeyAlias, parts.size() > 3 ? parts.at(3) : kEmptyMark);
        profile.insert(kKeyDistribution,
                       parts.size() > 4 ? parts.at(4) : kEmptyMark);
        profiles.append(profile);
    }
    return profiles;
}

// 对应Python: AgentWidget.refresh + ProfileWorker(parse_profiles=True)
void HermesAgentWidget::refresh()
{
    if (!m_backend || m_loading)
        return;
    m_loading = true;

    HermesBackend *backend = m_backend;
    QPointer<HermesAgentWidget> guard(this);
    schedule([guard, backend]() {
        // execCli 阻塞:必须在线程池线程上跑,否则冻结 UI
        const QString output = backend->execCli(
            {QStringLiteral("profile"), QStringLiteral("list")});
        const QList<QVariantMap> profiles = parseProfileList(output);
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard, [guard, profiles]() {
            if (!guard)
                return;
            guard->m_loading = false;
            guard->m_profiles = profiles;
            guard->renderProfileList();
        }, Qt::QueuedConnection);
    });
}

// 对应Python: _render_profile_list
void HermesAgentWidget::renderProfileList()
{
    // 记住当前选中项,刷新后尽量保持选中,避免跳回第一项
    const QString selectedName = currentProfileName();
    const QString filterText = m_searchInput->text().trimmed().toLower();

    m_profileList->clear();
    int targetRow = -1;
    for (const QVariantMap &profile : m_profiles) {
        const QString name = profile.value(kKeyName).toString();
        if (!filterText.isEmpty() && !name.toLower().contains(filterText))
            continue;
        auto *item = new QListWidgetItem(name, m_profileList);
        item->setData(Qt::UserRole, name);
        item->setData(ProfileItemDelegate::kProfileRole, profile);
        if (name == selectedName)
            targetRow = m_profileList->count() - 1;
    }
    // 恢复之前的选中项;找不到(首次加载/被过滤/已删除)则回退到第一项
    if (targetRow >= 0)
        m_profileList->setCurrentRow(targetRow);
    else if (m_profileList->count() > 0)
        m_profileList->setCurrentRow(0);
    else
        clearDetail();
}

// 对应Python: _filter_profiles
void HermesAgentWidget::onFilterTextChanged()
{
    renderProfileList();
}

QVariantMap HermesAgentWidget::currentProfile() const
{
    const QListWidgetItem *item = m_profileList->currentItem();
    if (!item)
        return QVariantMap();
    return item->data(ProfileItemDelegate::kProfileRole).toMap();
}

QString HermesAgentWidget::currentProfileName() const
{
    const QListWidgetItem *item = m_profileList->currentItem();
    if (!item)
        return QString();
    return item->data(Qt::UserRole).toString();
}

// ---------------------------------------------------------------------------
// detail panel
// ---------------------------------------------------------------------------

// 对应Python: _show_profile_detail
void HermesAgentWidget::onCurrentItemChanged(QListWidgetItem *current,
                                             QListWidgetItem *previous)
{
    Q_UNUSED(previous);
    if (!current) {
        clearDetail();
        return;
    }
    const QVariantMap profile =
        current->data(ProfileItemDelegate::kProfileRole).toMap();
    if (profile.isEmpty()) {
        clearDetail();
        return;
    }
    m_lblName->setText(profile.value(kKeyName).toString());
    m_lblModel->setText(profile.value(kKeyModel).toString());
    m_lblGateway->setText(profile.value(kKeyGateway).toString());
    m_lblAlias->setText(profile.value(kKeyAlias, kEmptyMark).toString());
    m_lblDistribution->setText(
        profile.value(kKeyDistribution, kEmptyMark).toString());
    m_lblActive->setText(profile.value(kKeyActive).toBool() ? tr("当前活跃")
                                                            : tr("非活跃"));
    updateActionButtons(profile);
}

// 对应Python: _clear_detail
void HermesAgentWidget::clearDetail()
{
    m_lblName->setText(kEmptyMark);
    m_lblModel->setText(kEmptyMark);
    m_lblGateway->setText(kEmptyMark);
    m_lblAlias->setText(kEmptyMark);
    m_lblDistribution->setText(kEmptyMark);
    m_lblActive->setText(kEmptyMark);
    updateActionButtons(QVariantMap());
}

// 根据选中 Profile 的状态切换各操作按钮可用性。
// 对应Python: _update_action_buttons
//   - 在终端中打开 / 停止网关:仅网关 running 时可用
//   - 启动网关:仅网关 stopped 时可用
//   - 编辑配置:任意状态均可用(有选中即可)
//   - 设为默认 / 删除:仅当 Profile 非默认(非活跃)时可用
void HermesAgentWidget::updateActionButtons(const QVariantMap &profile)
{
    if (profile.isEmpty()) {
        // 无选中:除刷新/新建等工具栏按钮外,右侧操作按钮全部禁用
        m_btnOpenTerminal->setEnabled(false);
        m_btnStopGateway->setEnabled(false);
        m_btnStartGateway->setEnabled(false);
        m_btnEditConfig->setEnabled(false);
        m_btnSetDefault->setEnabled(false);
        m_btnDelete->setEnabled(false);
        return;
    }

    const bool isRunning = isGatewayRunning(profile);
    const bool isStopped = profile.value(kKeyGateway).toString().compare(
                               QLatin1String("stopped"), Qt::CaseInsensitive) == 0;
    const bool isDefault = profile.value(kKeyActive).toBool();

    m_btnOpenTerminal->setEnabled(isRunning);
    m_btnStopGateway->setEnabled(isRunning);
    m_btnStartGateway->setEnabled(isStopped);
    m_btnEditConfig->setEnabled(true);
    m_btnSetDefault->setEnabled(!isDefault);
    // 默认 Profile 不允许删除
    m_btnDelete->setEnabled(!isDefault);
}

// ---------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------

// 对应Python: _run_command(命令完成后统一刷新列表)
void HermesAgentWidget::runCommand(const QStringList &args)
{
    if (!m_backend)
        return;
    HermesBackend *backend = m_backend;
    QPointer<HermesAgentWidget> guard(this);
    schedule([guard, backend, args]() {
        backend->execCli(args);
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard, [guard]() {
            if (guard)
                guard->refresh();
        }, Qt::QueuedConnection);
    });
}

// 对应Python: _on_error
void HermesAgentWidget::showError(const QString &message)
{
    QMessageBox::warning(this, tr("错误"), message);
}

// 对应Python: _open_in_terminal
void HermesAgentWidget::onOpenTerminalClicked()
{
    const QString profileName = currentProfileName();
    if (profileName.isEmpty())
        return;
    emit openTerminalRequested(profileName);
}

// 对应Python: _set_as_default
void HermesAgentWidget::onSetDefaultClicked()
{
    const QString profileName = currentProfileName();
    if (profileName.isEmpty())
        return;
    runCommand({QStringLiteral("profile"), QStringLiteral("use"), profileName});
}

// 对应Python: _create_profile(克隆当前 Profile)
void HermesAgentWidget::onCreateClicked()
{
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("新建 Profile"), tr("输入 Profile 名称:"),
        QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    runCommand({QStringLiteral("profile"), QStringLiteral("create"), name,
                QStringLiteral("--clone")});
}

// 对应Python: _delete_profile
void HermesAgentWidget::onDeleteClicked()
{
    const QString profileName = currentProfileName();
    if (profileName.isEmpty())
        return;
    const QVariantMap profile = currentProfile();

    // 默认(活跃)Profile 不允许删除,避免删掉当前正在使用的配置
    if (profile.value(kKeyActive).toBool()) {
        QMessageBox::warning(
            this, tr("无法删除"),
            tr("「%1」是当前默认 Profile，请先切换到其他 Profile 再删除。")
                .arg(profileName));
        return;
    }
    // 网关运行中先停止再删,避免残留进程
    if (isGatewayRunning(profile)) {
        QMessageBox::warning(
            this, tr("无法删除"),
            tr("「%1」的网关正在运行，请先停止网关再删除。").arg(profileName));
        return;
    }
    if (QMessageBox::question(
            this, tr("确认删除"),
            tr("确定要删除 Profile「%1」吗？此操作不可撤销。").arg(profileName),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return;
    runCommand({QStringLiteral("profile"), QStringLiteral("delete"),
                profileName, QStringLiteral("-y")});
}

// 对应Python: _rename_profile
void HermesAgentWidget::onRenameClicked()
{
    const QString oldName = currentProfileName();
    if (oldName.isEmpty())
        return;
    bool ok = false;
    const QString newName = QInputDialog::getText(
        this, tr("重命名 Profile"), tr("新名称:"),
        QLineEdit::Normal, oldName, &ok).trimmed();
    if (!ok || newName.isEmpty() || newName == oldName)
        return;
    runCommand({QStringLiteral("profile"), QStringLiteral("rename"), oldName,
                newName});
}

// 对应Python: _edit_config(关闭对话框后刷新,配置可能改变模型等展示字段)
void HermesAgentWidget::onEditConfigClicked()
{
    const QString profileName = currentProfileName();
    if (profileName.isEmpty() || !m_backend)
        return;
    const QString hermesHome = m_backend->hermesHome();
    if (hermesHome.isEmpty()) {
        showError(tr("无法定位 Hermes 主目录"));
        return;
    }
    const QString profileDir =
        QStringLiteral("%1/profiles/%2").arg(hermesHome, profileName);
    ProfileConfigDialog dialog(m_backend, profileName,
                               profileDir + QStringLiteral("/config.yaml"),
                               profileDir + QStringLiteral("/.env"), this);
    dialog.exec();
    refresh();
}

// 对应Python: _start_gateway
void HermesAgentWidget::onStartGatewayClicked()
{
    const QString profileName = currentProfileName();
    if (profileName.isEmpty())
        return;
    runCommand({QStringLiteral("-p"), profileName, QStringLiteral("gateway"),
                QStringLiteral("start")});
}

// 对应Python: _stop_gateway
void HermesAgentWidget::onStopGatewayClicked()
{
    const QString profileName = currentProfileName();
    if (profileName.isEmpty())
        return;
    runCommand({QStringLiteral("-p"), profileName, QStringLiteral("gateway"),
                QStringLiteral("stop")});
}

} // namespace cubeshell

