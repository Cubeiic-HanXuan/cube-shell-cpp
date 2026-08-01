#pragma once

// CompressDialog.h — 文件压缩对话框（文件名 + 格式）。
// 对应Python: ui/compress_dialog.py::CompressDialog

#include <QDialog>

class QComboBox;
class QLineEdit;

namespace cubeshell {

class CompressDialog : public QDialog {
    Q_OBJECT
public:
    explicit CompressDialog(QWidget *parent = nullptr,
                            const QString &defaultName = QStringLiteral("archive"));

    // 对应Python: CompressDialog.get_settings
    QString archiveName() const;
    QString format() const;    // ".tar.gz" | ".zip"
    // 完整目标文件名（name + format）。
    QString fileName() const;

private:
    QLineEdit *m_nameEdit = nullptr;
    QComboBox *m_formatCombo = nullptr;
};

} // namespace cubeshell
