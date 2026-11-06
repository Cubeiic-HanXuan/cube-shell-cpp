#pragma once
#include "claude_code/ClaudeSkillData.h"
#include <QPointer>
#include <QWidget>

class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QTreeWidget;
class QPlainTextEdit;
class QListWidget;

namespace cubeshell {
class ClaudeCodeBackend;
class SkillEditDialog;

class SkillsPanel : public QWidget {
    Q_OBJECT
public:
    explicit SkillsPanel(ClaudeCodeBackend *backend, QWidget *parent = nullptr);
    void refresh();
    void setConnectionLabel(const QString &label) { m_connectionLabel = label; }
protected:
    void changeEvent(QEvent *event) override;
private:
    ClaudeSkillScope scope() const;
    void resetContext();
    void updateButtons();
    void filter();
    void selectSkill();
    void openEditor(bool creating);
    void importSkill(bool directory);
    void exportSkill();
    void toggleSkill();
    void deleteSkill();
    quint64 beginOperation(const QString &label);
    void setStatus(const QString &text);
    void onListed(quint64 source, quint64 request, const QList<ClaudeSkillInfo> &entries, const QString &root, const QString &error);
    void onLoaded(quint64 source, quint64 request, const ClaudeSkillInfo &info, const QStringList &files, const QString &error);
    void onOperation(quint64 source, quint64 request, const QString &error);

    ClaudeCodeBackend *m_backend;
    quint64 m_source = 0;
    quint64 m_context = 0;
    quint64 m_nextRequest = 0;
    quint64 m_listRequest = 0;
    quint64 m_detailRequest = 0;
    quint64 m_writeRequest = 0;
    bool m_listing = false;
    bool m_reading = false;
    bool m_writing = false;
    bool m_ready = false;
    bool m_detailReady = false;
    QString m_connectionLabel;
    QString m_operation;
    QString m_restoreKey;
    ClaudeSkillInfo m_current;
    QPointer<SkillEditDialog> m_editor;
    QComboBox *m_scope = nullptr;
    QLineEdit *m_project = nullptr;
    QLineEdit *m_search = nullptr;
    QPushButton *m_browse = nullptr;
    QPushButton *m_refresh = nullptr;
    QPushButton *m_add = nullptr;
    QPushButton *m_import = nullptr;
    QPushButton *m_edit = nullptr;
    QPushButton *m_toggle = nullptr;
    QPushButton *m_export = nullptr;
    QPushButton *m_delete = nullptr;
    QTreeWidget *m_list = nullptr;
    QLabel *m_path = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_detail = nullptr;
    QPlainTextEdit *m_document = nullptr;
    QListWidget *m_files = nullptr;
};
} // namespace cubeshell
