// NginxManagerDialog.cpp — see NginxManagerDialog.h.
// 对应 docs/Nginx可视化管理实现方案.md §5.1；结构对齐 dialogs/KubeManagerDialog.cpp。

#include "NginxManagerDialog.h"

#include "NginxLogViewer.h"
#include "NginxSiteEditor.h"
#include "nginx/NginxManager.h"

#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QCursor>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPoint>
#include <QPushButton>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace cubeshell {

namespace {

enum Column { ColName = 0, ColDomains, ColPorts, ColType, ColTarget, ColStatus, ColCount };

// 站点索引保存在「文件名」列（占位行值为 -1）。
constexpr int kSiteIndexRole = Qt::UserRole + 1;

QString portsText(const QList<int> &ports)
{
    QStringList s;
    for (int p : ports)
        s.append(QString::number(p));
    return s.join(QLatin1Char(','));
}

} // namespace

NginxManagerDialog::NginxManagerDialog(NginxManager *manager, QWidget *parent)
    : QDialog(parent)
    , m_manager(manager)
{
    setWindowTitle(tr("Nginx 站点管理"));
    setWindowIcon(QIcon(QStringLiteral(":/nginx.png")));
    resize(960, 620);

    auto *root = new QVBoxLayout(this);

    // 第一行：后端 / 配置目录 / 状态。
    auto *bar = new QHBoxLayout();
    bar->addWidget(new QLabel(tr("后端:"), this));
    m_backendCombo = new QComboBox(this);
    m_backendCombo->addItem(tr("本机"), false);
    m_backendCombo->addItem(tr("SSH 会话"), true);
    bar->addWidget(m_backendCombo);
    m_confDirBtn = new QPushButton(tr("配置目录…"), this);
    bar->addWidget(m_confDirBtn);
    m_confDirLabel = new QLabel(this);
    m_confDirLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bar->addWidget(m_confDirLabel);
    m_binBtn = new QPushButton(tr("nginx 路径…"), this);
    bar->addWidget(m_binBtn);
    bar->addStretch(1);
    m_statusLabel = new QLabel(this);
    bar->addWidget(m_statusLabel);
    root->addLayout(bar);

    // 第二行：操作按钮。
    auto *bar2 = new QHBoxLayout();
    auto *refreshBtn = new QPushButton(tr("刷新"), this);
    auto *newBtn = new QPushButton(tr("新建站点…"), this);
    m_testBtn = new QPushButton(tr("校验配置"), this);
    m_reloadBtn = new QPushButton(tr("重载 Nginx"), this);
    auto *startBtn = new QPushButton(tr("启动"), this);
    auto *stopBtn = new QPushButton(tr("停止"), this);
    auto *restartBtn = new QPushButton(tr("重启"), this);
    auto *logBtn = new QPushButton(tr("查看日志"), this);
    bar2->addWidget(refreshBtn);
    bar2->addWidget(newBtn);
    bar2->addWidget(m_testBtn);
    bar2->addWidget(m_reloadBtn);
    bar2->addWidget(startBtn);
    bar2->addWidget(stopBtn);
    bar2->addWidget(restartBtn);
    bar2->addWidget(logBtn);
    bar2->addStretch(1);
    root->addLayout(bar2);

    // 站点树。
    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("treeWidgetNginx"));
    m_tree->setColumnCount(ColCount);
    m_tree->setHeaderLabels({tr("配置文件"), tr("域名"), tr("端口"), tr("类型"), tr("目标"),
                             tr("状态")});
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setRootIsDecorated(true);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setSectionResizeMode(ColName, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(ColDomains, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(ColPorts, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(ColType, QHeaderView::ResizeToContents);
    root->addWidget(m_tree, 1);

    auto *closeBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    root->addWidget(closeBox);

    // 控件信号。
    connect(m_backendCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &NginxManagerDialog::onBackendComboChanged);
    connect(refreshBtn, &QPushButton::clicked, this, &NginxManagerDialog::refreshInfo);
    connect(newBtn, &QPushButton::clicked, this, &NginxManagerDialog::onNewSite);
    connect(m_testBtn, &QPushButton::clicked, this, &NginxManagerDialog::onTestConfig);
    connect(m_reloadBtn, &QPushButton::clicked, this, &NginxManagerDialog::onReload);
    connect(logBtn, &QPushButton::clicked, this, &NginxManagerDialog::onViewLogs);
    connect(m_confDirBtn, &QPushButton::clicked, this, &NginxManagerDialog::onConfDirButtonClicked);
    connect(m_binBtn, &QPushButton::clicked, this, &NginxManagerDialog::onNginxBinButtonClicked);
    connect(startBtn, &QPushButton::clicked, this, &NginxManagerDialog::onStartService);
    connect(stopBtn, &QPushButton::clicked, this, &NginxManagerDialog::onStopService);
    connect(restartBtn, &QPushButton::clicked, this, &NginxManagerDialog::onRestartService);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this,
            &NginxManagerDialog::onTreeContextMenu);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &NginxManagerDialog::onItemDoubleClicked);
    connect(closeBox, &QDialogButtonBox::rejected, this, &QDialog::close);

    // Manager 信号 —— 一律 QueuedConnection（可能发自工作线程）。
    connect(m_manager, &NginxManager::availabilityReady, this,
            &NginxManagerDialog::onAvailabilityReady, Qt::QueuedConnection);
    connect(m_manager, &NginxManager::serviceStatusReady, this,
            &NginxManagerDialog::onServiceStatusReady, Qt::QueuedConnection);
    connect(m_manager, &NginxManager::sitesUpdated, this, &NginxManagerDialog::onSitesUpdated,
            Qt::QueuedConnection);
    connect(m_manager, &NginxManager::fileContentReady, this,
            &NginxManagerDialog::onFileContentReady, Qt::QueuedConnection);
    connect(m_manager, &NginxManager::fileSaved, this, &NginxManagerDialog::onFileSaved,
            Qt::QueuedConnection);
    connect(m_manager, &NginxManager::testResultReady, this,
            &NginxManagerDialog::onTestResultReady, Qt::QueuedConnection);
    connect(m_manager, &NginxManager::reloadFinished, this, &NginxManagerDialog::onReloadFinished,
            Qt::QueuedConnection);
    connect(m_manager, &NginxManager::operationFinished, this,
            &NginxManagerDialog::onOperationFinished, Qt::QueuedConnection);
    connect(m_manager, &NginxManager::errorOccurred, this, &NginxManagerDialog::onErrorOccurred,
            Qt::QueuedConnection);
    connect(m_manager, &NginxManager::logStarted, this,
            [this](const QString &title) {
                if (m_logViewer)
                    m_logViewer->beginStream(title);
            }, Qt::QueuedConnection);
    connect(m_manager, &NginxManager::logChunk, this,
            [this](const QString &t) {
                if (m_logViewer)
                    m_logViewer->appendChunk(t);
            }, Qt::QueuedConnection);
    connect(m_manager, &NginxManager::logsFinished, this,
            [this](int code) {
                if (m_logViewer)
                    m_logViewer->finishStream(code);
            }, Qt::QueuedConnection);
    connect(m_manager, &NginxManager::logTextReady, this,
            [this](const QString &title, const QString &text) {
                if (m_logViewer)
                    m_logViewer->showText(title, text);
            }, Qt::QueuedConnection);

    setConfDirDisplay(m_manager->confDir());
    refreshInfo();
}

// ---------------------------------------------------------------------------
// 主窗口对接
// ---------------------------------------------------------------------------

void NginxManagerDialog::setRemoteBackendAvailable(bool available, const QString &label)
{
    m_backendLabelRemote = label;
    m_backendCombo->setItemText(1, available ? label : tr("SSH 会话（无活动连接）"));
    if (auto *model = qobject_cast<QStandardItemModel *>(m_backendCombo->model())) {
        if (QStandardItem *it = model->item(1))
            it->setEnabled(available);
    }
}

void NginxManagerDialog::setBackendSelection(bool useRemote)
{
    m_backendIsRemote = useRemote;
    const QSignalBlocker blocker(m_backendCombo);
    m_backendCombo->setCurrentIndex(useRemote ? 1 : 0);
}

void NginxManagerDialog::setConfDirDisplay(const QString &path)
{
    m_confDirLabel->setText(tr("目录: %1").arg(path));
    m_confDirLabel->setToolTip(path);
}

// ---------------------------------------------------------------------------
// 刷新
// ---------------------------------------------------------------------------

void NginxManagerDialog::refreshInfo()
{
    if (m_refreshing)
        return;
    m_refreshing = true;
    m_binBtn->setToolTip(tr("当前: %1\n留空恢复自动探测").arg(m_manager->nginxBin()));
    showPlaceholderRow(tr("正在刷新…"));
    m_manager->checkAvailability();
    m_manager->refreshServiceStatus();
    m_manager->refreshSites();
}

void NginxManagerDialog::onAvailabilityReady(bool available, const QString &versionText)
{
    m_versionText = available ? versionText : QString();
    m_binBtn->setToolTip(tr("当前: %1\n留空恢复自动探测").arg(m_manager->nginxBin()));
    if (!available) {
        // 占位行给结论；原始探测输出/错误进 tooltip，便于定位（路径？权限？sudo？）
        showPlaceholderRow(tr("未检测到 nginx（本机未安装，或远端 PATH/路径不识别）"));
        m_statusLabel->setToolTip(versionText);
    } else {
        m_statusLabel->setToolTip(QString());
    }
    const QString svc = m_serviceText.isEmpty() ? tr("状态未知") : m_serviceText;
    m_statusLabel->setText(tr("版本: %1 · %2")
                               .arg(m_versionText.isEmpty() ? tr("未知") : m_versionText, svc));
}

void NginxManagerDialog::onServiceStatusReady(bool running, const QString &detail)
{
    Q_UNUSED(detail);
    m_serviceText = running ? tr("运行中") : tr("已停止");
    const QString ver = m_versionText.isEmpty() ? tr("未知") : m_versionText;
    m_statusLabel->setText(tr("版本: %1 · %2").arg(ver, m_serviceText));
}

void NginxManagerDialog::onSitesUpdated(const QList<cubeshell::nginx::SiteView> &sites)
{
    m_sites = sites;
    rebuildTree();
    m_refreshing = false;
}

// ---------------------------------------------------------------------------
// 树
// ---------------------------------------------------------------------------

void NginxManagerDialog::showPlaceholderRow(const QString &text)
{
    m_tree->clear();
    auto *item = new QTreeWidgetItem(m_tree);
    item->setText(ColName, text);
    item->setData(ColName, kSiteIndexRole, -1);
    item->setForeground(ColName, QBrush(QColor(140, 140, 140)));
}

void NginxManagerDialog::rebuildTree()
{
    m_tree->clear();
    if (m_sites.isEmpty()) {
        showPlaceholderRow(tr("没有找到站点配置（目录下无 *.conf）"));
        return;
    }

    auto *enabledRoot = new QTreeWidgetItem(m_tree);
    auto *disabledRoot = new QTreeWidgetItem(m_tree);
    int enabledCount = 0;
    int disabledCount = 0;

    for (int i = 0; i < m_sites.size(); ++i) {
        const nginx::SiteView &s = m_sites.at(i);
        QTreeWidgetItem *parent = s.enabled ? enabledRoot : disabledRoot;
        auto *it = new QTreeWidgetItem(parent);
        it->setText(ColName, s.fileName);
        it->setText(ColDomains, s.serverNames.join(QLatin1Char(',')));
        it->setText(ColPorts, portsText(s.listenPorts));
        it->setText(ColType, s.isProxy ? tr("反向代理") : (s.rootOrTarget.isEmpty() ? QString()
                                                                                   : tr("静态")));
        it->setText(ColTarget, s.rootOrTarget);
        it->setText(ColStatus, s.parseOk ? (s.enabled ? tr("已启用") : tr("未启用"))
                                         : tr("解析失败"));
        it->setData(ColName, kSiteIndexRole, i);
        it->setToolTip(ColName, s.filePath);
        if (!s.parseOk) {
            it->setForeground(ColStatus, QBrush(QColor(200, 60, 60)));
            it->setToolTip(ColStatus, s.error);
        }
        if (s.enabled)
            ++enabledCount;
        else
            ++disabledCount;
    }

    enabledRoot->setText(ColName, tr("已启用站点 (%1)").arg(enabledCount));
    enabledRoot->setData(ColName, kSiteIndexRole, -1);
    disabledRoot->setText(ColName, tr("未启用站点 (%1)").arg(disabledCount));
    disabledRoot->setData(ColName, kSiteIndexRole, -1);
    if (disabledCount == 0)
        disabledRoot->setHidden(true);

    m_tree->expandAll();
}

const nginx::SiteView *NginxManagerDialog::selectedSite() const
{
    QTreeWidgetItem *it = m_tree->currentItem();
    if (!it)
        return nullptr;
    const QVariant v = it->data(ColName, kSiteIndexRole);
    bool ok = false;
    const int idx = v.toInt(&ok);
    if (!ok || idx < 0 || idx >= m_sites.size())
        return nullptr;
    return &m_sites.at(idx);
}

// ---------------------------------------------------------------------------
// 编辑
// ---------------------------------------------------------------------------

NginxSiteEditor *NginxManagerDialog::editor()
{
    if (!m_editor) {
        m_editor = new NginxSiteEditor(this);
        connect(m_editor, &NginxSiteEditor::saveRequested, this,
                &NginxManagerDialog::onEditorSaveRequested);
    }
    return m_editor;
}

void NginxManagerDialog::beginEdit(const QString &path, bool isNew)
{
    m_pendingEditPath = path;
    m_pendingEditIsNew = isNew;
    if (isNew) {
        m_preSaveText.clear();
        openEditor(path, QString(), true);
        return;
    }
    m_manager->readFile(path);
}

void NginxManagerDialog::onFileContentReady(const QString &path, const QString &text)
{
    if (m_pendingEditPath.isEmpty() || path != m_pendingEditPath)
        return;
    m_preSaveText = text;
    openEditor(path, text, false);
}

void NginxManagerDialog::openEditor(const QString &path, const QString &text, bool isNew)
{
    m_pendingEditPath.clear();
    NginxSiteEditor *ed = editor();
    ed->setSite(path, text, isNew);
    ed->show();
    ed->raise();
    ed->activateWindow();
}

void NginxManagerDialog::onItemDoubleClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (!item)
        return;
    m_tree->setCurrentItem(item);
    if (const nginx::SiteView *s = selectedSite())
        beginEdit(s->filePath, false);
}

void NginxManagerDialog::onEditorSaveRequested(const QString &path, const QString &text)
{
    m_saving = true;
    m_pendingSavePath = path;
    m_manager->writeFile(path, text);
}

void NginxManagerDialog::onFileSaved(const QString &path)
{
    if (m_saving && path == m_pendingSavePath)
        m_manager->testConfig();
}

void NginxManagerDialog::onTestResultReady(bool ok, const QString &output)
{
    if (!m_saving) {
        QMessageBox::information(
            this, tr("配置校验"),
            (ok ? tr("nginx -t 校验通过。\n\n") : tr("nginx -t 校验未通过：\n\n")) + output);
        return;
    }
    m_saving = false;
    if (ok) {
        QMessageBox::information(this, tr("保存成功"),
                                 tr("站点已保存并通过 nginx -t 校验。\n点「重载 Nginx」使其生效。"));
        if (m_editor)
            m_editor->markSaved();
    } else {
        const QMessageBox::StandardButton r = QMessageBox::warning(
            this, tr("配置校验未通过"),
            tr("文件已写入，但 nginx -t 报错：\n\n%1\n\n是否回滚到保存前的内容？").arg(output),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (r == QMessageBox::Yes) {
            if (m_preSaveText.isEmpty())
                m_manager->deleteSite(m_pendingSavePath);
            else
                m_manager->writeFile(m_pendingSavePath, m_preSaveText);
        }
    }
    m_pendingSavePath.clear();
    refreshInfo();
}

// ---------------------------------------------------------------------------
// 操作按钮
// ---------------------------------------------------------------------------

void NginxManagerDialog::onNewSite()
{
    m_pendingEditIsNew = true;
    m_preSaveText.clear();
    const QString dir = m_manager->confDir() + QStringLiteral("/conf.d/");
    openEditor(dir + QStringLiteral("new-site.conf"), QString(), true);
}

void NginxManagerDialog::onTestConfig()
{
    m_saving = false;
    m_manager->testConfig();
}

void NginxManagerDialog::onReload()
{
    m_manager->reload();
}

void NginxManagerDialog::onViewLogs()
{
    showLogWindow(QStringLiteral("error"), true);
}

void NginxManagerDialog::onReloadFinished(bool ok, const QString &message)
{
    if (ok)
        QMessageBox::information(this, tr("重载 Nginx"), tr("nginx 已重载，配置生效。"));
    else
        QMessageBox::warning(this, tr("重载失败"), message);
    m_manager->refreshServiceStatus();
}

void NginxManagerDialog::onOperationFinished(bool ok, const QString &op, const QString &message)
{
    if (!ok) {
        QMessageBox::warning(this, tr("操作失败"),
                             tr("%1 操作失败：%2").arg(op, message.isEmpty() ? tr("未知错误")
                                                                            : message));
    } else if (op == QLatin1String("start") || op == QLatin1String("stop")
               || op == QLatin1String("restart")) {
        const QString label = op == QLatin1String("start")
                                  ? tr("启动")
                                  : (op == QLatin1String("stop") ? tr("停止") : tr("重启"));
        QMessageBox::information(this, tr("服务操作"), tr("nginx 服务已%1。").arg(label));
    }
    refreshInfo();
}

void NginxManagerDialog::onErrorOccurred(const QString &message)
{
    m_statusLabel->setText(message);
    m_refreshing = false;
    if (m_saving) {
        m_saving = false;
        QMessageBox::warning(this, tr("错误"), message);
    }
}

void NginxManagerDialog::onConfDirButtonClicked()
{
    bool ok = false;
    const QString dir = QInputDialog::getText(this, tr("配置目录"),
                                              tr("nginx 配置目录（如 /etc/nginx）："),
                                              QLineEdit::Normal, m_manager->confDir(), &ok);
    if (ok && !dir.trimmed().isEmpty())
        emit confDirSelected(dir.trimmed());
}

void NginxManagerDialog::onNginxBinButtonClicked()
{
    bool ok = false;
    const QString path = QInputDialog::getText(
        this, tr("nginx 可执行文件"),
        tr("nginx 可执行文件路径（留空恢复自动探测）："), QLineEdit::Normal,
        m_manager->nginxBin(), &ok);
    if (ok)
        emit nginxBinarySelected(path.trimmed());
}

void NginxManagerDialog::onStartService()
{
    m_manager->startService();
}

void NginxManagerDialog::onStopService()
{
    m_manager->stopService();
}

void NginxManagerDialog::onRestartService()
{
    m_manager->restartService();
}

void NginxManagerDialog::onBackendComboChanged()
{
    m_backendIsRemote = m_backendCombo->currentData().toBool();
    emit backendChangeRequested(m_backendIsRemote);
}

// ---------------------------------------------------------------------------
// 右键菜单
// ---------------------------------------------------------------------------

void NginxManagerDialog::onTreeContextMenu(const QPoint &position)
{
    QTreeWidgetItem *item = m_tree->itemAt(position);
    if (item)
        m_tree->setCurrentItem(item);
    const nginx::SiteView *site = selectedSite();
    if (!site)
        return;

    QMenu menu(this);
    menu.addAction(tr("编辑站点"), this, [this, site]() { beginEdit(site->filePath, false); });
    menu.addAction(site->enabled ? tr("停用站点") : tr("启用站点"), this,
                   [this, site]() { toggleEnabled(*site); });
    menu.addSeparator();
    menu.addAction(tr("校验配置 (nginx -t)"), this, &NginxManagerDialog::onTestConfig);
    menu.addAction(tr("查看错误日志"), this, [this]() { showLogWindow(QStringLiteral("error"), true); });
    menu.addAction(tr("复制域名"), this, [site]() {
        QApplication::clipboard()->setText(site->serverNames.join(QLatin1Char(' ')));
    });
    menu.addSeparator();
    menu.addAction(tr("删除站点"), this, [this, site]() { deleteSitePrompt(*site); });
    menu.exec(m_tree->viewport()->mapToGlobal(position));
}

void NginxManagerDialog::toggleEnabled(const nginx::SiteView &site)
{
    if (site.dirKind == QLatin1String("sites-available")
        || site.dirKind == QLatin1String("sites-enabled")) {
        if (site.enabled)
            m_manager->disableSite(site.fileName);
        else
            m_manager->enableSite(site.fileName);
        return;
    }
    // conf.d / nginx.conf：nginx 按 *.conf 加载，通过改后缀实现停用/启用。
    if (site.fileName == QLatin1String("nginx.conf")) {
        QMessageBox::information(this, tr("无法停用"), tr("主配置 nginx.conf 不能停用。"));
        return;
    }
    if (site.enabled) {
        m_manager->moveFile(site.filePath, site.filePath + QStringLiteral(".disabled"));
    } else if (site.fileName.endsWith(QLatin1String(".disabled"))) {
        QString to = site.filePath;
        to.chop(QStringLiteral(".disabled").size());
        m_manager->moveFile(site.filePath, to);
    } else {
        QMessageBox::information(this, tr("无法启用"),
                                 tr("该文件不是 .disabled 命名，无法自动启用。"));
    }
}

void NginxManagerDialog::deleteSitePrompt(const nginx::SiteView &site)
{
    const auto r = QMessageBox::question(
        this, tr("删除站点"),
        tr("确定删除配置文件「%1」？此操作不可撤销。").arg(site.fileName),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (r == QMessageBox::Yes)
        m_manager->deleteSite(site.filePath);
}

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------

void NginxManagerDialog::showLogWindow(const QString &kind, bool follow)
{
    if (!m_logViewer) {
        m_logViewer = new NginxLogViewer(this);
        connect(m_logViewer, &NginxLogViewer::stopRequested, this,
                [this]() { m_manager->stopLogStream(); });
    }
    m_logViewer->show();
    m_logViewer->raise();
    m_logViewer->activateWindow();
    if (follow) {
        if (!m_manager->isStreamingLogs())
            m_manager->startLogStream(kind);
    } else {
        m_manager->fetchLog(kind);
    }
}

} // namespace cubeshell
