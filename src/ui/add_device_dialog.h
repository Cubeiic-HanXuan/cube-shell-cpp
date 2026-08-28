#pragma once

// AddDeviceDialog.h — add / edit a saved SSH / RDP / Serial / Telnet / TCP device.
//
// C++ counterpart of AddConfigUi (cube-shell.py:5989). Edits name / username /
// password-or-key / host / port (plus RDP auth/domain when built with
// CUBESHELL_WITH_RDP, plus serial port params when built with
// CUBESHELL_WITH_SERIAL, plus TCP/Telnet params unconditionally) and returns a
// DeviceEntry via device(). Used for both "add" and "edit" (setDevice to pre-fill).
//
// 协议下拉框无条件存在：TCP/Telnet 不依赖任何可选组件，所以至少有
// SSH / Telnet / TCP 三项可选。（早先这里有个 CUBESHELL_HAS_PROTOCOL_COMBO 宏，
// 定义为"RDP 或 Serial 任一开启"，在两者都关掉的鸿蒙构建上会让整个下拉框消失。）

#include <QDialog>

#include <functional>

#include "ProxySettingsWidget.h"
#include "config/DeviceConfigStore.h"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QFormLayout;
class QPushButton;
class QStackedWidget;

namespace cubeshell {

class ConnectionTester;

class AddDeviceDialog : public QDialog {
    Q_OBJECT
public:
    explicit AddDeviceDialog(QWidget *parent = nullptr);

    // Pre-fill for editing an existing entry.
    void setDevice(const DeviceEntry &entry);
    // The edited entry (valid after accept()).
    //
    // 注意 e.password 的语义：为空表示「用户没有输入新密码」，**不是**「密码被
    // 清空」。密码不再随条目落盘（见 DeviceConfigStore 的说明），编辑已有设备时
    // 密码框一开始就是空的，把空串当成"清空"会让人一改端口就丢密码。
    // 要判断用户是否真的动过密码，用 passwordEdited()。
    DeviceEntry device() const;

    // 用户是否在本次对话框里动过密码框。
    // 只有它为 true 时调用方才应该去改存储里的密码。
    bool passwordEdited() const { return m_passwordEdited; }

    // 告知对话框：这个设备在钥匙串里已经存有密码。
    // 于是密码框可以留空，占位符提示"留空则不修改"（区别于新建时的
    // "留空则在连接时输入"）。密码本身不再是必填项——见 validate()。
    void setHasStoredPassword(bool has);

    // --- 代理 ---------------------------------------------------------------
    //
    // 与密码那三个函数逐一对应。代理口令也是「留空 = 没动过」的语义，
    // 只有 proxyPasswordEdited() 为真时调用方才该去改存储里的代理口令。
    void setHasStoredProxyPassword(bool has);
    bool proxyPasswordEdited() const;

    // 跳板机下拉的候选设备（由持有 DeviceConfigStore 的 MainWindow 提供）。
    // 编辑既有设备时 excludeId 传当前设备 id，把自己从候选里摘掉。
    void setProxyDeviceCatalog(const QList<ProxyDeviceItem> &devices,
                              const QString &excludeId = QString());

    // 注入「按 id 取已存代理口令」的回调。用途同 setPasswordResolver：
    // 编辑既有设备、代理口令框留空时，「测试连接」要拿钥匙串里的真实口令去测，
    // 否则走需要认证的代理会误报失败。
    void setProxyPasswordResolver(std::function<QString(const QString &deviceId)> resolver);

    // 注入「把这些跳板机连同凭据推给建连路径」的回调。
    //
    // 「测试连接」专用，且**必须**注入，否则「跳转服务器」一测就报
    // "跳板机 xxx 已不存在（可能已被删除）"：建链在工作线程上按 id 查的是
    // GlobalState 里那份快照（见 GlobalState::setJumpHostCatalog），而快照是
    // MainWindow 扫**已保存的设备**攒出来的。对话框里刚选好还没保存的那一跳
    // 不在任何已存条目的 hopIds 里，快照里自然没有它——设备明明在侧栏里摆着，
    // 报的却是"已不存在"，极难对上。
    void setJumpHostPublisher(std::function<void(const QStringList &hopIds)> publisher);

    // 注入「按 id 取已存密码」的回调（由持有 DeviceConfigStore 的 MainWindow 提供）。
    // 测试连接时用：编辑既有设备、密码框留空（"留空则不修改"）的情况下，
    // 得拿钥匙串里的真实密码去测，否则会误报认证失败。新建设备无需设置。
    void setPasswordResolver(std::function<QString(const QString &deviceId)> resolver);

    void accept() override;

private slots:
    void onAuthMethodChanged(int index);
    void onBrowseKey();
    void onTestConnection();
    void onTestFinished(bool ok, const QString &message);

private:
    bool validate(QString *err) const;
    // 测试进行中/结束时的按钮与状态条切换（testing=true 进入探测态）。
    void setTestingUi(bool testing);

    // 当前选中的协议值（"ssh" | "rdp" | "serial" | "telnet" | "tcp"）。
    // 一律取 currentData() 而非 currentText()：显示文本是给人看的，一旦有人
    // 给协议名加上 tr() 或改个大小写，比对文本的判定就会静默失效。
    QString selectedProtocol() const;
    bool rdpSelected() const;
    bool serialSelected() const;
    bool telnetSelected() const;
    bool tcpSelected() const;

    // 对应Python: _on_protocol_changed（cube-shell.py:6073-6084）
    void onProtocolChanged(int index);
#ifdef CUBESHELL_WITH_SERIAL
    void onRefreshPorts();
#endif

    // 行显隐 / 换页之后把对话框收回到内容高度。
    // QFormLayout 隐藏行会让 sizeHint 变小，但 QDialog 不会自动跟着缩小
    //（窗口尺寸一旦被撑大就保持不变），多出来的高度会显示成一块空白。
    // 只收高度、保留当前宽度：连 adjustSize() 一起用会把用户手动拉宽的
    // 对话框每次都打回 sizeHint。
    void refitHeight();

    QFormLayout *m_form = nullptr;
    QComboBox *m_protocol = nullptr;        // Row 0: SSH | RDP | Serial | Telnet | TCP
    QLineEdit *m_name = nullptr;
    QLineEdit *m_username = nullptr;
    QLineEdit *m_host = nullptr;
    QLineEdit *m_port = nullptr;

    QComboBox *m_authMethod = nullptr;      // 0 = password, 1 = private key
    QStackedWidget *m_authStack = nullptr;

    // 代理（仅 SSH 可见——本轮只接线 SSH，见 SshClient::setProxyConfig）。
    ProxySettingsWidget *m_proxy = nullptr;

    // password page
    QLineEdit *m_password = nullptr;
    // key page
    QComboBox *m_keyType = nullptr;         // Ed25519Key / RSAKey / ECDSAKey / DSSKey
    QLineEdit *m_keyFile = nullptr;
    QPushButton *m_browseKey = nullptr;

    // 端口框里当前放的是哪个协议的默认值。切协议时据此判断"用户没改过端口"，
    // 从而可以安全地换成新协议的默认端口（用户手填过的端口不动）。
    QString m_portDefaultFor;

    // 编辑态：钥匙串里已有密码 / 用户动过密码框。见 passwordEdited() 的说明。
    bool m_hasStoredPassword = false;
    bool m_passwordEdited = false;
    // 正在编辑的条目 id。新建设备时为空，由 device() 现分配。
    QString m_id;

#ifdef CUBESHELL_WITH_RDP
    // RDP 专用控件。对应Python: _inject_protocol_fields（cube-shell.py:5989-6084）
    QComboBox *m_rdpAuth = nullptr;         // data: "ntlm" | "plain"
    QLineEdit *m_domain = nullptr;          // Windows 域（可选）
#endif
#ifdef CUBESHELL_WITH_SERIAL
    // 串口专用控件（Python 版无对应实现）。
    QWidget   *m_serialPortRow = nullptr;   // 端口下拉 + 刷新按钮的容器
    QComboBox *m_serialPort = nullptr;
    QComboBox *m_baud = nullptr;
    QComboBox *m_dataBits = nullptr;
    QComboBox *m_parity = nullptr;
    QComboBox *m_stopBits = nullptr;
    QComboBox *m_flow = nullptr;
    QComboBox *m_newline = nullptr;
    QCheckBox *m_localEcho = nullptr;
    QCheckBox *m_rxImplicitCr = nullptr;
#endif
    // TCP/Telnet 专用控件（无条件编译）。换行/回显/接收补 CR 三项与串口是
    // 同一套语义，但刻意用独立控件而非复用串口那三个——串口控件在
    // CUBESHELL_WITH_SERIAL=OFF 时不存在，而 TCP/Telnet 在任何构建里都要能用。
    QComboBox *m_netNewline = nullptr;
    QCheckBox *m_netLocalEcho = nullptr;
    QCheckBox *m_netRxImplicitCr = nullptr;
    QComboBox *m_termType = nullptr;        // Telnet TERMINAL-TYPE 上报值
    QCheckBox *m_negotiate = nullptr;       // Telnet IAC 选项协商
    QCheckBox *m_autoLogin = nullptr;       // Telnet 自动登录

    // --- 测试连接 ---
    QDialogButtonBox *m_buttonBox = nullptr; // OK/Cancel，探测期间禁用 OK
    QPushButton *m_testButton = nullptr;     // 「测试连接」/ 探测中变为「取消」
    QLabel *m_testStatus = nullptr;          // 行内结果反馈（绿✓/红✗）
    ConnectionTester *m_tester = nullptr;
    std::function<QString(const QString &)> m_passwordResolver;
    std::function<QString(const QString &)> m_proxyPasswordResolver;
    std::function<void(const QStringList &)> m_jumpHostPublisher;
};

} // namespace cubeshell
