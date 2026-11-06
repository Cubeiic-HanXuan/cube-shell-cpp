#pragma once
#include <QDialog>
#include <QByteArray>

class QLineEdit;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QCloseEvent;

namespace cubeshell {
class SkillEditDialog : public QDialog {
    Q_OBJECT
public:
    SkillEditDialog(const QString &directory, const QByteArray &content, bool creating, QWidget *parent = nullptr);
    void finishSave(const QString &error);
    void invalidateSource();
    void reject() override;
signals:
    void saveRequested(const QString &directory, const QByteArray &content);
private:
    void validate();
    void submit();
    QByteArray content() const;
    QLineEdit *m_name = nullptr;
    QPlainTextEdit *m_editor = nullptr;
    QLabel *m_error = nullptr;
    QPushButton *m_save = nullptr;
    QByteArray m_original;
    QString m_initialText;
    bool m_creating = false;
    bool m_pending = false;
    bool m_invalid = false;
};
} // namespace cubeshell
