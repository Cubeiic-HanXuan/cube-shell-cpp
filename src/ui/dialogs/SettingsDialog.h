#pragma once

// SettingsDialog.h — 设置对话框（Tab 式布局：主题 / 语言 / 通用）。
// 对应Python: function/theme.py::MainWindow（主题设置窗）
//           + cube-shell.py::show_language_settings（语言设置）
// 主题选中即实时预览（ThemeManager），Cancel 撤销预览；
// OK 保存到 QSettings，并同步 GlobalState（theme.json，与 Python 版共享）。

#include <QDialog>

class QComboBox;
class QFontComboBox;
class QListWidget;
class QSpinBox;
class QTabWidget;

namespace cubeshell {

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr);

    // 打开后选中指定 Tab（0=主题 1=语言 2=通用），供不同菜单入口定位。
    void setCurrentTab(int index);

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

private:
    QWidget *createThemeTab();
    QWidget *createLanguageTab();
    QWidget *createGeneralTab();
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
    QString m_originalAppearance;         // Cancel 时还原实时预览
};

} // namespace cubeshell
