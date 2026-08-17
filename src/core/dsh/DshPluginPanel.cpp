// DshPluginPanel.cpp — 插件管理 Tab。见 DshPluginPanel.h 的说明。

#include "dsh/DshPluginPanel.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace cubeshell {

DshPluginPanel::DshPluginPanel(DshManager *manager, QWidget *parent)
    : QWidget(parent)
    , m_manager(manager)
{
    buildUi();
    connect(m_manager, &DshManager::pluginLog, this, &DshPluginPanel::onPluginLog);
    connect(m_manager, &DshManager::pluginOpFinished,
            this, &DshPluginPanel::onPluginOpFinished);
}

void DshPluginPanel::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    // --- 顶部：profile 选择 + bundles 摘要 ---
    auto *topRow = new QHBoxLayout();
    topRow->addWidget(new QLabel(tr("Profile:"), this));
    m_profileCombo = new QComboBox(this);
    m_profileCombo->setMinimumWidth(160);
    topRow->addWidget(m_profileCombo);
    topRow->addSpacing(16);
    m_bundlesLabel = new QLabel(this);
    m_bundlesLabel->setWordWrap(true);
    m_bundlesLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    topRow->addWidget(m_bundlesLabel, 1);
    m_refreshBtn = new QPushButton(tr("刷新"), this);
    topRow->addWidget(m_refreshBtn);
    root->addLayout(topRow);

    connect(m_profileCombo, &QComboBox::currentIndexChanged,
            this, &DshPluginPanel::onProfileChanged);
    connect(m_refreshBtn, &QPushButton::clicked, this, &DshPluginPanel::refresh);

    // --- 已装插件表 ---
    m_table = new QTableWidget(this);
    m_table->setColumnCount(2);
    m_table->setHorizontalHeaderLabels({tr("插件包"), tr("版本")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    root->addWidget(m_table, 1);

    connect(m_table, &QTableWidget::itemSelectionChanged,
            this, &DshPluginPanel::onSelectionChanged);

    // --- 安装 / 卸载 ---
    auto *opBox = new QGroupBox(tr("安装 / 卸载"), this);
    auto *opLay = new QHBoxLayout(opBox);
    m_packageEdit = new QLineEdit(this);
    m_packageEdit->setPlaceholderText(
        tr("npm 包名，例如 deepseek-harness-tui / dsh-mcp-adapter"));
    opLay->addWidget(m_packageEdit, 1);
    m_installBtn = new QPushButton(tr("安装插件"), this);
    m_removeBtn = new QPushButton(tr("卸载所选"), this);
    opLay->addWidget(m_installBtn);
    opLay->addWidget(m_removeBtn);
    root->addWidget(opBox);

    connect(m_installBtn, &QPushButton::clicked, this, &DshPluginPanel::onInstallClicked);
    connect(m_removeBtn, &QPushButton::clicked, this, &DshPluginPanel::onRemoveClicked);

    // --- 操作日志 ---
    auto *logBox = new QGroupBox(tr("操作日志"), this);
    auto *logLay = new QVBoxLayout(logBox);
    m_logView = new QPlainTextEdit(this);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(2000);
    m_logView->setMaximumHeight(160);
    logLay->addWidget(m_logView);
    root->addWidget(logBox);

    m_removeBtn->setEnabled(false);
}

QString DshPluginPanel::currentProfile() const
{
    return m_profileCombo->currentText();
}

void DshPluginPanel::refresh()
{
    // 重列 profile，尽量保持当前选择。
    const QString keep = currentProfile();
    const QStringList profiles = DshManager::listProfiles();
    {
        // 阻断信号，避免填充过程触发多次 reloadPlugins。
        const QSignalBlocker blocker(m_profileCombo);
        m_profileCombo->clear();
        m_profileCombo->addItems(profiles);
        if (!keep.isEmpty()) {
            const int idx = m_profileCombo->findText(keep);
            if (idx >= 0)
                m_profileCombo->setCurrentIndex(idx);
        }
    }
    if (profiles.isEmpty()) {
        m_bundlesLabel->setText(
            tr("未发现任何 profile（%1）。可先安装插件创建，例如 tui。")
                .arg(DshManager::profilesDir()));
    }
    reloadPlugins();
}

void DshPluginPanel::reloadPlugins()
{
    const QString profile = currentProfile();
    m_table->setRowCount(0);
    if (profile.isEmpty()) {
        m_bundlesLabel->clear();
        onSelectionChanged();
        return;
    }

    const QStringList bundles = DshManager::profileBundles(profile);
    m_bundlesLabel->setText(bundles.isEmpty()
                                ? tr("bundles: （无）")
                                : tr("bundles: %1").arg(bundles.join(QStringLiteral(", "))));

    const QList<DshManager::PluginInfo> plugins = DshManager::listPlugins(profile);
    m_table->setRowCount(plugins.size());
    for (int i = 0; i < plugins.size(); ++i) {
        m_table->setItem(i, 0, new QTableWidgetItem(plugins.at(i).name));
        m_table->setItem(i, 1, new QTableWidgetItem(plugins.at(i).version));
    }
    onSelectionChanged();
}

void DshPluginPanel::onProfileChanged(int)
{
    reloadPlugins();
}

void DshPluginPanel::onSelectionChanged()
{
    const bool hasSel = !m_table->selectedItems().isEmpty();
    m_removeBtn->setEnabled(hasSel && !m_manager->isPluginBusy());
}

void DshPluginPanel::setBusy(bool busy)
{
    m_installBtn->setEnabled(!busy);
    m_removeBtn->setEnabled(!busy && !m_table->selectedItems().isEmpty());
    m_profileCombo->setEnabled(!busy);
    m_refreshBtn->setEnabled(!busy);
}

void DshPluginPanel::onInstallClicked()
{
    const QString profile = currentProfile();
    const QString pkg = m_packageEdit->text().trimmed();
    if (pkg.isEmpty()) {
        QMessageBox::information(this, tr("安装插件"), tr("请先填写要安装的 npm 包名。"));
        return;
    }
    if (profile.isEmpty()) {
        // 没有任何 profile 时，dsh plugin add 会顺带创建它，这里提示预期行为。
        QMessageBox::information(this, tr("安装插件"),
                                 tr("当前没有可选 profile。请在下拉框中选择，"
                                    "或先用终端执行：dsh plugin --profile <名字> add %1")
                                     .arg(pkg));
        return;
    }
    setBusy(true);
    m_manager->addPlugin(profile, pkg);
}

void DshPluginPanel::onRemoveClicked()
{
    const int row = m_table->currentRow();
    if (row < 0 || !m_table->item(row, 0))
        return;
    const QString pkg = m_table->item(row, 0)->text();
    const QString profile = currentProfile();
    const auto btn = QMessageBox::question(
        this, tr("卸载插件"),
        tr("确定从 profile「%1」卸载插件 %2 ?").arg(profile, pkg));
    if (btn != QMessageBox::Yes)
        return;
    setBusy(true);
    m_manager->removePlugin(profile, pkg);
}

void DshPluginPanel::onPluginLog(const QString &line)
{
    appendLog(line);
}

void DshPluginPanel::onPluginOpFinished(bool ok, const QString &message)
{
    appendLog(ok ? tr("✓ %1").arg(message) : tr("✗ %1").arg(message));
    setBusy(false);
    if (ok) {
        m_packageEdit->clear();
        refresh(); // 插件列表/新建的 profile 都可能变了
    }
}

void DshPluginPanel::appendLog(const QString &text)
{
    m_logView->appendPlainText(text);
}

} // namespace cubeshell
