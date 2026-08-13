// ThemeManager.cpp — 主题配置加载与全局样式应用实现。
// 对应Python: function/theme.py + cube-shell.py::setDarkTheme/setLightTheme/applyAppearance
//
// 全局 QSS 来自编译进二进制的 qdarktheme 导出资源
//（cpp/resources/qdarktheme/{dark,light}.qss + svg/ 图标，由 export_from_venv.py
// 从 Python 版 venv 的 pyqtdarktheme-fork 导出，见 src/app/CMakeLists.txt），
// 与 Python 版 qdarktheme.load_stylesheet(custom_colors=...) 的输出逐字一致
//（仅 url 从临时绝对路径改写为 Qt 资源路径），品牌主色 dark #00A1FF /
// light #E05B00 已在导出时注入，无需额外 override。

#include "ThemeManager.h"

#include <QApplication>
#include <QColor>
#include <QFile>
#include <QTextStream>
#include <QtGlobal>

#include "DataParser.h"

namespace cubeshell {

namespace {

// qdarktheme 风格配色。primary 色与 Python 侧 custom_colors 一致：
// 对应Python: cube-shell.py::setDarkTheme ("[dark]": {"primary": "#00A1FF"})
// 对应Python: cube-shell.py::setLightTheme ("[light]": {"primary": "#E05B00"})
struct ThemeColors {
    QString primary;
    QString background;
    QString surface;      // 输入框/列表等控件底色
    QString border;
    QString text;
    QString textDisabled;
    QString highlightText;
    QString hover;
};

ThemeColors colorsFor(ThemeManager::Appearance appearance)
{
    if (appearance == ThemeManager::Appearance::Light) {
        // 参考 pyqtdarktheme light 主题基色
        return {
            QStringLiteral("#E05B00"), // primary
            QStringLiteral("#F8F9FA"), // background
            QStringLiteral("#FFFFFF"), // surface
            QStringLiteral("#DADCE0"), // border
            QStringLiteral("#4D5157"), // text
            QStringLiteral("#BABDC2"), // text disabled
            QStringLiteral("#FFFFFF"), // highlight text
            QStringLiteral("#EBEBEB"), // hover
        };
    }
    // 参考 pyqtdarktheme dark 主题基色
    return {
        QStringLiteral("#00A1FF"), // primary
        QStringLiteral("#202124"), // background
        QStringLiteral("#2B2D30"), // surface
        QStringLiteral("#3F4042"), // border
        QStringLiteral("#E4E7EB"), // text
        QStringLiteral("#697177"), // text disabled
        QStringLiteral("#FFFFFF"), // highlight text
        QStringLiteral("#33373B"), // hover
    };
}

// qdarktheme 导出 qss 在 Qt 资源系统中的路径。
// qrc 的 prefix 为 "/qdarktheme"（见 resources/qdarktheme/qdarktheme.qrc），
// qss 内部的 url(":/qdarktheme/svg/xxx.svg") 已在导出时改写完成，
// 因此无需二次处理即可解析图标。
QString qssResourcePath(ThemeManager::Appearance appearance)
{
    return appearance == ThemeManager::Appearance::Light
               ? QStringLiteral(":/qdarktheme/light.qss")
               : QStringLiteral(":/qdarktheme/dark.qss");
}

// 读取资源里的 qss；资源缺失（未编进目标）时返回空串。
QString readQss(const QString &resourcePath)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QTextStream stream(&file);
    return stream.readAll();
}

} // namespace

QStringList ThemeManager::availableThemes()
{
    return {QStringLiteral("dark"), QStringLiteral("light")};
}

// 对应Python: cube-shell.py::applyAppearance（"light" 走亮色，其余走暗色）
ThemeManager::Appearance ThemeManager::appearanceFromString(const QString &name)
{
    return name.trimmed().toLower() == QStringLiteral("light") ? Appearance::Light
                                                               : Appearance::Dark;
}

QString ThemeManager::appearanceToString(Appearance appearance)
{
    return appearance == Appearance::Light ? QStringLiteral("light")
                                           : QStringLiteral("dark");
}

// 对应Python: function/theme.py::MainWindow._load_current_settings
bool ThemeManager::load(const QString &themeJsonPath, QString *errorOut)
{
    QString err;
    const QJsonObject data = DataParser::readJsonFile(themeJsonPath, &err);
    if (data.isEmpty() && !err.isEmpty()) {
        if (errorOut)
            *errorOut = err;
        return false;
    }
    m_jsonPath = themeJsonPath;
    m_raw = data;

    // 加载外观设置（appearance 缺省视为 dark，与 Python 侧 `or "dark"` 一致）
    m_appearance = appearanceFromString(
        data.value(QStringLiteral("appearance")).toString(QStringLiteral("dark")));
    // 加载字体设置
    const QString savedFont = data.value(QStringLiteral("font")).toString();
    if (!savedFont.isEmpty())
        m_fontFamily = savedFont;
    // 加载字体大小（缺省 14，与 Python data.get('font_size', 14) 一致）
    m_fontSize = data.value(QStringLiteral("font_size")).toInt(14);

    m_theme = data.value(QStringLiteral("theme")).toString(m_theme);
    m_themeColor = data.value(QStringLiteral("theme_color")).toString(m_themeColor);
    m_terminalTheme =
        data.value(QStringLiteral("terminal_theme")).toString(m_terminalTheme);
    m_language = data.value(QStringLiteral("language")).toString(m_language);
    m_version = data.value(QStringLiteral("version")).toString(m_version);
    return true;
}

// 对应Python: function/theme.py::MainWindow._set_appearance 中 util.write_json
bool ThemeManager::save(QString *errorOut) const
{
    if (m_jsonPath.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("theme.json path not set (call load first)");
        return false;
    }
    // 基于原始对象更新，保留 theme.json 现有键与未知键
    QJsonObject data = m_raw;
    data.insert(QStringLiteral("font"), m_fontFamily);
    data.insert(QStringLiteral("theme"), m_theme);
    data.insert(QStringLiteral("theme_color"), m_themeColor);
    data.insert(QStringLiteral("appearance"), appearanceToString(m_appearance));
    data.insert(QStringLiteral("font_size"), m_fontSize);
    data.insert(QStringLiteral("terminal_theme"), m_terminalTheme);
    data.insert(QStringLiteral("language"), m_language);
    if (!m_version.isEmpty())
        data.insert(QStringLiteral("version"), m_version);
    return DataParser::writeJsonFile(m_jsonPath, data, errorOut);
}

namespace {

// 兜底 QSS：qdarktheme 导出资源不可用（qrc 未编进目标）时使用的自绘样式，
// 即集成前的实现，配色与官方主题近似，保证界面不会退回系统默认外观。
QString fallbackStyleSheet(ThemeManager::Appearance appearance)
{
    const ThemeColors c = colorsFor(appearance);
    return QStringLiteral(
               "QWidget {"
               "    background-color: %2;"
               "    color: %5;"
               "    selection-background-color: %1;"
               "    selection-color: %7;"
               "}"
               "QToolTip {"
               "    background-color: %3;"
               "    color: %5;"
               "    border: 1px solid %4;"
               "    padding: 2px;"
               "}"
               "QPushButton {"
               "    background-color: %3;"
               "    border: 1px solid %4;"
               "    border-radius: 4px;"
               "    padding: 4px 12px;"
               "}"
               "QPushButton:hover { background-color: %8; border-color: %1; }"
               "QPushButton:pressed { background-color: %1; color: %7; }"
               "QPushButton:disabled { color: %6; border-color: %4; }"
               "QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QDoubleSpinBox, QComboBox {"
               "    background-color: %3;"
               "    border: 1px solid %4;"
               "    border-radius: 4px;"
               "    padding: 2px 4px;"
               "}"
               "QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus,"
               "QSpinBox:focus, QComboBox:focus { border: 1px solid %1; }"
               "QComboBox QAbstractItemView {"
               "    background-color: %3;"
               "    border: 1px solid %4;"
               "    selection-background-color: %1;"
               "    selection-color: %7;"
               "}"
               "QTabWidget::pane { border: 1px solid %4; }"
               // 标签栏底色跟随窗口，避免 macOS 原生样式画出一条灰带。
               "QTabBar { background-color: %2; border: none; }"
               "QTabBar::tab {"
               "    background-color: %2;"
               "    color: %5;"
               "    padding: 6px 12px;"
               "    border: 1px solid transparent;"
               "}"
               "QTabBar::tab:selected {"
               "    color: %1;"
               "    border-bottom: 2px solid %1;"
               "}"
               "QTabBar::tab:hover { background-color: %8; }"
               // 左侧竖向图标栏：无边框、与窗口同底色（对应Python 的窄图标栏）。
               "QToolBar { background-color: %2; border: none; padding: 2px; spacing: 2px; }"
               "QToolBar::separator { background-color: %4; }"
               "QToolButton { background-color: transparent; border: none;"
               "    border-radius: 4px; padding: 4px; }"
               "QToolButton:hover { background-color: %8; }"
               "QToolButton:pressed { background-color: %4; }"
               "QStatusBar { background-color: %2; color: %5; }"
               "QMenuBar { background-color: %2; }"
               "QMenuBar::item:selected { background-color: %8; color: %1; }"
               "QMenu { background-color: %3; border: 1px solid %4; }"
               "QMenu::item:selected { background-color: %1; color: %7; }"
               "QTreeView, QListView, QTableView {"
               "    background-color: %3;"
               "    alternate-background-color: %2;"
               "    border: 1px solid %4;"
               "}"
               "QTreeView::item:selected, QListView::item:selected,"
               "QTableView::item:selected { background-color: %1; color: %7; }"
               "QHeaderView::section {"
               "    background-color: %2;"
               "    color: %5;"
               "    border: 1px solid %4;"
               "    padding: 4px;"
               "}"
               "QScrollBar:vertical { background: %2; width: 10px; margin: 0; }"
               "QScrollBar:horizontal { background: %2; height: 10px; margin: 0; }"
               "QScrollBar::handle { background: %4; border-radius: 5px; min-height: 20px; }"
               "QScrollBar::handle:hover { background: %1; }"
               "QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }"
               "QProgressBar {"
               "    border: 1px solid %4;"
               "    border-radius: 4px;"
               "    text-align: center;"
               "}"
               "QProgressBar::chunk { background-color: %1; }"
               "QCheckBox::indicator:checked, QRadioButton::indicator:checked {"
               "    background-color: %1;"
               "    border: 1px solid %1;"
               "}"
               "QStatusBar { background-color: %2; color: %5; }"
               "QGroupBox { border: 1px solid %4; border-radius: 4px; margin-top: 8px; }"
               "QGroupBox::title { subcontrol-origin: margin; left: 8px; color: %1; }")
        .arg(c.primary, c.background, c.surface, c.border, c.text, c.textDisabled,
             c.highlightText, c.hover);
}

} // namespace

#ifdef CUBESHELL_PLATFORM_OHOS
namespace {

// HarmonyOS 专用补丁 QSS：修 QComboBox 在鸿蒙上的两个平台缺陷。
//  1) combobox-popup:0 —— 鸿蒙上 Qt::Popup 不是桌面意义的顶层弹窗，而是依附
//     主窗口的子窗口语义（见 Qt for HarmonyOS「API 兼容性注意事项」），
//     QComboBox 默认用 Popup 顶层窗口显示下拉列表，在鸿蒙上根本弹不出来；
//     该属性置 0 后，下拉列表改用普通 QFrame 子控件画在当前窗口内。
//  2) min-height 用固定像素 —— qdarktheme 全局 QSS 的 min-height:1.5em 依赖
//     字体度量，鸿蒙字体系统与桌面差异大（平台限制：默认无等宽字体、度量
//     不同），闭合态文本被上下裁切只剩中间一截；固定像素高度兜底。
QString ohosStylePatch()
{
    return QStringLiteral(
        "QComboBox { combobox-popup: 0; min-height: 32px; padding: 2px 8px 2px 8px; }");
}

} // namespace
#endif // CUBESHELL_PLATFORM_OHOS

// 对应Python: cube-shell.py::setDarkTheme / setLightTheme
//（Python 侧 qdarktheme.load_stylesheet ↔ 这里读其导出的 qss 资源，
// 主色已在导出时通过 custom_colors 注入，无需追加 override）
QString ThemeManager::styleSheet(Appearance appearance)
{
    const QString resourcePath = qssResourcePath(appearance);
    const QString baseQss = readQss(resourcePath);
    QString qss;
    if (baseQss.isEmpty()) {
        // qrc 未编进当前目标（例如只链接 cube_core 的测试可执行文件）时降级，
        // 保证界面仍有完整深/浅色样式。
        qWarning("ThemeManager: qdarktheme resource %s unavailable, "
                 "falling back to built-in stylesheet",
                 qUtf8Printable(resourcePath));
        qss = fallbackStyleSheet(appearance);
    } else {
        qss = baseQss;
    }
#ifdef CUBESHELL_PLATFORM_OHOS
    qss += ohosStylePatch();
#endif
    return qss;
}

// 对应Python: cube-shell.py::setDarkTheme / setLightTheme（qdarktheme 附带的 palette）
QPalette ThemeManager::palette(Appearance appearance)
{
    const ThemeColors c = colorsFor(appearance);
    QPalette pal;
    pal.setColor(QPalette::Window, QColor(c.background));
    pal.setColor(QPalette::WindowText, QColor(c.text));
    pal.setColor(QPalette::Base, QColor(c.surface));
    pal.setColor(QPalette::AlternateBase, QColor(c.background));
    pal.setColor(QPalette::Text, QColor(c.text));
    pal.setColor(QPalette::Button, QColor(c.surface));
    pal.setColor(QPalette::ButtonText, QColor(c.text));
    pal.setColor(QPalette::Highlight, QColor(c.primary));
    pal.setColor(QPalette::HighlightedText, QColor(c.highlightText));
    pal.setColor(QPalette::ToolTipBase, QColor(c.surface));
    pal.setColor(QPalette::ToolTipText, QColor(c.text));
    pal.setColor(QPalette::Link, QColor(c.primary));
    pal.setColor(QPalette::PlaceholderText, QColor(c.textDisabled));
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor(c.textDisabled));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(c.textDisabled));
    pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(c.textDisabled));
    return pal;
}

// 对应Python: cube-shell.py::applyAppearance
void ThemeManager::applyTheme(QApplication *app, Appearance appearance)
{
    if (!app)
        return;
    app->setPalette(palette(appearance));
    app->setStyleSheet(styleSheet(appearance));
}

// 对应Python: cube-shell.py::applyAppearance（字符串入口）
void ThemeManager::applyTheme(QApplication *app, const QString &themeName)
{
    applyTheme(app, appearanceFromString(themeName));
}

} // namespace cubeshell
