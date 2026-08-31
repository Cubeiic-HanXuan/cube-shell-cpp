// KubeManagerDialog.cpp — see KubeManagerDialog.h.
// 对应 docs/Kubernetes功能实现方案.md §5；结构对齐 dialogs/DockerManagerDialog.cpp。

#include "KubeManagerDialog.h"

#include "KubeLogViewer.h"
#include "editors/KubeYamlEditor.h"
#include "config/GlobalState.h"

#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QCursor>
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace cubeshell {

namespace {

// 树列序：名称 / 状态 / AGE / 详情。
enum Column {
    ColName = 0,
    ColStatus,
    ColAge,
    ColDetails,
    ColCount
};

// 占位行（加载中/提示行）标记，右键菜单对这类行不弹出。
constexpr int kPlaceholderRole = Qt::UserRole + 1;
// 对象行的 KubeObjectRef（QVariant 承载）。
constexpr int kRefRole = Qt::UserRole + 2;
// 展开态记忆键：分组存组名、kind 存 apiPlural。与显示文本解耦，
// 计数变化导致文本变化时展开态仍能对上。
constexpr int kExpandKeyRole = Qt::UserRole + 3;

// 对应 Docker 对话框的 operationDisplayName。
QString operationDisplayName(const QString &op)
{
    if (op == QLatin1String("delete"))
        return QStringLiteral("删除");
    if (op == QLatin1String("scale"))
        return QStringLiteral("扩缩容");
    if (op == QLatin1String("restart"))
        return QStringLiteral("滚动重启");
    return op;
}

// ---------------------------------------------------------------------------
// 状态语义着色（Lens/k9s 式：状态列文字 + 圆点按健康度着色）
// ---------------------------------------------------------------------------

enum class StatusSeverity { Neutral, Ok, Warn, Error };

// 状态文本 → 严重度。pod/副本类是 "M/N phase"，其余是单词短语；统一小写后
// 按 Error → Warn → Ok → Neutral 顺序匹配。
StatusSeverity statusSeverity(const QString &apiPlural, const QString &status)
{
    const QString s = status.toLower();
    // events 的状态列是 type（"Warning" / "Normal"）。
    if (apiPlural == QLatin1String("events"))
        return s == QLatin1String("warning") ? StatusSeverity::Error
                                             : StatusSeverity::Neutral;

    static const char *const kErrorKeys[] = {
        "crashloopbackoff", "imagepullbackoff", "errimagepull", "error",
        "failed",           "notready",         "oomkilled",    "evicted",
        "unknown",
    };
    for (const char *key : kErrorKeys) {
        if (s.contains(QLatin1String(key)))
            return StatusSeverity::Error;
    }

    static const char *const kWarnKeys[] = {
        "pending",     "containercreating", "terminating",
        "suspended",   "podinitializing",
    };
    for (const char *key : kWarnKeys) {
        if (s.contains(QLatin1String(key)))
            return StatusSeverity::Warn;
    }

    // 终态词优先于就绪比：完成的 Job Pod 是 "0/1 Succeeded"（ready=0），
    // 不能按 0/N 误判为 Error。
    static const char *const kTerminalOkKeys[] = {
        "succeeded", "complete",
    };
    for (const char *key : kTerminalOkKeys) {
        if (s.contains(QLatin1String(key)))
            return StatusSeverity::Ok;
    }

    // "M/N ..." 就绪比：全就绪 Ok；0/N Error；中间态 Warn。
    const int slash = s.indexOf(QLatin1Char('/'));
    if (slash > 0) {
        bool okReady = false;
        bool okTotal = false;
        const int ready = s.left(slash).trimmed().toInt(&okReady);
        // total 后可能跟相位词（"1/1 Running"），取空白前缀。
        const int total =
            s.mid(slash + 1).section(QLatin1Char(' '), 0, 0).toInt(&okTotal);
        if (okReady && okTotal && total > 0) {
            if (ready >= total)
                return StatusSeverity::Ok;
            return ready == 0 ? StatusSeverity::Error : StatusSeverity::Warn;
        }
    }

    static const char *const kOkKeys[] = {
        "running", "ready",   "bound",   "active", "available",
    };
    for (const char *key : kOkKeys) {
        if (s.contains(QLatin1String(key)))
            return StatusSeverity::Ok;
    }
    return StatusSeverity::Neutral;
}

// 深色用亮色、浅色用深色，保证两种主题下都可读不刺眼。
QColor severityColor(StatusSeverity severity, bool darkTheme)
{
    switch (severity) {
    case StatusSeverity::Ok:
        return darkTheme ? QColor(0x4C, 0xAF, 0x50) : QColor(0x2E, 0x7D, 0x32);
    case StatusSeverity::Warn:
        return darkTheme ? QColor(0xFF, 0xA7, 0x26) : QColor(0xE6, 0x51, 0x00);
    case StatusSeverity::Error:
        return darkTheme ? QColor(0xEF, 0x53, 0x50) : QColor(0xC6, 0x28, 0x28);
    default:
        return QColor();
    }
}

bool isDarkTheme()
{
    return GlobalState::instance().appearance().trimmed().compare(
               QLatin1String("light"), Qt::CaseInsensitive) != 0;
}

// 状态列前缀的实心圆点。运行时 QPixmap 自绘（不新增图标文件，规避鸿蒙运行
// 资源必须走 qrc 的约束）；按 (severity, 主题) 缓存，避免每行重建。
QIcon statusDotIcon(StatusSeverity severity, bool darkTheme)
{
    static QHash<int, QIcon> cache;
    const int key = (int(severity) << 1) | (darkTheme ? 1 : 0);
    const auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return it.value();

    // 超采样抗锯齿：按 kScale 倍分辨率画，再把 devicePixelRatio 设回 kScale。
    // 否则 10×10 物理像素在高分屏（DPR=2）被当逻辑像素放大 2 倍，边缘发虚。
    constexpr int logical = 10;
    constexpr qreal kScale = 4.0;
    const int px = int(logical * kScale);
    QPixmap pm(px, px);
    pm.setDevicePixelRatio(kScale);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(severityColor(severity, darkTheme));
    // 逻辑坐标系（QPainter 已按 DPR 缩放），圆点居中、留 2px 边距。
    p.drawEllipse(QRectF(2, 2, logical - 4, logical - 4));
    p.end();
    const QIcon icon(pm);
    cache.insert(key, icon);
    return icon;
}

} // namespace

// ---------------------------------------------------------------------------
// 端口转发管理小对话框（仅本文件内使用）
// ---------------------------------------------------------------------------

class KubePortForwardDialog : public QDialog {
public:
    KubePortForwardDialog(KubeManager *manager, QWidget *parent = nullptr)
        : QDialog(parent)
        , m_manager(manager)
    {
        setWindowTitle(tr("端口转发"));
        setMinimumSize(560, 320);
        setModal(false);

        auto *layout = new QVBoxLayout(this);
        m_list = new QTreeWidget(this);
        m_list->setColumnCount(5);
        m_list->setHeaderLabels({tr("目标"), tr("命名空间"), tr("本地端口"),
                                 tr("目标端口"), tr("操作")});
        m_list->setRootIsDecorated(false);
        layout->addWidget(m_list);

        auto *newBox = new QHBoxLayout();
        m_targetLabel = new QLabel(this);
        m_localSpin = new QSpinBox(this);
        m_localSpin->setRange(1, 65535);
        m_localSpin->setValue(8080);
        m_remoteSpin = new QSpinBox(this);
        m_remoteSpin->setRange(1, 65535);
        m_remoteSpin->setValue(80);
        auto *createBtn = new QPushButton(tr("创建转发"), this);
        newBox->addWidget(m_targetLabel);
        newBox->addStretch();
        newBox->addWidget(new QLabel(tr("本地端口:"), this));
        newBox->addWidget(m_localSpin);
        newBox->addWidget(new QLabel(tr("目标端口:"), this));
        newBox->addWidget(m_remoteSpin);
        newBox->addWidget(createBtn);
        layout->addLayout(newBox);

        auto *closeBtn = new QPushButton(tr("关闭"), this);
        auto *bottom = new QHBoxLayout();
        bottom->addStretch();
        bottom->addWidget(closeBtn);
        layout->addLayout(bottom);

        connect(createBtn, &QPushButton::clicked, this, [this]() {
            if (!m_hasTarget)
                return;
            m_manager->startPortForward(m_target,
                                        quint16(m_localSpin->value()),
                                        quint16(m_remoteSpin->value()));
        });
        connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
        connect(m_manager, &KubeManager::portForwardsChanged,
                this, &KubePortForwardDialog::refreshList, Qt::QueuedConnection);
        refreshList();
    }

    void setTarget(const KubeObjectRef &ref)
    {
        m_target = ref;
        m_hasTarget = true;
        m_targetLabel->setText(tr("新转发: %1/%2").arg(ref.apiPlural, ref.name));
    }

    void refreshList()
    {
        m_list->clear();
        const QList<KubePortForwardInfo> forwards = m_manager->portForwards();
        for (const KubePortForwardInfo &info : forwards) {
            auto *item = new QTreeWidgetItem(m_list);
            item->setText(0, info.targetText);
            item->setText(1, info.namespace_);
            item->setText(2, QString::number(info.localPort));
            item->setText(3, QString::number(info.remotePort));
            auto *stopBtn = new QPushButton(tr("停止"), m_list);
            const int id = info.id;
            connect(stopBtn, &QPushButton::clicked, this, [this, id]() {
                m_manager->stopPortForward(id);
            });
            m_list->setItemWidget(item, 4, stopBtn);
        }
        if (forwards.isEmpty()) {
            auto *item = new QTreeWidgetItem(m_list);
            item->setText(0, tr("（没有活动转发；对话框关闭不会停止已有转发）"));
            item->setFirstColumnSpanned(true);
        }
    }

private:
    KubeManager *m_manager; // not owned
    QTreeWidget *m_list = nullptr;
    QLabel *m_targetLabel = nullptr;
    QSpinBox *m_localSpin = nullptr;
    QSpinBox *m_remoteSpin = nullptr;
    KubeObjectRef m_target;
    bool m_hasTarget = false;
};

// ---------------------------------------------------------------------------
// KubeManagerDialog
// ---------------------------------------------------------------------------

KubeManagerDialog::KubeManagerDialog(KubeManager *manager, QWidget *parent)
    : QDialog(parent)
    , m_manager(manager)
{
    setWindowTitle(tr("Kubernetes 集群管理"));
    setMinimumSize(960, 560);
    setModal(false);

    auto *layout = new QVBoxLayout(this);

    // --- 顶部工具条：后端 / 上下文 / 命名空间 / 刷新 ---
    auto *topBar = new QHBoxLayout();
    m_backendCombo = new QComboBox(this);
    m_backendCombo->addItem(tr("本机 kubectl"));
    // 远程项文本较长（"当前 SSH 会话 (user@host)"）：按内容自适应 + 最小宽度
    // 兜底，否则被同排下拉挤压、当前值显示成省略号。
    m_backendCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_backendCombo->setMinimumWidth(200);
    m_contextCombo = new QComboBox(this);
    m_contextCombo->setMinimumWidth(180);
    m_namespaceCombo = new QComboBox(this);
    m_namespaceCombo->setMinimumWidth(140);
    m_namespaceCombo->setEditable(true);
    auto *refreshBtn = new QPushButton(tr("刷新"), this);
    m_kubeconfigBtn = new QPushButton(tr("kubeconfig…"), this);
    topBar->addWidget(new QLabel(tr("后端:"), this));
    topBar->addWidget(m_backendCombo);
    topBar->addWidget(new QLabel(tr("上下文:"), this));
    topBar->addWidget(m_contextCombo);
    topBar->addWidget(new QLabel(tr("命名空间:"), this));
    topBar->addWidget(m_namespaceCombo);
    topBar->addWidget(refreshBtn);
    topBar->addWidget(m_kubeconfigBtn);
    topBar->addStretch();
    layout->addLayout(topBar);

    // --- 资源树 ---
    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("treeWidgetKube"));
    m_tree->setColumnCount(ColCount);
    m_tree->setHeaderLabels({tr("名称"), tr("状态"), tr("AGE"), tr("详情")});
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    QHeaderView *header = m_tree->header();
    header->setSectionResizeMode(ColName, QHeaderView::Interactive);
    header->setSectionResizeMode(ColDetails, QHeaderView::Stretch);
    m_tree->setColumnWidth(ColName, 320);
    m_tree->setColumnWidth(ColStatus, 160);
    // 轻微留白提升可读性（局部 QSS 只作用本树，不影响全局 qdarktheme）。
    m_tree->setStyleSheet(QStringLiteral("QTreeWidget::item { padding: 2px 2px; }"));
    connect(m_tree, &QTreeWidget::customContextMenuRequested,
            this, &KubeManagerDialog::onTreeContextMenu);
    layout->addWidget(m_tree);

    // --- 底部：状态信息 + 快捷操作 ---
    auto *bottomBar = new QHBoxLayout();
    m_statusLabel = new QLabel(this);
    auto *logsBtn = new QPushButton(tr("日志"), this);
    auto *yamlBtn = new QPushButton(tr("YAML"), this);
    auto *terminalBtn = new QPushButton(tr("终端"), this);
    auto *forwardBtn = new QPushButton(tr("端口转发"), this);
    bottomBar->addWidget(m_statusLabel);
    bottomBar->addStretch();
    bottomBar->addWidget(logsBtn);
    bottomBar->addWidget(yamlBtn);
    bottomBar->addWidget(terminalBtn);
    bottomBar->addWidget(forwardBtn);
    layout->addLayout(bottomBar);

    connect(refreshBtn, &QPushButton::clicked, this, &KubeManagerDialog::refreshInfo);
    connect(refreshBtn, &QPushButton::clicked, this, [this]() {
        m_statusLabel->clear();
    });
    connect(m_kubeconfigBtn, &QPushButton::clicked,
            this, &KubeManagerDialog::onKubeconfigButtonClicked);

    // 后端切换只发信号：executor 更换在主窗口，换完会回调 refreshInfo()。
    connect(m_backendCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int index) {
                emit backendChangeRequested(index == 1);
            });
    // 上下文/命名空间用 activated（仅用户操作触发），程序化填充时 blockSignals。
    connect(m_contextCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int) {
                m_manager->switchContext(m_contextCombo->currentText());
            });
    connect(m_namespaceCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int) {
                const QString ns = m_namespaceCombo->currentText().trimmed();
                if (ns.isEmpty())
                    return;
                m_manager->setNamespace(ns);
                m_manager->refreshAll();
            });

    // 底部快捷操作 = 右键菜单的平铺项，作用于当前选中行。
    connect(logsBtn, &QPushButton::clicked, this, [this]() {
        KubeObjectRef ref;
        if (selectedRef(&ref) && ref.apiPlural == QLatin1String("pods"))
            showLogsForPod(ref, QString());
        else
            m_statusLabel->setText(tr("请先选中一个 Pod。"));
    });
    connect(yamlBtn, &QPushButton::clicked, this, [this]() {
        KubeObjectRef ref;
        if (!selectedRef(&ref)) {
            m_statusLabel->setText(tr("请先选中一个资源对象。"));
            return;
        }
        m_yamlEditRequested = false;
        m_manager->fetchYaml(ref);
    });
    connect(terminalBtn, &QPushButton::clicked, this, [this]() {
        KubeObjectRef ref;
        if (selectedRef(&ref) && ref.apiPlural == QLatin1String("pods"))
            execIntoPod(ref, QString());
        else
            m_statusLabel->setText(tr("请先选中一个 Pod。"));
    });
    connect(forwardBtn, &QPushButton::clicked, this, [this]() {
        KubeObjectRef ref;
        if (selectedRef(&ref) && (ref.apiPlural == QLatin1String("pods")
                                  || ref.apiPlural == QLatin1String("services")))
            showPortForwardDialog(ref);
        else
            showPortForwardDialog(KubeObjectRef()); // 仅管理已有转发
    });

    // Manager 信号来自工作线程 —— 显式 QueuedConnection（Docker 对话框同款纪律）。
    connect(m_manager, &KubeManager::availabilityReady,
            this, &KubeManagerDialog::onAvailabilityReady, Qt::QueuedConnection);
    connect(m_manager, &KubeManager::contextsUpdated,
            this, &KubeManagerDialog::onContextsUpdated, Qt::QueuedConnection);
    connect(m_manager, &KubeManager::namespacesUpdated,
            this, &KubeManagerDialog::onNamespacesUpdated, Qt::QueuedConnection);
    connect(m_manager, &KubeManager::resourcesUpdated,
            this, &KubeManagerDialog::onResourcesUpdated, Qt::QueuedConnection);
    connect(m_manager, &KubeManager::refreshFinished,
            this, &KubeManagerDialog::onRefreshFinished, Qt::QueuedConnection);
    connect(m_manager, &KubeManager::yamlReady,
            this, &KubeManagerDialog::onYamlReady, Qt::QueuedConnection);
    connect(m_manager, &KubeManager::applyFinished,
            this, &KubeManagerDialog::onApplyFinished, Qt::QueuedConnection);
    connect(m_manager, &KubeManager::operationFinished,
            this, &KubeManagerDialog::onOperationFinished, Qt::QueuedConnection);
    connect(m_manager, &KubeManager::textReady,
            this, &KubeManagerDialog::onTextReady, Qt::QueuedConnection);
    connect(m_manager, &KubeManager::errorOccurred,
            this, &KubeManagerDialog::onErrorOccurred, Qt::QueuedConnection);
    connect(m_manager, &KubeManager::logStarted,
            this, [this](const QString &title) { logViewer()->beginStream(title); },
            Qt::QueuedConnection);
    connect(m_manager, &KubeManager::logChunk,
            this, [this](const QString &text) { logViewer()->appendChunk(text); },
            Qt::QueuedConnection);
    connect(m_manager, &KubeManager::logsFinished,
            this, [this](int exitCode) { logViewer()->finishStream(exitCode); },
            Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// 后端下拉
// ---------------------------------------------------------------------------

void KubeManagerDialog::setRemoteBackendAvailable(bool available, const QString &label)
{
    m_backendLabelRemote = label;
    const bool hasRemoteItem = m_backendCombo->count() > 1;
    if (available && !hasRemoteItem)
        m_backendCombo->addItem(label);
    else if (available && hasRemoteItem)
        m_backendCombo->setItemText(1, label);
    else if (!available && hasRemoteItem) {
        // 会话断开：选中项被移除前主窗口已把后端切回本机。
        m_backendCombo->removeItem(1);
    }
}

void KubeManagerDialog::setBackendSelection(bool useRemote)
{
    const int index = (useRemote && m_backendCombo->count() > 1) ? 1 : 0;
    m_backendIsRemote = (index == 1);
    m_backendCombo->blockSignals(true);
    m_backendCombo->setCurrentIndex(index);
    m_backendCombo->blockSignals(false);
    // 后端变了，kubeconfig 提示随之切换为本机/远程语义。
    setKubeconfigPathDisplay(m_manager->kubeconfigPath());
}

// kubeconfig 按钮菜单：本机后端走文件对话框；远程后端的路径在远端服务器上，
// 不能用本机文件框，改用文本输入（留空即默认 ~/.kube/config）。
// 两种后端都提供「恢复默认解析」清除项。
void KubeManagerDialog::onKubeconfigButtonClicked()
{
    QMenu menu(this);
    QAction *chooseAction = menu.addAction(
        m_backendIsRemote ? tr("输入远程 kubeconfig 路径…")
                          : tr("选择本机 kubeconfig 文件…"));
    QAction *clearAction = menu.addAction(tr("恢复默认解析（清除自定义路径）"));
    clearAction->setEnabled(!m_manager->kubeconfigPath().isEmpty());

    QAction *picked = menu.exec(QCursor::pos());
    if (picked == chooseAction) {
        if (m_backendIsRemote) {
            bool ok = false;
            const QString path = QInputDialog::getText(
                this, tr("远程 kubeconfig"),
                tr("远程服务器上的 kubeconfig 绝对路径：\n（留空 = 默认 ~/.kube/config）"),
                QLineEdit::Normal, m_manager->kubeconfigPath(), &ok);
            if (ok)
                emit kubeconfigSelected(path.trimmed()); // 空串即恢复默认
        } else {
            const QString path = QFileDialog::getOpenFileName(
                this, tr("选择 kubeconfig 文件"), QDir::homePath());
            if (!path.isEmpty())
                emit kubeconfigSelected(path);
        }
    } else if (picked == clearAction) {
        emit kubeconfigSelected(QString()); // 空 = 恢复默认解析
    }
}

void KubeManagerDialog::setKubeconfigPathDisplay(const QString &path)
{
    // 本机/远程的默认解析位置不同：本机是 KUBECONFIG 或 ~/.kube/config；
    // 远程是远端登录用户家目录下的 ~/.kube/config。
    const QString defaultDesc = m_backendIsRemote
        ? tr("远程默认解析（远端 ~/.kube/config）")
        : tr("本机默认解析（KUBECONFIG / ~/.kube/config）");
    m_kubeconfigBtn->setToolTip(path.isEmpty()
        ? tr("当前使用 %1").arg(defaultDesc)
        : tr("当前 %1 kubeconfig: %2")
              .arg(m_backendIsRemote ? tr("远程") : tr("本机"), path));
}

// ---------------------------------------------------------------------------
// 刷新流程
// ---------------------------------------------------------------------------

void KubeManagerDialog::refreshInfo()
{
    // 对应 Docker 对话框的 m_refreshing 守卫：刷新中忽略并发刷新。
    if (m_refreshing)
        return;
    m_refreshing = true;
    showLoadingRow();
    // 探测结果驱动后续：可用 → 上下文/命名空间/全量；不可用 → 占位行。
    m_manager->checkAvailability();
}

void KubeManagerDialog::showPlaceholderRow(const QString &text)
{
    m_tree->clear();
    auto *item = new QTreeWidgetItem();
    item->setText(ColName, text);
    item->setData(ColName, kPlaceholderRole, true);
    m_tree->addTopLevelItem(item);
}

void KubeManagerDialog::showLoadingRow()
{
    showPlaceholderRow(tr("正在加载 Kubernetes 资源..."));
}

void KubeManagerDialog::onAvailabilityReady(bool available, const QString &versionText)
{
    if (!available) {
        m_refreshing = false;
        m_rowsByKind.clear();
        const QString backend = m_manager->isRemote()
            ? tr("远端 SSH 主机") : tr("本机");
        showPlaceholderRow(tr("%1未检测到 kubectl：%2\n"
                              "请安装 kubectl，或切换其他后端后重试。")
                               .arg(backend, versionText));
        return;
    }
    m_statusLabel->setText(tr("kubectl %1").arg(versionText));
    m_manager->refreshContexts();
    m_manager->refreshNamespaces();
    m_manager->refreshAll();
}

void KubeManagerDialog::onContextsUpdated(const QList<cubeshell::KubeContextInfo> &contexts)
{
    m_contextCombo->blockSignals(true);
    m_contextCombo->clear();
    for (const KubeContextInfo &info : contexts)
        m_contextCombo->addItem(info.name);
    const QString current = m_manager->currentContext();
    const int idx = m_contextCombo->findText(current);
    if (idx >= 0)
        m_contextCombo->setCurrentIndex(idx);
    m_contextCombo->blockSignals(false);
}

void KubeManagerDialog::onNamespacesUpdated(const QStringList &namespaces)
{
    m_namespaceCombo->blockSignals(true);
    const QString previous = m_namespaceCombo->currentText();
    m_namespaceCombo->clear();
    m_namespaceCombo->addItems(namespaces);
    const QString current = m_manager->currentNamespace();
    // 当前命名空间不在列表里（新上下文未选过）时补进去，避免选择丢失。
    if (!current.isEmpty() && m_namespaceCombo->findText(current) < 0)
        m_namespaceCombo->addItem(current);
    int idx = m_namespaceCombo->findText(current);
    if (idx < 0)
        idx = m_namespaceCombo->findText(previous);
    if (idx >= 0)
        m_namespaceCombo->setCurrentIndex(idx);
    m_namespaceCombo->blockSignals(false);
}

// ---------------------------------------------------------------------------
// 资源树
// ---------------------------------------------------------------------------

void KubeManagerDialog::onResourcesUpdated(const QString &group, const QString &apiPlural,
                                           const QList<cubeshell::KubeResourceRow> &rows)
{
    Q_UNUSED(group);
    m_rowsByKind.insert(apiPlural, rows);
    // 首条数据到达时清掉"正在加载"占位行。
    rebuildTree();
}

void KubeManagerDialog::onRefreshFinished()
{
    m_refreshing = false;
    if (m_rowsByKind.isEmpty())
        showPlaceholderRow(tr("没有可用的 Kubernetes 资源"));
}

QString KubeManagerDialog::detailsText(const QString &apiPlural,
                                       const cubeshell::KubeResourceRow &row)
{
    // Events：reason + object + message(xN) 更可读，不走 key=value 平铺。
    if (apiPlural == QLatin1String("events")) {
        QString text = QStringLiteral("%1 %2 %3")
                           .arg(row.extra.value(QStringLiteral("reason")),
                                row.extra.value(QStringLiteral("object")),
                                row.extra.value(QStringLiteral("message")));
        const QString count = row.extra.value(QStringLiteral("count"));
        if (!count.isEmpty() && count != QLatin1String("1"))
            text += QStringLiteral(" (x%1)").arg(count);
        return text.trimmed();
    }
    QStringList parts;
    const QList<QString> keys = row.extra.keys();
    QList<QString> sorted = keys;
    std::sort(sorted.begin(), sorted.end());
    for (const QString &key : sorted) {
        const QString value = row.extra.value(key);
        if (!value.isEmpty())
            parts.append(QStringLiteral("%1=%2").arg(key, value));
    }
    return parts.join(QStringLiteral("  "));
}

void KubeManagerDialog::rebuildTree()
{
    // 保留用户展开态：重建前记录展开的分组/类型节点。键取自 kExpandKeyRole
    //（组名 / apiPlural），与显示文本解耦——计数变化不改键。
    QSet<QString> expanded;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *groupItem = m_tree->topLevelItem(i);
        const QString groupKey = groupItem->data(ColName, kExpandKeyRole).toString();
        if (groupItem->isExpanded())
            expanded.insert(groupKey);
        for (int j = 0; j < groupItem->childCount(); ++j) {
            QTreeWidgetItem *kindItem = groupItem->child(j);
            if (kindItem->isExpanded())
                expanded.insert(groupKey + QLatin1Char('/')
                                + kindItem->data(ColName, kExpandKeyRole).toString());
        }
    }

    m_tree->clear();
    const QFont boldFont = [this]() {
        QFont f = m_tree->font();
        f.setBold(true);
        return f;
    }();
    // 空类型置灰：用主题 Disabled 文本色，深浅主题自动适配。
    const QBrush disabledBrush(
        m_tree->palette().color(QPalette::Disabled, QPalette::Text));

    const QStringList groups = KubeResourceParser::groupOrder();
    for (const QString &group : groups) {
        const QList<KubeResourceKind> kinds = KubeResourceParser::kindsInGroup(group);
        int groupTotal = 0;
        for (const KubeResourceKind &kind : kinds)
            groupTotal += m_rowsByKind.value(kind.apiPlural).size();

        auto *groupItem = new QTreeWidgetItem();
        groupItem->setText(ColName, QStringLiteral("%1 (%2)").arg(group).arg(groupTotal));
        groupItem->setFont(ColName, boldFont);
        groupItem->setData(ColName, kExpandKeyRole, group);
        m_tree->addTopLevelItem(groupItem);

        for (const KubeResourceKind &kind : kinds) {
            const QList<KubeResourceRow> rows = m_rowsByKind.value(kind.apiPlural);
            auto *kindItem = new QTreeWidgetItem();
            kindItem->setText(ColName, QStringLiteral("%1 (%2)")
                                           .arg(kind.displayName)
                                           .arg(rows.size()));
            kindItem->setData(ColName, kExpandKeyRole, kind.apiPlural);
            // 空类型置灰（保留结构可见性，但视觉降权）。
            if (rows.isEmpty())
                kindItem->setForeground(ColName, disabledBrush);
            groupItem->addChild(kindItem);
            for (const KubeResourceRow &row : rows)
                addRowItem(row, kind.apiPlural, kindItem);
        }
    }

    if (!m_firstBuildDone) {
        // 首轮：只展开有数据的类型（及其分组）；空类型折叠，避免满屏铺开。
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *groupItem = m_tree->topLevelItem(i);
            bool groupHasRows = false;
            for (int j = 0; j < groupItem->childCount(); ++j) {
                QTreeWidgetItem *kindItem = groupItem->child(j);
                const bool hasRows = kindItem->childCount() > 0;
                kindItem->setExpanded(hasRows);
                groupHasRows = groupHasRows || hasRows;
            }
            groupItem->setExpanded(groupHasRows);
        }
        m_firstBuildDone = true;
    } else {
        // 之后恢复用户展开态。
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *groupItem = m_tree->topLevelItem(i);
            const QString groupKey = groupItem->data(ColName, kExpandKeyRole).toString();
            groupItem->setExpanded(expanded.contains(groupKey));
            for (int j = 0; j < groupItem->childCount(); ++j) {
                QTreeWidgetItem *kindItem = groupItem->child(j);
                const QString key = groupKey + QLatin1Char('/')
                                    + kindItem->data(ColName, kExpandKeyRole).toString();
                kindItem->setExpanded(expanded.contains(key));
            }
        }
    }
}

void KubeManagerDialog::addRowItem(const cubeshell::KubeResourceRow &row,
                                   const QString &apiPlural, QTreeWidgetItem *parentItem)
{
    auto *item = new QTreeWidgetItem();
    item->setText(ColName, row.name);
    item->setText(ColStatus, row.status);
    item->setText(ColAge, row.age);
    item->setText(ColDetails, detailsText(apiPlural, row));
    item->setToolTip(ColDetails, detailsText(apiPlural, row));

    // 状态列语义着色 + 圆点；Neutral（如 Service 的 ClusterIP）保持主题默认。
    const StatusSeverity severity = statusSeverity(apiPlural, row.status);
    if (severity != StatusSeverity::Neutral) {
        const bool dark = isDarkTheme();
        item->setForeground(ColStatus, QBrush(severityColor(severity, dark)));
        item->setIcon(ColStatus, statusDotIcon(severity, dark));
    }

    KubeObjectRef ref;
    ref.apiPlural = apiPlural;
    ref.name = row.name;
    ref.namespace_ = row.namespace_;
    item->setData(ColName, kRefRole, QVariant::fromValue(ref));
    // Pod 容器列表带上，右键"日志/终端"子菜单直接用（免去 exec 前再查一次）。
    item->setData(ColStatus, Qt::UserRole, row.containers);
    parentItem->addChild(item);
}

bool KubeManagerDialog::selectedRef(cubeshell::KubeObjectRef *refOut) const
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item)
        return false;
    const QVariant data = item->data(ColName, kRefRole);
    if (!data.isValid())
        return false;
    if (refOut)
        *refOut = data.value<KubeObjectRef>();
    return true;
}

// ---------------------------------------------------------------------------
// 右键菜单（按 kind 动态构建）
// ---------------------------------------------------------------------------

void KubeManagerDialog::onTreeContextMenu(const QPoint &position)
{
    QTreeWidgetItem *item = m_tree->itemAt(position);
    if (!item || item->data(ColName, kPlaceholderRole).toBool())
        return;
    const QVariant data = item->data(ColName, kRefRole);
    if (!data.isValid())
        return; // 分组/类型节点不弹菜单
    const KubeObjectRef ref = data.value<KubeObjectRef>();
    const KubeResourceKind *kind = KubeResourceParser::findKind(ref.apiPlural);
    if (!kind)
        return;

    auto *menu = new QMenu(this);
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);

    QAction *viewYaml = menu->addAction(tr("查看 YAML"));
    QAction *editYaml = menu->addAction(tr("编辑并应用 YAML"));
    QAction *describe = menu->addAction(tr("描述 (describe)"));
    menu->addSeparator();

    connect(viewYaml, &QAction::triggered, this, [this, ref]() {
        m_yamlEditRequested = false;
        m_manager->fetchYaml(ref);
    });
    connect(editYaml, &QAction::triggered, this, [this, ref]() {
        m_yamlEditRequested = true;
        m_manager->fetchYaml(ref);
    });
    connect(describe, &QAction::triggered, this, [this, ref]() {
        m_manager->fetchDescribe(ref);
    });

    const bool isPod = ref.apiPlural == QLatin1String("pods");
    if (isPod) {
        const QStringList containers =
            item->data(ColStatus, Qt::UserRole).toStringList();
        // 多容器 Pod：子菜单列容器名（kubectl logs/exec 必须带 -c）。
        auto addContainerActions = [&](QMenu *parentMenu, const QString &title,
                                       bool forLogs) {
            if (containers.size() <= 1) {
                QAction *act = parentMenu->addAction(title);
                const QString container = containers.value(0);
                connect(act, &QAction::triggered, this, [this, ref, container, forLogs]() {
                    if (forLogs)
                        showLogsForPod(ref, container);
                    else
                        execIntoPod(ref, container);
                });
                return;
            }
            QMenu *sub = parentMenu->addMenu(title);
            for (const QString &container : containers) {
                QAction *act = sub->addAction(container);
                connect(act, &QAction::triggered, this, [this, ref, container, forLogs]() {
                    if (forLogs)
                        showLogsForPod(ref, container);
                    else
                        execIntoPod(ref, container);
                });
            }
        };
        addContainerActions(menu, tr("日志"), true);
        addContainerActions(menu, tr("终端"), false);
        menu->addSeparator();
    }

    if (kind->scalable) {
        QAction *scale = menu->addAction(tr("扩缩容..."));
        connect(scale, &QAction::triggered, this, [this, ref]() {
            bool ok = false;
            const int replicas = QInputDialog::getInt(
                this, tr("扩缩容"), tr("%1/%2 副本数:").arg(ref.apiPlural, ref.name),
                1, 0, 100, 1, &ok);
            if (ok)
                m_manager->scaleResource(ref, replicas);
        });
    }
    if (kind->restartable) {
        QAction *restart = menu->addAction(tr("滚动重启"));
        connect(restart, &QAction::triggered, this, [this, ref]() {
            m_manager->rolloutRestart(ref);
        });
    }
    if (isPod || ref.apiPlural == QLatin1String("services")) {
        QAction *forward = menu->addAction(tr("端口转发..."));
        connect(forward, &QAction::triggered, this, [this, ref]() {
            showPortForwardDialog(ref);
        });
    }

    menu->addSeparator();
    QAction *remove = menu->addAction(tr("删除"));
    connect(remove, &QAction::triggered, this, [this, ref]() {
        const auto choice = QMessageBox::warning(
            this, tr("删除资源"),
            tr("确定删除 %1/%2 吗？该操作不可撤销。")
                .arg(ref.apiPlural, ref.name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (choice == QMessageBox::Yes)
            m_manager->deleteResource(ref);
    });

    menu->popup(QCursor::pos());
}

// ---------------------------------------------------------------------------
// 日志 / 终端 / 端口转发
// ---------------------------------------------------------------------------

KubeLogViewer *KubeManagerDialog::logViewer()
{
    if (!m_logViewer) {
        m_logViewer = new KubeLogViewer(this);
        connect(m_logViewer, &KubeLogViewer::stopRequested,
                m_manager, &KubeManager::stopPodLogs);
    }
    return m_logViewer;
}

KubeYamlEditor *KubeManagerDialog::yamlEditor()
{
    if (!m_yamlEditor) {
        m_yamlEditor = new KubeYamlEditor(this);
        connect(m_yamlEditor, &KubeYamlEditor::applyRequested,
                m_manager, &KubeManager::applyYaml);
    }
    return m_yamlEditor;
}

void KubeManagerDialog::showLogsForPod(const cubeshell::KubeObjectRef &pod,
                                       const QString &container)
{
    // 已有日志流在跑时先停掉（同一时刻只看一个 Pod 的日志）。
    if (m_manager->isStreamingLogs())
        m_manager->stopPodLogs();
    logViewer()->beginStream(QStringLiteral("logs %1/%2")
                                 .arg(pod.namespace_, pod.name));
    m_manager->startPodLogs(pod, container);
}

void KubeManagerDialog::execIntoPod(const cubeshell::KubeObjectRef &pod,
                                    const QString &container)
{
    // 交互式 exec 借力终端仿真器（方案 §6.1）：命令串转发到当前终端标签页。
    QString cmd = QStringLiteral("kubectl exec -it %1")
                      .arg(KubeManager::shellQuote(pod.name));
    if (!container.isEmpty())
        cmd += QStringLiteral(" -c %1").arg(KubeManager::shellQuote(container));
    const QString ns = pod.namespace_.isEmpty()
        ? m_manager->currentNamespace() : pod.namespace_;
    cmd += QStringLiteral(" -n %1").arg(KubeManager::shellQuote(ns));
    cmd += m_manager->buildBaseFlags();
    cmd += QStringLiteral(" -- sh\n");
    emit terminalCommandRequested(cmd);
}

void KubeManagerDialog::showPortForwardDialog(const cubeshell::KubeObjectRef &target)
{
    auto *dialog = new KubePortForwardDialog(m_manager, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    if (!target.name.isEmpty())
        dialog->setTarget(target);
    dialog->show();
}

// ---------------------------------------------------------------------------
// 结果回调
// ---------------------------------------------------------------------------

void KubeManagerDialog::onYamlReady(const cubeshell::KubeObjectRef &ref,
                                    const QString &yamlText)
{
    yamlEditor()->showYaml(QStringLiteral("%1/%2@%3")
                               .arg(ref.apiPlural, ref.name, ref.namespace_),
                           yamlText, m_yamlEditRequested);
}

void KubeManagerDialog::onApplyFinished(bool success, const QString &message)
{
    if (success) {
        m_statusLabel->setText(tr("apply 成功: %1").arg(message));
        // apply 可能涉及多文档多种资源，全量回查一次最稳妥。
        m_manager->refreshAll();
    } else {
        QMessageBox::warning(this, tr("应用 YAML"), tr("apply 失败:\n%1").arg(message));
    }
}

void KubeManagerDialog::onOperationFinished(bool success, const QString &op,
                                            const cubeshell::KubeObjectRef &ref,
                                            const QString &message)
{
    if (success) {
        m_statusLabel->setText(tr("%1 成功: %2")
                                   .arg(operationDisplayName(op), message));
        // 滚动重启成功后跟踪 rollout 进度（textReady → 日志窗口）。
        if (op == QLatin1String("restart"))
            m_manager->rolloutStatus(ref);
        return;
    }
    QMessageBox alarmBox(this);
    alarmBox.setWindowTitle(tr("操作失败"));
    alarmBox.setText(tr("%1 %2/%3 失败:\n%4")
                         .arg(operationDisplayName(op), ref.apiPlural,
                              ref.name, message));
    alarmBox.setIcon(QMessageBox::Warning);
    alarmBox.exec();
}

void KubeManagerDialog::onTextReady(const QString &title, const QString &text)
{
    logViewer()->showText(title, text);
}

void KubeManagerDialog::onErrorOccurred(const QString &message)
{
    m_statusLabel->setText(message);
}

} // namespace cubeshell
