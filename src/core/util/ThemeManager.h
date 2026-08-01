#pragma once

// ThemeManager.h — 主题配置加载与全局样式应用。
// 对应Python: function/theme.py + cube-shell.py::setDarkTheme/setLightTheme/applyAppearance
//
// Python 侧全局样式由 pyqtdarktheme(qdarktheme.load_stylesheet) 生成，
// dark 主题 primary 色 #00A1FF、light 主题 primary 色 #E05B00
//（见 cube-shell.py setDarkTheme/setLightTheme 的 custom_colors）。
// C++ 侧直接使用从同一 venv 导出的 qdarktheme 真实 QSS + SVG 图标
//（cpp/resources/qdarktheme/，由 export_from_venv.py 生成并编进二进制，
// 主色已在导出时注入），与 Python 版视觉一比一一致；资源缺失时回退到
// 内置自绘 QSS。
//
// conf/theme.json 现有格式保持不变（直接复用该文件）：
// { "font", "theme", "theme_color", "appearance", "font_size",
//   "terminal_theme", "language", "version" }

#include <QJsonObject>
#include <QPalette>
#include <QString>
#include <QStringList>

class QApplication;

namespace cubeshell {

class ThemeManager {
public:
    // 外观枚举（theme.json 的 "appearance" 字段："dark"/"light"）
    // 对应Python: cube-shell.py::applyAppearance 的 dark/light 分支
    enum class Appearance {
        Dark,
        Light,
    };

    // 支持的主题名查询（"dark"/"light"）
    static QStringList availableThemes();
    // "light"（不区分大小写）→ Light，其余 → Dark（与 Python 判断逻辑一致）
    // 对应Python: cube-shell.py::applyAppearance
    static Appearance appearanceFromString(const QString &name);
    static QString appearanceToString(Appearance appearance);

    // 加载 conf/theme.json；失败时保留默认值并返回 false
    // 对应Python: function/theme.py::MainWindow._load_current_settings
    bool load(const QString &themeJsonPath, QString *errorOut = nullptr);

    // 把当前配置写回 theme.json（保持原有键与格式）
    // 对应Python: function/theme.py::MainWindow._set_appearance 中 util.write_json
    bool save(QString *errorOut = nullptr) const;

    // 配置项访问（键名与 theme.json 一一对应）
    QString fontFamily() const { return m_fontFamily; }       // "font"
    int fontSize() const { return m_fontSize; }               // "font_size"
    Appearance appearance() const { return m_appearance; }    // "appearance"
    QString terminalTheme() const { return m_terminalTheme; } // "terminal_theme"
    QString themeColor() const { return m_themeColor; }       // "theme_color"
    QString language() const { return m_language; }           // "language"
    QString version() const { return m_version; }             // "version"

    // 修改配置（内存态，save() 落盘）
    // 对应Python: function/theme.py::MainWindow._set_appearance
    void setAppearance(Appearance appearance) { m_appearance = appearance; }
    // 对应Python: function/theme.py::MainWindow.apply_font_settings
    void setFont(const QString &family, int pointSize)
    {
        m_fontFamily = family;
        m_fontSize = pointSize;
    }

    // 生成全局 QSS（qdarktheme 导出的真实 qss，主色已注入）
    // 对应Python: cube-shell.py::setDarkTheme / setLightTheme
    static QString styleSheet(Appearance appearance);
    // 生成配套 QPalette（qdarktheme 同时会设置 palette）
    static QPalette palette(Appearance appearance);

    // 应用主题到整个应用（setStyleSheet + setPalette）
    // 对应Python: cube-shell.py::applyAppearance
    static void applyTheme(QApplication *app, Appearance appearance);
    static void applyTheme(QApplication *app, const QString &themeName);

private:
    QString m_jsonPath;
    // 未加载配置时的默认值与 conf/theme.json 当前内容保持一致
    QString m_fontFamily = QStringLiteral("Monaco");
    QString m_theme = QStringLiteral("rrt");                  // "theme"
    QString m_themeColor = QStringLiteral("#000000");
    Appearance m_appearance = Appearance::Dark;
    int m_fontSize = 14;
    QString m_terminalTheme = QStringLiteral("Tango");
    QString m_language = QStringLiteral("zh_CN");
    QString m_version;
    QJsonObject m_raw; // 保留未知键，写回时不丢失
};

} // namespace cubeshell
