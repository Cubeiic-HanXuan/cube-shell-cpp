// HermesMemoryWidget.cpp — see HermesMemoryWidget.h for the port map.
// 对应Python: core/hermes/memory_widget.py

#include "hermes/HermesMemoryWidget.h"

#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <QtDebug>

namespace cubeshell {

namespace {
const char *kMemoryFiles[] = {"MEMORY.md", "USER.md", "SOUL.md"};
} // namespace

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------

HermesMemoryWidget::HermesMemoryWidget(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
}

HermesMemoryWidget::~HermesMemoryWidget()
{
    for (QFuture<void> &f : m_futures)
        f.waitForFinished();
}

// 对应Python: MemoryWidget.set_backend
void HermesMemoryWidget::setBackend(HermesBackend *backend)
{
    m_backend = backend;
}

// 对应Python: MemoryWidget.refresh
void HermesMemoryWidget::refresh()
{
    loadSessions();
    loadMemoryFiles();
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

// 对应Python: MemoryWidget._init_ui
void HermesMemoryWidget::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    layout->addWidget(splitter);

    // ═══ 左侧面板:搜索栏 + 会话列表 + 操作按钮 ═══
    auto *leftPanel = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(4, 4, 4, 4);
    leftLayout->setSpacing(8);

    auto *searchLayout = new QHBoxLayout;
    m_searchInput = new QLineEdit(leftPanel);
    m_searchInput->setPlaceholderText(tr("搜索对话内容..."));
    connect(m_searchInput, &QLineEdit::returnPressed,
            this, &HermesMemoryWidget::onSearchClicked);
    searchLayout->addWidget(m_searchInput);

    m_searchScopeCombo = new QComboBox(leftPanel);
    m_searchScopeCombo->addItem(tr("全部"), QStringLiteral("all"));
    m_searchScopeCombo->addItem(tr("按会话"), QStringLiteral("session"));
    m_searchScopeCombo->addItem(tr("按角色"), QStringLiteral("role"));
    m_searchScopeCombo->setFixedWidth(80);
    searchLayout->addWidget(m_searchScopeCombo);

    auto *searchBtn = new QPushButton(tr("搜索"), leftPanel);
    connect(searchBtn, &QPushButton::clicked,
            this, &HermesMemoryWidget::onSearchClicked);
    searchLayout->addWidget(searchBtn);
    leftLayout->addLayout(searchLayout);

    m_sessionTable = new QTableWidget(leftPanel);
    m_sessionTable->setColumnCount(6);
    m_sessionTable->setHorizontalHeaderLabels(
        {QStringLiteral("ID"), tr("标题"), tr("时间"), tr("来源"),
         tr("模型"), tr("Token数")});
    m_sessionTable->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    m_sessionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_sessionTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_sessionTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_sessionTable->setAlternatingRowColors(true);
    m_sessionTable->verticalHeader()->setVisible(false);
    // 隐藏 ID 列(仅内部引用)
    m_sessionTable->setColumnHidden(0, true);
    connect(m_sessionTable, &QTableWidget::itemSelectionChanged,
            this, &HermesMemoryWidget::onSessionSelected);
    leftLayout->addWidget(m_sessionTable);

    auto *btnLayout = new QHBoxLayout;
    auto *refreshBtn = new QPushButton(tr("刷新"), leftPanel);
    connect(refreshBtn, &QPushButton::clicked,
            this, &HermesMemoryWidget::onRefreshClicked);
    btnLayout->addWidget(refreshBtn);

    auto *exportBtn = new QPushButton(tr("导出"), leftPanel);
    connect(exportBtn, &QPushButton::clicked,
            this, &HermesMemoryWidget::onExportClicked);
    btnLayout->addWidget(exportBtn);

    auto *deleteBtn = new QPushButton(tr("删除"), leftPanel);
    connect(deleteBtn, &QPushButton::clicked,
            this, &HermesMemoryWidget::onDeleteClicked);
    btnLayout->addWidget(deleteBtn);

    btnLayout->addStretch();
    leftLayout->addLayout(btnLayout);

    splitter->addWidget(leftPanel);

    // ═══ 右侧面板:对话详情 / Memory 文件 ═══
    m_rightTab = new QTabWidget(splitter);

    m_detailBrowser = new QTextBrowser(m_rightTab);
    m_detailBrowser->setOpenExternalLinks(false);
    m_detailBrowser->setPlaceholderText(tr("选择左侧会话查看详情..."));
    m_rightTab->addTab(m_detailBrowser, tr("对话详情"));

    m_memoryTab = new QTabWidget(m_rightTab);
    for (const char *name : kMemoryFiles) {
        const QString filename = QString::fromLatin1(name);
        auto *editorWidget = new QWidget(m_memoryTab);
        auto *editorLayout = new QVBoxLayout(editorWidget);
        editorLayout->setContentsMargins(4, 4, 4, 4);

        auto *editor = new QPlainTextEdit(editorWidget);
        editor->setPlaceholderText(tr("文件内容: %1").arg(filename));
        editorLayout->addWidget(editor);

        auto *saveBtn = new QPushButton(tr("保存"), editorWidget);
        connect(saveBtn, &QPushButton::clicked, this,
                [this, filename, editor]() {
                    saveMemoryFile(filename, editor->toPlainText());
                });
        auto *saveLayout = new QHBoxLayout;
        saveLayout->addStretch();
        saveLayout->addWidget(saveBtn);
        editorLayout->addLayout(saveLayout);

        m_memoryEditors.insert(filename, editor);
        m_memoryTab->addTab(editorWidget, filename);
    }
    m_rightTab->addTab(m_memoryTab, tr("Memory 文件"));

    splitter->addWidget(m_rightTab);

    // 左 40% / 右 60%
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 6);
}

// ---------------------------------------------------------------------------
// background loaders
// ---------------------------------------------------------------------------

void HermesMemoryWidget::schedule(std::function<void()> job)
{
    for (int i = m_futures.size() - 1; i >= 0; --i) {
        if (m_futures.at(i).isFinished())
            m_futures.removeAt(i);
    }
    m_futures.append(QtConcurrent::run(std::move(job)));
}

void HermesMemoryWidget::postError(const QString &message)
{
    QMetaObject::invokeMethod(this, [this, message]() { onError(message); },
                              Qt::QueuedConnection);
}

// 对应Python: MemoryQueryWorker._load_sessions
void HermesMemoryWidget::loadSessions()
{
    if (!m_backend)
        return;
    HermesBackend *backend = m_backend;
    schedule([this, backend]() {
        const QString dbPath =
            backend->hermesHome() + QStringLiteral("/state.db");
        if (!backend->fileExists(dbPath)) {
            postError(tr("数据库未找到: state.db 不存在"));
            return;
        }
        const QString sql = QStringLiteral(
            "SELECT s.id, "
            "COALESCE(NULLIF(s.title, ''), "
            "(SELECT substr(m.content, 1, 60) FROM messages m "
            "WHERE m.session_id = s.id AND m.role = 'user' AND m.content IS NOT NULL "
            "ORDER BY m.timestamp ASC LIMIT 1), s.id) as title, "
            "s.started_at, s.source, s.model, "
            "(COALESCE(s.input_tokens,0) + COALESCE(s.output_tokens,0)) as token_count "
            "FROM sessions s ORDER BY s.started_at DESC LIMIT 100");
        const QList<HermesBackend::SqliteRow> rows =
            backend->readSqlite(dbPath, sql);
        QMetaObject::invokeMethod(
            this, [this, rows]() { onSessionsLoaded(rows); },
            Qt::QueuedConnection);
    });
}

// 对应Python: MemoryQueryWorker._load_memory_files
void HermesMemoryWidget::loadMemoryFiles()
{
    if (!m_backend)
        return;
    HermesBackend *backend = m_backend;
    schedule([this, backend]() {
        const QString home = backend->hermesHome();
        QHash<QString, QString> files;
        for (const char *name : kMemoryFiles) {
            const QString filename = QString::fromLatin1(name);
            const QString path = home + QLatin1Char('/') + filename;
            files.insert(filename, backend->fileExists(path)
                                       ? backend->readFile(path)
                                       : QString());
        }
        QMetaObject::invokeMethod(
            this, [this, files]() { onFilesLoaded(files); },
            Qt::QueuedConnection);
    });
}

// 对应Python: MemoryQueryWorker._load_messages
void HermesMemoryWidget::showSessionDetail(const QString &sessionId)
{
    if (!m_backend || sessionId.isEmpty())
        return;
    HermesBackend *backend = m_backend;
    schedule([this, backend, sessionId]() {
        const QString dbPath =
            backend->hermesHome() + QStringLiteral("/state.db");
        if (!backend->fileExists(dbPath)) {
            postError(tr("数据库未找到: state.db 不存在"));
            return;
        }
        const QString sql = QStringLiteral(
            "SELECT timestamp as created_at, role, content FROM messages "
            "WHERE session_id = '%1' ORDER BY timestamp ASC").arg(sessionId);
        const QList<HermesBackend::SqliteRow> rows =
            backend->readSqlite(dbPath, sql);
        QMetaObject::invokeMethod(
            this, [this, rows]() { onMessagesLoaded(rows); },
            Qt::QueuedConnection);
    });
}

// 对应Python: MemoryQueryWorker._search_messages(FTS 优先,LIKE 回退)
void HermesMemoryWidget::searchMessages(const QString &query)
{
    if (!m_backend || query.isEmpty())
        return;
    HermesBackend *backend = m_backend;
    schedule([this, backend, query]() {
        const QString dbPath =
            backend->hermesHome() + QStringLiteral("/state.db");
        if (!backend->fileExists(dbPath)) {
            postError(tr("数据库未找到: state.db 不存在"));
            return;
        }
        QString safeQuery = query;
        safeQuery.replace(QLatin1Char('\''), QLatin1String("''"));
        const QString sqlFts = QStringLiteral(
            "SELECT m.timestamp as created_at, m.role, m.content, s.title, "
            "s.id as session_id "
            "FROM messages m JOIN sessions s ON m.session_id = s.id "
            "WHERE m.id IN (SELECT rowid FROM messages_fts WHERE content MATCH '%1') "
            "ORDER BY m.timestamp DESC LIMIT 50").arg(safeQuery);
        QList<HermesBackend::SqliteRow> rows =
            backend->readSqlite(dbPath, sqlFts);
        if (rows.isEmpty()) {
            // FTS 表可能不存在,回退 LIKE
            const QString sqlLike = QStringLiteral(
                "SELECT m.timestamp as created_at, m.role, m.content, s.title, "
                "s.id as session_id "
                "FROM messages m JOIN sessions s ON m.session_id = s.id "
                "WHERE m.content LIKE '%") + safeQuery + QStringLiteral("%' "
                "ORDER BY m.timestamp DESC LIMIT 50");
            rows = backend->readSqlite(dbPath, sqlLike);
        }
        QMetaObject::invokeMethod(
            this, [this, rows]() { onSearchResults(rows); },
            Qt::QueuedConnection);
    });
}

// 对应Python: MemoryWidget._export_session
void HermesMemoryWidget::exportSession(const QString &sessionId)
{
    if (!m_backend || sessionId.isEmpty())
        return;
    HermesBackend *backend = m_backend;
    schedule([this, backend, sessionId]() {
        const QString home = backend->hermesHome();
        const QString dbPath = home + QStringLiteral("/state.db");

        const QList<HermesBackend::SqliteRow> sessionInfo = backend->readSqlite(
            dbPath, QStringLiteral("SELECT * FROM sessions WHERE id = '%1'")
                        .arg(sessionId));
        const QList<HermesBackend::SqliteRow> messages = backend->readSqlite(
            dbPath, QStringLiteral(
                "SELECT timestamp as created_at, role, content FROM messages "
                "WHERE session_id = '%1' ORDER BY timestamp ASC").arg(sessionId));

        QJsonObject sessionObj;
        if (!sessionInfo.isEmpty()) {
            const auto &cols = sessionInfo.first().columns;
            for (auto it = cols.constBegin(); it != cols.constEnd(); ++it)
                sessionObj.insert(it.key(), it.value());
        }
        QJsonArray msgArray;
        for (const HermesBackend::SqliteRow &row : messages) {
            QJsonObject obj;
            for (auto it = row.columns.constBegin();
                 it != row.columns.constEnd(); ++it)
                obj.insert(it.key(), it.value());
            msgArray.append(obj);
        }
        QJsonObject exportData;
        exportData.insert(QStringLiteral("session"), sessionObj);
        exportData.insert(QStringLiteral("messages"), msgArray);

        const QString exportPath = home + QStringLiteral("/exports/session_")
            + sessionId + QStringLiteral(".json");
        const bool ok = backend->writeFile(
            exportPath, QString::fromUtf8(
                QJsonDocument(exportData).toJson(QJsonDocument::Indented)));

        QMetaObject::invokeMethod(this, [this, ok, exportPath]() {
            if (ok)
                QMessageBox::information(this, tr("导出成功"),
                                         tr("会话已导出到:\n%1").arg(exportPath));
            else
                QMessageBox::warning(this, tr("导出失败"),
                                     tr("写入文件失败:\n%1").arg(exportPath));
        }, Qt::QueuedConnection);
    });
}

// 对应Python: MemoryWidget._delete_session + MemoryQueryWorker._delete_session
void HermesMemoryWidget::deleteSession(const QString &sessionId)
{
    if (!m_backend || sessionId.isEmpty())
        return;
    const auto reply = QMessageBox::question(
        this, tr("确认删除"),
        tr("确定要删除选中的会话吗？此操作不可恢复。"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    HermesBackend *backend = m_backend;
    schedule([this, backend, sessionId]() {
        const QString dbPath =
            backend->hermesHome() + QStringLiteral("/state.db");
        if (!backend->fileExists(dbPath)) {
            postError(tr("数据库未找到: state.db 不存在"));
            return;
        }
        backend->readSqlite(dbPath,
                            QStringLiteral(
                                "DELETE FROM messages WHERE session_id = '%1'")
                                .arg(sessionId));
        backend->readSqlite(dbPath,
                            QStringLiteral("DELETE FROM sessions WHERE id = '%1'")
                                .arg(sessionId));
        QMetaObject::invokeMethod(this, [this]() { loadSessions(); },
                                  Qt::QueuedConnection);
    });
}

// 对应Python: MemoryWidget._save_memory_file
void HermesMemoryWidget::saveMemoryFile(const QString &filename,
                                        const QString &content)
{
    if (!m_backend)
        return;
    HermesBackend *backend = m_backend;
    schedule([this, backend, filename, content]() {
        const QString path =
            backend->hermesHome() + QLatin1Char('/') + filename;
        const bool ok = backend->writeFile(path, content);
        QMetaObject::invokeMethod(this, [this, ok, filename]() {
            if (ok)
                QMessageBox::information(this, tr("保存成功"),
                                         tr("%1 已保存").arg(filename));
            else
                QMessageBox::warning(this, tr("保存失败"),
                                     tr("保存 %1 失败").arg(filename));
        }, Qt::QueuedConnection);
    });
}

// ---------------------------------------------------------------------------
// UI-thread result handlers
// ---------------------------------------------------------------------------

// 对应Python: MemoryWidget._on_sessions_loaded
void HermesMemoryWidget::onSessionsLoaded(
    const QList<HermesBackend::SqliteRow> &sessions)
{
    m_sessionTable->setRowCount(0);
    for (const HermesBackend::SqliteRow &row : sessions) {
        const int idx = m_sessionTable->rowCount();
        m_sessionTable->insertRow(idx);
        m_sessionTable->setItem(idx, 0, new QTableWidgetItem(
            row.columns.value(QStringLiteral("id"))));
        m_sessionTable->setItem(idx, 1, new QTableWidgetItem(
            row.columns.value(QStringLiteral("title"))));
        m_sessionTable->setItem(idx, 2, new QTableWidgetItem(
            formatSessionTime(row.columns.value(QStringLiteral("started_at")))));
        m_sessionTable->setItem(idx, 3, new QTableWidgetItem(
            row.columns.value(QStringLiteral("source"))));
        m_sessionTable->setItem(idx, 4, new QTableWidgetItem(
            row.columns.value(QStringLiteral("model"))));
        m_sessionTable->setItem(idx, 5, new QTableWidgetItem(
            row.columns.value(QStringLiteral("token_count"),
                              QStringLiteral("0"))));
    }
}

// 对应Python: MemoryWidget._on_messages_loaded
void HermesMemoryWidget::onMessagesLoaded(
    const QList<HermesBackend::SqliteRow> &messages)
{
    if (messages.isEmpty())
        m_detailBrowser->setHtml(
            QStringLiteral("<p style=\"color: #7f8c8d;\">%1</p>")
                .arg(tr("该会话暂无消息记录")));
    else
        m_detailBrowser->setHtml(renderMessagesHtml(messages));
    // 切换到对话详情 Tab
    m_rightTab->setCurrentIndex(0);
}

// 对应Python: MemoryWidget._on_search_results
void HermesMemoryWidget::onSearchResults(
    const QList<HermesBackend::SqliteRow> &messages)
{
    QStringList htmlParts;
    htmlParts << QStringLiteral("<h3>%1 (%2 %3)</h3>")
                     .arg(tr("搜索结果"))
                     .arg(messages.size())
                     .arg(tr("条"));
    for (const HermesBackend::SqliteRow &row : messages) {
        const QString timeStr =
            formatMessageTime(row.columns.value(QStringLiteral("created_at")));
        const QString role = row.columns.value(QStringLiteral("role"));
        const QString sessionTitle = row.columns.value(QStringLiteral("title"));
        const QString sessionId =
            row.columns.value(QStringLiteral("session_id"));
        QString contentEscaped =
            escapeHtml(row.columns.value(QStringLiteral("content")));
        contentEscaped.replace(QLatin1Char('\n'), QLatin1String("<br>"));

        QString color;
        if (role == QLatin1String("user"))
            color = QStringLiteral("#2980b9");
        else if (role == QLatin1String("assistant"))
            color = QStringLiteral("#27ae60");
        else
            color = QStringLiteral("#7f8c8d");

        htmlParts << QStringLiteral(
            "<div style=\"margin-bottom: 10px; padding: 8px; "
            "border-left: 3px solid %1; background: rgba(0,0,0,0.02);\">"
            "<div style=\"color: #555; font-size: 11px; margin-bottom: 2px;\">"
            "%2</div>"
            "<div style=\"color: %1; font-weight: bold;\">[%3] %4</div>"
            "<div style=\"white-space: pre-wrap;\">%5</div>"
            "</div>")
            .arg(color,
                 tr("会话: %1 (ID: %2)")
                     .arg(escapeHtml(sessionTitle), escapeHtml(sessionId)),
                 timeStr, role, contentEscaped);
    }
    m_detailBrowser->setHtml(htmlParts.join(QString()));
    m_rightTab->setCurrentIndex(0);
}

// 对应Python: MemoryWidget._on_files_loaded
void HermesMemoryWidget::onFilesLoaded(const QHash<QString, QString> &files)
{
    for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
        QPlainTextEdit *editor = m_memoryEditors.value(it.key());
        if (editor)
            editor->setPlainText(it.value());
    }
}

// 对应Python: MemoryWidget._on_error
void HermesMemoryWidget::onError(const QString &message)
{
    qWarning() << "HermesMemoryWidget error:" << message;
    m_detailBrowser->setHtml(
        QStringLiteral("<p style=\"color: #e74c3c; font-weight: bold;\">%1</p>")
            .arg(escapeHtml(message)));
}

// ---------------------------------------------------------------------------
// UI events
// ---------------------------------------------------------------------------

QString HermesMemoryWidget::selectedSessionId() const
{
    const QModelIndexList rows =
        m_sessionTable->selectionModel()->selectedRows();
    if (rows.isEmpty())
        return QString();
    const QTableWidgetItem *item = m_sessionTable->item(rows.first().row(), 0);
    return item ? item->text() : QString();
}

// 对应Python: MemoryWidget._on_session_selected
void HermesMemoryWidget::onSessionSelected()
{
    const QString sessionId = selectedSessionId();
    if (!sessionId.isEmpty())
        showSessionDetail(sessionId);
}

// 对应Python: MemoryWidget._on_search_clicked
void HermesMemoryWidget::onSearchClicked()
{
    const QString query = m_searchInput->text().trimmed();
    if (!query.isEmpty())
        searchMessages(query);
}

// 对应Python: MemoryWidget._on_refresh_clicked
void HermesMemoryWidget::onRefreshClicked()
{
    loadSessions();
    loadMemoryFiles();
}

// 对应Python: MemoryWidget._on_export_clicked
void HermesMemoryWidget::onExportClicked()
{
    const QString sessionId = selectedSessionId();
    if (sessionId.isEmpty()) {
        QMessageBox::information(this, tr("提示"), tr("请先选择一个会话"));
        return;
    }
    exportSession(sessionId);
}

// 对应Python: MemoryWidget._on_delete_clicked
void HermesMemoryWidget::onDeleteClicked()
{
    const QString sessionId = selectedSessionId();
    if (sessionId.isEmpty()) {
        QMessageBox::information(this, tr("提示"), tr("请先选择一个会话"));
        return;
    }
    deleteSession(sessionId);
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// started_at 可能为 Unix 时间戳,转为 "yyyy-MM-dd HH:mm"
QString HermesMemoryWidget::formatSessionTime(const QString &startedAt)
{
    bool ok = false;
    const double secs = startedAt.toDouble(&ok);
    if (ok && secs > 0)
        return QDateTime::fromSecsSinceEpoch(qint64(secs))
            .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    return startedAt;
}

// message created_at 时间戳转为 "HH:mm:ss"
QString HermesMemoryWidget::formatMessageTime(const QString &createdAt)
{
    bool ok = false;
    const double secs = createdAt.toDouble(&ok);
    if (ok && secs > 0)
        return QDateTime::fromSecsSinceEpoch(qint64(secs))
            .toString(QStringLiteral("HH:mm:ss"));
    return createdAt;
}

QString HermesMemoryWidget::escapeHtml(const QString &text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('&'), QLatin1String("&amp;"))
           .replace(QLatin1Char('<'), QLatin1String("&lt;"))
           .replace(QLatin1Char('>'), QLatin1String("&gt;"));
    return escaped;
}

// 对应Python: MemoryWidget._on_messages_loaded 的 HTML 拼装部分
QString HermesMemoryWidget::renderMessagesHtml(
    const QList<HermesBackend::SqliteRow> &messages) const
{
    QStringList htmlParts;
    htmlParts << QStringLiteral(
        "<style>"
        ".msg-block { margin-bottom: 14px; padding: 10px 12px; border-radius: 6px; }"
        ".msg-user { background: #1a3a5c; border-left: 4px solid #3498db; }"
        ".msg-assistant { background: #1a3c2a; border-left: 4px solid #2ecc71; }"
        ".msg-tool { background: #2a2a2a; border-left: 4px solid #7f8c8d; "
        "  font-family: \"SF Mono\", \"Menlo\", \"Monaco\", monospace; font-size: 11px; "
        "  max-height: 120px; overflow: hidden; color: #999; }"
        ".msg-system { background: #2a1a3a; border-left: 4px solid #9b59b6; }"
        ".msg-header { font-weight: bold; margin-bottom: 6px; font-size: 12px; }"
        ".msg-content { white-space: pre-wrap; word-break: break-word; line-height: 1.5; }"
        ".tool-label { display: inline-block; background: #444; color: #aaa; "
        "  padding: 1px 6px; border-radius: 3px; font-size: 10px; margin-left: 8px; }"
        ".tool-hint { color: #666; font-style: italic; font-size: 11px; margin-top: 4px; }"
        "</style>");

    for (const HermesBackend::SqliteRow &row : messages) {
        const QString timeStr =
            formatMessageTime(row.columns.value(QStringLiteral("created_at")));
        const QString role = row.columns.value(QStringLiteral("role"));
        const QString content = row.columns.value(QStringLiteral("content"));
        const QString contentEscaped = escapeHtml(content);

        if (role == QLatin1String("user")
            || role == QLatin1String("assistant")) {
            const bool isUser = role == QLatin1String("user");
            const QString cssClass = isUser ? QStringLiteral("msg-user")
                                            : QStringLiteral("msg-assistant");
            const QString roleLabel = isUser ? QStringLiteral("👤 User")
                                             : QStringLiteral("🤖 Assistant");
            const QString headerColor = isUser ? QStringLiteral("#3498db")
                                               : QStringLiteral("#2ecc71");
            QString displayContent = contentEscaped;
            displayContent.replace(QLatin1Char('\n'), QLatin1String("<br>"));
            htmlParts << QStringLiteral(
                "<div class=\"msg-block %1\">"
                "<div class=\"msg-header\" style=\"color: %2;\">"
                "%3 <span style=\"font-weight:normal;color:#888;font-size:11px;\">%4</span></div>"
                "<div class=\"msg-content\">%5</div>"
                "</div>")
                .arg(cssClass, headerColor, roleLabel, timeStr, displayContent);
        } else if (role == QLatin1String("tool")) {
            // 工具输出:等宽字体,超 500 字符截断并给出提示
            QString truncated;
            QString hint;
            if (content.size() > 500) {
                truncated = contentEscaped.left(500) + QStringLiteral("...");
                hint = QStringLiteral("<div class=\"tool-hint\">%1</div>")
                           .arg(tr("（共 %1 字符，已截断显示）").arg(content.size()));
            } else {
                truncated = contentEscaped;
            }
            truncated.replace(QLatin1Char('\n'), QLatin1String("<br>"));
            htmlParts << QStringLiteral(
                "<div class=\"msg-block msg-tool\">"
                "<div class=\"msg-header\" style=\"color: #7f8c8d;\">"
                "🔧 Tool <span style=\"font-weight:normal;font-size:11px;\">%1</span></div>"
                "<div class=\"msg-content\">%2</div>"
                "%3"
                "</div>")
                .arg(timeStr, truncated, hint);
        } else if (role == QLatin1String("system")) {
            // 系统消息超 300 字符截断
            QString displayContent;
            if (content.size() > 300) {
                displayContent = contentEscaped.left(300);
                displayContent.replace(QLatin1Char('\n'), QLatin1String("<br>"));
                displayContent += QStringLiteral("...");
            } else {
                displayContent = contentEscaped;
                displayContent.replace(QLatin1Char('\n'), QLatin1String("<br>"));
            }
            htmlParts << QStringLiteral(
                "<div class=\"msg-block msg-system\">"
                "<div class=\"msg-header\" style=\"color: #9b59b6;\">"
                "⚙️ System <span style=\"font-weight:normal;color:#888;font-size:11px;\">%1</span></div>"
                "<div class=\"msg-content\" style=\"font-size:12px;color:#bbb;\">%2</div>"
                "</div>")
                .arg(timeStr, displayContent);
        } else {
            // 其他角色(如 function)
            QString displayContent = contentEscaped.left(300);
            displayContent.replace(QLatin1Char('\n'), QLatin1String("<br>"));
            htmlParts << QStringLiteral(
                "<div class=\"msg-block\" style=\"background:#2a2a2a;border-left:4px solid #95a5a6;\">"
                "<div class=\"msg-header\" style=\"color: #95a5a6;\">"
                "%1 <span style=\"font-weight:normal;color:#888;font-size:11px;\">%2</span></div>"
                "<div class=\"msg-content\" style=\"font-size:12px;color:#aaa;\">%3</div>"
                "</div>")
                .arg(escapeHtml(role), timeStr, displayContent);
        }
    }
    return htmlParts.join(QString());
}

} // namespace cubeshell
