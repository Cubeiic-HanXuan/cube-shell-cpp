#pragma once

// SerialConnectDialog.h — “新建串口连接”对话框。
//
// 主菜单「终端 → 新建串口连接」的入口：选端口和帧格式，确定后由 MainWindow
// 打开一个串口标签页（不落盘保存；要保存成设备走「添加设备」→ 协议选 Serial）。
//
// 顺带提供串口参数下拉框的填充/读写辅助函数——SerialTerminalWidget 的顶部
// 工具栏用的是同一套选项，映射逻辑集中在这里，避免两处各写一份。
//
// 仅在 CUBESHELL_WITH_SERIAL=ON 时编译。

#include <QDialog>

#include "config/DeviceConfigStore.h"   // DeviceEntry（serialSettingsFromDevice 用）
#include "serial/SerialClient.h"

class QCheckBox;
class QComboBox;

namespace cubeshell {

// --- 串口参数下拉框辅助（对话框与终端工具栏共用） ---
namespace serialcombo {

// 按各自的取值集合填充下拉框，并选中默认项。
void fillPorts(QComboBox *combo, const QString &preferred = QString());
void fillBaudRates(QComboBox *combo);
void fillDataBits(QComboBox *combo);
void fillParity(QComboBox *combo);
void fillStopBits(QComboBox *combo);
void fillFlowControl(QComboBox *combo);
void fillNewlineMode(QComboBox *combo);

// 按 currentData() 取值（缺省值用于下拉框为空/数据非法时兜底）。
qint32 baudRateOf(const QComboBox *combo);
QSerialPort::DataBits dataBitsOf(const QComboBox *combo);
QSerialPort::Parity parityOf(const QComboBox *combo);
QSerialPort::StopBits stopBitsOf(const QComboBox *combo);
QSerialPort::FlowControl flowControlOf(const QComboBox *combo);
SerialSettings::NewlineMode newlineModeOf(const QComboBox *combo);

// 按值选中对应项（找不到则保持不变）。
void selectData(QComboBox *combo, const QVariant &value);

// --- DeviceEntry 的字符串形态 ↔ QSerialPort 枚举 ---
// DeviceConfigStore 存字符串是为了不依赖 Qt6::SerialPort，映射收敛在这里。
QSerialPort::Parity parityFromString(const QString &s);
QString parityToString(QSerialPort::Parity p);
QSerialPort::StopBits stopBitsFromString(const QString &s);
QString stopBitsToString(QSerialPort::StopBits s);
QSerialPort::FlowControl flowControlFromString(const QString &s);
QString flowControlToString(QSerialPort::FlowControl f);
QSerialPort::DataBits dataBitsFromInt(int bits);
SerialSettings::NewlineMode newlineModeFromString(const QString &s);
QString newlineModeToString(SerialSettings::NewlineMode m);

} // namespace serialcombo

// DeviceEntry（持久化形态）→ SerialSettings（运行时形态）。
SerialSettings serialSettingsFromDevice(const DeviceEntry &device);

class SerialConnectDialog : public QDialog {
    Q_OBJECT
public:
    explicit SerialConnectDialog(QWidget *parent = nullptr);

    // 用户选定的连接参数（accept() 之后有效）。
    SerialSettings settings() const;
    void setSettings(const SerialSettings &settings);

    void accept() override;

private slots:
    void onRefreshPorts();

private:
    QComboBox *m_port = nullptr;
    QComboBox *m_baud = nullptr;
    QComboBox *m_dataBits = nullptr;
    QComboBox *m_parity = nullptr;
    QComboBox *m_stopBits = nullptr;
    QComboBox *m_flow = nullptr;
    QComboBox *m_newline = nullptr;
    QCheckBox *m_localEcho = nullptr;
};

} // namespace cubeshell
