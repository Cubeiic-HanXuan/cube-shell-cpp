#pragma once

// HermesMemoryWidget.h — Hermes Agent memory browser.
// 对应Python: core/hermes/memory_widget.py（MemoryWidget + MemoryQueryWorker）
//
// 左侧:会话列表 + 全文搜索;右侧:对话详情(HTML 渲染) + Memory 文件编辑。
// All database/file work runs on the global thread pool (QtConcurrent);
// results are posted back to the UI thread via queued QMetaObject::invokeMethod.

#include <QFuture>
#include <QHash>
#include <QVector>
#include <QWidget>

#include "hermes/HermesBackend.h"

#include <functional>

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QTextBrowser;

namespace cubeshell {

class HermesMemoryWidget : public QWidget {
    Q_OBJECT
public:
    explicit HermesMemoryWidget(QWidget *parent = nullptr);
    ~HermesMemoryWidget() override;

    // 对应Python: set_backend(不触发加载)
    void setBackend(HermesBackend *backend);

public slots:
    // 对应Python: refresh(Tab 激活时调用,加载会话列表 + Memory 文件)
    void refresh();

private slots:
    void onSessionSelected();
    void onSearchClicked();
    void onRefreshClicked();
    void onExportClicked();
    void onDeleteClicked();

private:
    void buildUi();

    // --- background loaders (QtConcurrent) ---
    void loadSessions();
    void loadMemoryFiles();
    void showSessionDetail(const QString &sessionId);
    void searchMessages(const QString &query);
    void exportSession(const QString &sessionId);
    void deleteSession(const QString &sessionId);
    void saveMemoryFile(const QString &filename, const QString &content);
    void schedule(std::function<void()> job);
    void postError(const QString &message);

    // --- UI-thread result handlers ---
    void onSessionsLoaded(const QList<HermesBackend::SqliteRow> &sessions);
    void onMessagesLoaded(const QList<HermesBackend::SqliteRow> &messages);
    void onSearchResults(const QList<HermesBackend::SqliteRow> &messages);
    void onFilesLoaded(const QHash<QString, QString> &files);
    void onError(const QString &message);

    // --- pure helpers ---
    QString selectedSessionId() const;
    QString renderMessagesHtml(const QList<HermesBackend::SqliteRow> &messages) const;
    static QString formatSessionTime(const QString &startedAt);
    static QString formatMessageTime(const QString &createdAt);
    static QString escapeHtml(const QString &text);

    HermesBackend *m_backend = nullptr; // not owned

    // left panel
    QLineEdit *m_searchInput = nullptr;
    QComboBox *m_searchScopeCombo = nullptr;
    QTableWidget *m_sessionTable = nullptr;
    // right panel
    QTabWidget *m_rightTab = nullptr;
    QTextBrowser *m_detailBrowser = nullptr;
    QTabWidget *m_memoryTab = nullptr;
    QHash<QString, QPlainTextEdit *> m_memoryEditors;

    QVector<QFuture<void>> m_futures; // joined in destructor
};

} // namespace cubeshell
