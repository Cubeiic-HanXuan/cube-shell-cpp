#pragma once

// NetConnectDialog.h — “新建 Telnet 连接” / “新建 TCP 连接”对话框。
//
// 主菜单「终端 → 新建 Telnet/TCP 连接」的入口：填主机端口与终端行为，确定后
// 由 MainWindow 打开一个网络终端标签页（不落盘保存；要保存成设备走
// 「添加设备」→ 协议选 Telnet/TCP）。形态对照 SerialConnectDialog。
//
// 顺带提供 TCP/Telnet 参数下拉框的填充/读写辅助函数——NetTerminalWidget 的
// 顶部工具栏与「添加设备」对话框用的是同一套选项，映射逻辑集中在这里。
//
// 无条件编译（Qt6::Network 是顶层必需组件，鸿蒙上同样可用）。

#include <QDialog>

#include "config/DeviceConfigStore.h"   // DeviceEntry（netSettingsFromDevice 用）
#include "net/TcpClient.h"              // TcpSettings

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;

namespace cubeshell {

// --- TCP/Telnet 参数下拉框辅助（对话框、终端工具栏、添加设备对话框共用） ---
namespace netcombo {

// 模式选择："Telnet"（data "telnet"）/ "TCP"（data "tcp"）。
void fillMode(QComboBox *combo, const QString &preferred = QString());
QString modeOf(const QComboBox *combo);

// 发送换行符：CR / LF / CRLF。
void fillNewlineMode(QComboBox *combo,
                     TcpSettings::NewlineMode preferred = TcpSettings::NewlineMode::CrLf);
TcpSettings::NewlineMode newlineModeOf(const QComboBox *combo);

// 终端类型：常见取值的可编辑下拉框（TERMINAL-TYPE 子协商上报值）。
// 显示文本即取值，故 termTypeOf 直接取 currentText——没有 serialcombo 里
// 「显示文本与 userData 不同」带来的手输值丢失问题。
void fillTermTypes(QComboBox *combo, const QByteArray &preferred = QByteArray());
QByteArray termTypeOf(const QComboBox *combo);

// 按值选中对应项（找不到则保持不变）。
void selectData(QComboBox *combo, const QVariant &value);

// --- DeviceEntry 的字符串形态 ↔ TcpSettings 枚举 ---
// 与 serialcombo 的同名函数刻意分开：SerialConnectDialog 只在
// CUBESHELL_WITH_SERIAL=ON 时编译，TCP/Telnet 不能依赖它。
TcpSettings::NewlineMode newlineModeFromString(const QString &s);
QString newlineModeToString(TcpSettings::NewlineMode m);

} // namespace netcombo

// DeviceEntry（持久化形态）→ TcpSettings（运行时形态）。
TcpSettings netSettingsFromDevice(const DeviceEntry &device);

class NetConnectDialog : public QDialog {
    Q_OBJECT
public:
    // mode: "telnet" | "tcp"。决定标题与显示哪些行——两种协议的参数集合差别
    // 不小（裸 TCP 没有协商/终端类型/自动登录），做成一个带模式开关的对话框
    // 比两个几乎重复的类清楚。
    explicit NetConnectDialog(const QString &mode, QWidget *parent = nullptr);

    // 用户填写的连接参数（accept() 之后有效）。
    TcpSettings settings() const;
    void setSettings(const TcpSettings &settings);

    void accept() override;

private:
    bool isTelnet() const { return m_mode == QLatin1String("telnet"); }

    QString m_mode;

    QLineEdit *m_host = nullptr;
    QSpinBox  *m_port = nullptr;
    QComboBox *m_newline = nullptr;
    QCheckBox *m_localEcho = nullptr;
    QCheckBox *m_rxImplicitCr = nullptr;

    // 仅 Telnet 模式创建（TCP 模式下为 nullptr，各处访问前判空）。
    QLineEdit *m_username = nullptr;
    QLineEdit *m_password = nullptr;
    QComboBox *m_termType = nullptr;
    QCheckBox *m_negotiate = nullptr;
    QCheckBox *m_autoLogin = nullptr;
};

} // namespace cubeshell
