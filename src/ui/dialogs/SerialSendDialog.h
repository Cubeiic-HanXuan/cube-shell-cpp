#pragma once

#include <QDialog>

#include "serial/SerialClient.h"

class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace cubeshell {

class SerialBridge;
class SerialLineSender;

// 非模态工具，始终绑定创建它的串口会话，不跟随主窗口的活动标签页。
class SerialSendDialog : public QDialog {
    Q_OBJECT
public:
    SerialSendDialog(SerialClient *client, SerialBridge *bridge, QWidget *parent = nullptr);
    ~SerialSendDialog() override;

    void setConnectionInfo(const QString &portName, SerialSettings::NewlineMode mode);
    void loadClipboard();
    void done(int result) override;

signals:
    void busyChanged(bool busy);

private:
    void editText();
    bool replaceText(const QString &text, bool confirmExisting);
    void updateUi();

    SerialLineSender *m_sender = nullptr;
    QLabel *m_connection = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_import = nullptr;
    QPushButton *m_edit = nullptr;
    QPushButton *m_next = nullptr;
    QPushButton *m_auto = nullptr;
    QPushButton *m_pause = nullptr;
    QPushButton *m_stop = nullptr;
    QSpinBox *m_interval = nullptr;
    QLabel *m_progress = nullptr;
    QLabel *m_status = nullptr;
    bool m_wasBusy = false;
};

} // namespace cubeshell
