#pragma once

// SettingsDialog.h — 设置对话框（Tab 式布局：主题 / 语言 / 通用）。
// 对应Python: function/theme.py::MainWindow（主题设置窗）
//           + cube-shell.py::show_language_settings（语言设置）
// 主题选中即实时预览（ThemeManager），Cancel 撤销预览；
// OK 保存到 QSettings，并同步 GlobalState（theme.json，与 Python 版共享）。

#include <QDialog>

#include "ProxySettingsWidget.h"

class QCheckBox;
class QComboBox;
class QFontComboBox;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QTabWidget;

namespace cubeshell {

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr);

    // 打开后选中指定 Tab（0=主题 1=语言 2=通用 3=代理），供不同菜单入口定位。
    void setCurrentTab(int index);

    // --- 代理页 ------------------------------------------------------------
    //
    // 全局代理的**配置**由本对话框自己读写（GlobalState/theme.json），但
    // **口令**与**跳板机候选**都要设备存储才拿得到，而这个对话框刻意不认识
    // DeviceConfigStore——由 MainWindow 喂进来 / 取回去，与 AddDeviceDialog
    // 那套 resolver 同一个路子。
    void setProxyDeviceCatalog(const QList<ProxyDeviceItem> &devices);
    void setHasStoredProxyPassword(bool has);
    // accept() 之后由 MainWindow 落进钥匙串：只有 edited 为真才该动已存的那份
    //（空口令框是"没改"，不是"清空"）。
    bool proxyPasswordEdited() const;
    QString proxyPassword() const;

    // OK：保存并应用（外观即时应用；语言重启后全量生效）。
    void accept() override;
    // Cancel/Esc：撤销主题实时预览，恢复进入对话框时的外观。
    void reject() override;

    // 用户选择的 SSH 命令超时秒数（供上层 CommandExecutor 使用）。
    int sshTimeoutSeconds() const;
    // 默认终端编码（UTF-8 / GBK / GB2312）。
    QString terminalEncoding() const;

signals:
    // 外观已切换（"dark"/"light"），主窗口可刷新自绘部件。
    void appearanceChanged(const QString &appearance);
    // 字体设置已修改（family, pointSize），终端可即时应用。
    void fontChanged(const QString &family, int pointSize);
    // 设备列表字号已修改，设备列表可即时应用。
    void deviceListFontSizeChanged(int pointSize);
    // 回滚行数已修改，已打开的终端可即时应用（setHistorySize）。
    void scrollbackLinesChanged(int lines);
    // 命令补全开关已修改，已打开的终端可即时生效（无需重开会话）。
    void commandCompletionEnabledChanged(bool enabled);

private:
    QWidget *createThemeTab();
    QWidget *createLanguageTab();
    QWidget *createGeneralTab();
    QWidget *createProxyTab();
    void loadCurrentSettings();

    QTabWidget *m_tabWidget = nullptr;
    QListWidget *m_themeList = nullptr;   // dark / light（选中即实时预览）
    QComboBox *m_language = nullptr;      // LanguageManager 支持列表
    QFontComboBox *m_fontFamily = nullptr;
    QSpinBox *m_fontSize = nullptr;
    QSpinBox *m_deviceListFontSize = nullptr; // 设备列表字号
    QSpinBox *m_sshTimeout = nullptr;     // 秒
    QComboBox *m_encoding = nullptr;      // UTF-8 / GBK / GB2312
    QSpinBox *m_scrollback = nullptr;     // 终端回滚行数（搜索可检索的范围）
    QCheckBox *m_commandCompletion = nullptr; // 终端命令补全开关（默认开启）
    QComboBox *m_hostKeyVerification = nullptr; // 主机密钥校验策略
    QCheckBox *m_keepaliveEnabled = nullptr;    // SSH 保活开关
    QSpinBox *m_keepaliveInterval = nullptr;    // SSH 保活间隔（秒）
    QSpinBox *m_keepaliveGrace = nullptr;       // keepalive 无应答判定断开（秒）
    // 会话日志录制（SSH/Telnet/串口 共用；C++ 独有，只写 GlobalState）
    QLineEdit *m_logDir = nullptr;              // 录制目录（空=默认 dataDir()/session-logs）
    QCheckBox *m_logAutoName = nullptr;         // 自动按 设备名+时间 命名
    QCheckBox *m_logTimestamps = nullptr;       // 行首时间戳
    QSpinBox *m_logMaxMB = nullptr;             // 单文件上限 MB（0=不轮转）
    QSpinBox *m_logBackupCount = nullptr;       // 轮转保留卷数
    ProxySettingsWidget *m_proxy = nullptr;   // 「代理」页：那份全局代理
    QString m_originalAppearance;         // Cancel 时还原实时预览
};

} // namespace cubeshell
