#pragma once

// AddTunnelDialog.h — 新增/编辑 SSH 隧道配置对话框。
// 对应Python: ui/add_tunnel_config.py::Ui_AddTunnelConfig
//           + cube-shell.py::AddTunnelConfig.addTunnel

#include <QDialog>
#include <QStringList>

#include "ssh/TunnelPool.h"

class QComboBox;
class QLineEdit;

namespace cubeshell {

class AddTunnelDialog : public QDialog {
    Q_OBJECT
public:
    // deviceNames: config.dat 中的设备列表（SSH 凭据来源下拉框）。
    explicit AddTunnelDialog(const QStringList &deviceNames, QWidget *parent = nullptr);

    // 编辑模式：预填一条既有配置（名称只读）。
    void setEntry(const QString &name, const TunnelEntry &entry);

    QString tunnelName() const;
    TunnelEntry entry() const;

    void accept() override;

private slots:
    void onTypeChanged(int index);

private:
    QComboBox *m_type = nullptr;          // 本地/远程/动态
    QComboBox *m_device = nullptr;        // SSH 服务器（设备名）
    QLineEdit *m_name = nullptr;
    QLineEdit *m_remoteBind = nullptr;    // host:port
    QLineEdit *m_localBind = nullptr;     // host:port
    QLineEdit *m_browserOpen = nullptr;   // 启动后自动打开的 URL
};

} // namespace cubeshell
