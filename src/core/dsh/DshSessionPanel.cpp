// DshSessionPanel.cpp — 会话管理 Tab。见 DshSessionPanel.h 的说明。

#include "dsh/DshSessionPanel.h"

#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace cubeshell {

DshSessionPanel::DshSessionPanel(DshManager *manager, QWidget *parent)
    : QWidget(parent)
    , m_manager(manager)
{
    buildUi();
}

void DshSessionPanel::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    // --- 顶部：摘要 + 刷新 ---
    auto *topRow = new QHBoxLayout();
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    topRow->addWidget(m_summaryLabel, 1);
    m_refreshBtn = new QPushButton(tr("刷新"), this);
    topRow->addWidget(m_refreshBtn);
    root->addLayout(topRow);

    connect(m_refreshBtn, &QPushButton::clicked, this, &DshSessionPanel::refresh);

    // --- 会话表 ---
    m_table = new QTableWidget(this);
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels(
        {tr("会话 ID"), tr("标题"), tr("工作目录"), tr("轮次"), tr("最后修改"), tr("大小")});
    QHeaderView *hh = m_table->horizontalHeader();
    hh->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(1, QHeaderView::Stretch);          // 标题
    hh->setSectionResizeMode(2, QHeaderView::Stretch);          // 工作目录
    hh->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    root->addWidget(m_table, 1);

    connect(m_table, &QTableWidget::itemSelectionChanged,
            this, &DshSessionPanel::onSelectionChanged);

    // --- 操作行 ---
    auto *opRow = new QHBoxLayout();
    opRow->addWidget(new QLabel(tr("恢复用 profile:"), this));
    m_profileCombo = new QComboBox(this);
    m_profileCombo->setMinimumWidth(140);
    opRow->addWidget(m_profileCombo);
    opRow->addStretch();
    m_resumeBtn = new QPushButton(tr("在终端恢复"), this);
    m_deleteBtn = new QPushButton(tr("删除会话"), this);
    opRow->addWidget(m_resumeBtn);
    opRow->addWidget(m_deleteBtn);
    root->addLayout(opRow);

    connect(m_resumeBtn, &QPushButton::clicked, this, &DshSessionPanel::onResumeClicked);
    connect(m_deleteBtn, &QPushButton::clicked, this, &DshSessionPanel::onDeleteClicked);

    m_resumeBtn->setEnabled(false);
    m_deleteBtn->setEnabled(false);
}

// 无标题时按轮次区分三种情况，避免把「没缓存」误显示成「空会话」。
QString DshSessionPanel::displayTitle(const DshManager::SessionInfo &s)
{
    if (!s.title.isEmpty())
        return s.title;
    if (s.turns < 0)
        return tr("(无元信息)");
    if (s.turns == 0)
        return tr("(空会话)");
    return tr("(未命名)");
}

// 列表里只显示 uuid 前 8 位；完整 id（含 session- 前缀）放 tooltip。
QString DshSessionPanel::shortId(const QString &id)
{
    QString s = id;
    if (s.startsWith(QStringLiteral("session-")))
        s.remove(0, int(qstrlen("session-")));
    return s.left(8);
}

QString DshSessionPanel::prettySize(qint64 bytes)
{
    if (bytes <= 0)
        return QStringLiteral("-");
    return QLocale().formattedDataSize(bytes);
}

void DshSessionPanel::refresh()
{
    // profile 下拉：默认选 tui（它提供 --resume 交互恢复）。
    const QString keep = m_profileCombo->currentText();
    const QStringList profiles = DshManager::listProfiles();
    {
        const QSignalBlocker blocker(m_profileCombo);
        m_profileCombo->clear();
        m_profileCombo->addItems(profiles);
        int idx = keep.isEmpty() ? -1 : m_profileCombo->findText(keep);
        if (idx < 0)
            idx = m_profileCombo->findText(QStringLiteral("tui"));
        if (idx >= 0)
            m_profileCombo->setCurrentIndex(idx);
    }

    m_sessions = DshManager::listSessions();
    m_table->setRowCount(m_sessions.size());
    for (int i = 0; i < m_sessions.size(); ++i) {
        const DshManager::SessionInfo &s = m_sessions.at(i);

        // ID 列只显示 uuid 前 8 位（全长挤掉别的列），完整 id 放 tooltip。
        auto *idItem = new QTableWidgetItem(shortId(s.id));
        idItem->setToolTip(s.id);
        m_table->setItem(i, 0, idItem);

        auto *titleItem = new QTableWidgetItem(displayTitle(s));
        if (s.title.isEmpty())
            titleItem->setForeground(QBrush(QColor(0x9e, 0x9e, 0x9e)));
        else
            titleItem->setToolTip(s.title); // 长标题被列宽截断时可悬停看全
        m_table->setItem(i, 1, titleItem);

        // 工作目录：精确值直接显示；反解的近似值明确标注，避免误以为能恢复。
        auto *cwdItem = new QTableWidgetItem(
            s.cwdExact ? s.cwd : tr("%1 (近似)").arg(s.cwd));
        cwdItem->setToolTip(
            s.cwdExact ? tr("会话目录：%1").arg(s.workspace)
                       : tr("storages 缓存中无此会话，路径由目录名 %1 反解，可能不准")
                             .arg(s.workspace));
        m_table->setItem(i, 2, cwdItem);

        m_table->setItem(i, 3, new QTableWidgetItem(
            s.turns < 0 ? QStringLiteral("-") : QString::number(s.turns)));
        m_table->setItem(i, 4, new QTableWidgetItem(
            s.modified.toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
        m_table->setItem(i, 5, new QTableWidgetItem(prettySize(s.sizeBytes)));
    }

    m_summaryLabel->setText(
        m_sessions.isEmpty()
            ? tr("暂无会话（%1）").arg(DshManager::sessionsDir())
            : tr("共 %1 个会话 · 目录 %2")
                  .arg(m_sessions.size()).arg(DshManager::sessionsDir()));
    onSelectionChanged();
}

const DshManager::SessionInfo *DshSessionPanel::selectedSession() const
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_sessions.size() || m_table->selectedItems().isEmpty())
        return nullptr;
    return &m_sessions.at(row);
}

void DshSessionPanel::onSelectionChanged()
{
    const bool has = (selectedSession() != nullptr);
    m_resumeBtn->setEnabled(has && !m_profileCombo->currentText().isEmpty());
    m_deleteBtn->setEnabled(has);
}

void DshSessionPanel::onResumeClicked()
{
    const DshManager::SessionInfo *s = selectedSession();
    if (!s)
        return;
    const QString profile = m_profileCombo->currentText();
    if (profile.isEmpty()) {
        QMessageBox::information(this, tr("恢复会话"),
                                 tr("没有可用 profile。请先在「插件」页安装一个"
                                    "带界面的 profile（如 tui）。"));
        return;
    }
    // 空会话恢复出来是空白界面，先说清楚，免得当成恢复失败。
    if (s->turns == 0) {
        const auto btn = QMessageBox::question(
            this, tr("恢复会话"),
            tr("这条会话没有任何对话内容（0 轮），恢复后是一个空白界面。仍要继续?"));
        if (btn != QMessageBox::Yes)
            return;
    }
    // 终端开在会话原本的工作目录里，好让 agent 的文件操作落在对的项目上。
    // 这不影响能否找到会话（--resume 按 id 全局查找，与 cwd 无关），所以目录
    // 没了也照样恢复，只是退回终端默认目录。
    const QString cwd = QDir(s->cwd).exists() ? s->cwd : QString();
    // 已全局安装用 dsh，否则退回 npx（与状态页 CLI 命令构造同策略）。
    const QString base = DshManager::findNodeTool(QStringLiteral("dsh")).isEmpty()
                             ? QStringLiteral("npx -y @deepseek-ai/dsh")
                             : QStringLiteral("dsh");
    // s->id 含 "session-" 前缀，必须原样传：剥成裸 uuid 时 dsh 报 session not found。
    emit openCliRequested(QStringLiteral("%1 --profile %2 --resume %3")
                              .arg(base, profile, s->id),
                          cwd);
}

void DshSessionPanel::onDeleteClicked()
{
    const DshManager::SessionInfo *s = selectedSession();
    if (!s)
        return;
    const QString id = s->id;
    const QString ws = s->workspace;
    const auto btn = QMessageBox::question(
        this, tr("删除会话"),
        tr("确定删除会话 %1 ?\n标题：%2\n工作目录：%3\n此操作不可恢复。")
            .arg(shortId(id), displayTitle(*s), s->cwd));
    if (btn != QMessageBox::Yes)
        return;

    QString error;
    if (!DshManager::deleteSession(ws, id, &error)) {
        QMessageBox::warning(this, tr("删除会话"), error);
        return;
    }
    refresh();
}

} // namespace cubeshell
