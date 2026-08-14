#include "SettingsDialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QSettings>
#include <QSizePolicy>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include "LanguageManager.h"
#include "config/GlobalState.h"
#include "util/ThemeManager.h"

namespace cubeshell {

// theme.json 中 SSH 超时的键（C++ 侧新增，Python 侧未知键会被原样保留）。
static const char kSshTimeoutKey[] = "ssh_timeout";

// 表单字段统一最小宽度：各 Tab 里所有输入框/下拉框同宽，
// 避免长短不一显得杂乱，也给长下拉值留出显示空间。
constexpr int kFieldMinWidth = 280;

static void unifyFieldWidth(QWidget *w)
{
    w->setMinimumWidth(kFieldMinWidth);
    QSizePolicy sp = w->sizePolicy();
    sp.setHorizontalPolicy(QSizePolicy::Expanding);
    w->setSizePolicy(sp);
}

// 下拉框闭合态对超长值会省略显示，用 tooltip 兜底展示全值。
static void addFullTextToolTip(QComboBox *combo)
{
    QObject::connect(combo, &QComboBox::currentTextChanged, combo,
                     [combo](const QString &text) { combo->setToolTip(text); });
    combo->setToolTip(combo->currentText());
}

// QSettings 键（组织/应用名见 app/main.cpp 的 setOrganizationName）。
namespace settings_keys {
static const char kTheme[]      = "settings/theme";
static const char kLanguage[]   = "settings/language";
static const char kFontFamily[] = "settings/font_family";
static const char kFontSize[]   = "settings/font_size";
static const char kDeviceListFontSize[] = "settings/device_list_font_size";
static const char kSshTimeout[] = "settings/ssh_timeout";
static const char kEncoding[]   = "settings/terminal_encoding";
static const char kScrollback[] = "settings/scrollback_lines";
} // namespace settings_keys

// 对应Python: function/theme.py::MainWindow.__init__（设置窗布局）
SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("设置"));
    setMinimumSize(560, 380);

    m_tabWidget = new QTabWidget(this);
    m_tabWidget->addTab(createThemeTab(), tr("主题设置"));
    m_tabWidget->addTab(createLanguageTab(), tr("语言设置"));
    m_tabWidget->addTab(createGeneralTab(), tr("通用"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_tabWidget, 1);
    layout->addWidget(buttons);

    loadCurrentSettings();

    // 选中主题即实时预览（确认前不落盘；Cancel 经 reject() 还原）。
    // 对应Python: theme.py 单选切换后立即 applyAppearance
    connect(m_themeList, &QListWidget::currentItemChanged, this,
            [](QListWidgetItem *current, QListWidgetItem *) {
                if (current)
                    ThemeManager::applyTheme(qApp, current->data(Qt::UserRole).toString());
            });
}

// 打开后选中指定 Tab（主题设置入口定位主题页，通用设置入口定位通用页）。
void SettingsDialog::setCurrentTab(int index)
{
    if (index >= 0 && index < m_tabWidget->count())
        m_tabWidget->setCurrentIndex(index);
}

// 主题 Tab：可用主题列表 + 实时预览说明。
// 对应Python: theme.py 的 dark/light 单选
QWidget *SettingsDialog::createThemeTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *tip = new QLabel(tr("选中主题即可实时预览，点“确定”后应用并保存。"), page);
    tip->setWordWrap(true);
    tip->setStyleSheet(QStringLiteral("color: gray;"));
    layout->addWidget(tip);

    m_themeList = new QListWidget(page);
    for (const QString &name : ThemeManager::availableThemes()) {
        auto *item = new QListWidgetItem(
            name.compare(QLatin1String("light"), Qt::CaseInsensitive) == 0
                ? tr("亮色") : tr("暗色"),
            m_themeList);
        item->setData(Qt::UserRole, name);
    }
    layout->addWidget(m_themeList, 1);
    return page;
}

// 语言 Tab：语言下拉（重启后全量生效）。
// 对应Python: cube-shell.py::show_language_settings 的语言下拉
QWidget *SettingsDialog::createLanguageTab()
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);

    m_language = new QComboBox(page);
    const auto langs = LanguageManager::supportedLanguages();
    for (const LanguageInfo &info : langs)
        m_language->addItem(QStringLiteral("%1 (%2)").arg(info.nativeName, info.code), info.code);
    unifyFieldWidth(m_language);
    addFullTextToolTip(m_language);
    form->addRow(tr("选择应用程序语言"), m_language);

    auto *hint = new QLabel(tr("更改语言后需要重启应用程序才能生效"), page);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: gray;"));
    form->addRow(hint);
    return page;
}

// 通用 Tab：字体 / SSH 超时 / 默认终端编码。
// 对应Python: theme.py::apply_font_settings + 命令执行超时策略
QWidget *SettingsDialog::createGeneralTab()
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);

    m_fontFamily = new QFontComboBox(page);
    m_fontFamily->setFontFilters(QFontComboBox::MonospacedFonts);
    unifyFieldWidth(m_fontFamily);
    addFullTextToolTip(m_fontFamily);
    form->addRow(tr("终端字体"), m_fontFamily);

    m_fontSize = new QSpinBox(page);
    m_fontSize->setRange(8, 40);
    unifyFieldWidth(m_fontSize);
    form->addRow(tr("终端字体大小:"), m_fontSize);

    // 设备列表（左侧分组/设备树）字号，与终端字号相互独立。
    m_deviceListFontSize = new QSpinBox(page);
    m_deviceListFontSize->setRange(8, 40);
    unifyFieldWidth(m_deviceListFontSize);
    form->addRow(tr("设备列表字体大小:"), m_deviceListFontSize);

    m_sshTimeout = new QSpinBox(page);
    m_sshTimeout->setRange(5, 600);
    m_sshTimeout->setSuffix(tr(" 秒"));
    unifyFieldWidth(m_sshTimeout);
    form->addRow(tr("SSH 连接超时："), m_sshTimeout);

    m_encoding = new QComboBox(page);
    m_encoding->addItems({QStringLiteral("UTF-8"), QStringLiteral("GBK"),
                          QStringLiteral("GB2312")});
    unifyFieldWidth(m_encoding);
    addFullTextToolTip(m_encoding);
    form->addRow(tr("默认终端编码："), m_encoding);

    // 回滚行数：决定能往回翻多少输出，也决定「查找」能检索到多大范围。
    m_scrollback = new QSpinBox(page);
    m_scrollback->setRange(0, 1000000);
    m_scrollback->setSingleStep(1000);
    m_scrollback->setSuffix(tr(" 行"));
    m_scrollback->setSpecialValueText(tr("不保留"));   // 0
    m_scrollback->setToolTip(tr("终端保留的历史输出行数，也是“查找”能检索的范围。\n"
                                "行数越大越占内存，排查线上日志建议 10000 以上。"));
    unifyFieldWidth(m_scrollback);
    form->addRow(tr("终端回滚行数："), m_scrollback);
    return page;
}

// 对应Python: function/theme.py::MainWindow._load_current_settings
// QSettings 优先，缺省回退 GlobalState（theme.json，与 Python 版共享）。
void SettingsDialog::loadCurrentSettings()
{
    GlobalState &state = GlobalState::instance();
    QSettings qs;

    m_originalAppearance =
        qs.value(settings_keys::kTheme, state.appearance()).toString();
    for (int i = 0; i < m_themeList->count(); ++i) {
        if (m_themeList->item(i)->data(Qt::UserRole).toString() == m_originalAppearance) {
            m_themeList->setCurrentRow(i);
            break;
        }
    }
    if (!m_themeList->currentItem() && m_themeList->count() > 0)
        m_themeList->setCurrentRow(0);

    const int langIdx = m_language->findData(
        qs.value(settings_keys::kLanguage, state.language()).toString());
    if (langIdx >= 0)
        m_language->setCurrentIndex(langIdx);

    const QString family =
        qs.value(settings_keys::kFontFamily, state.fontFamily()).toString();
    if (!family.isEmpty())
        m_fontFamily->setCurrentFont(QFont(family));
    m_fontSize->setValue(qs.value(settings_keys::kFontSize, state.fontSize()).toInt());
    m_deviceListFontSize->setValue(
        qs.value(settings_keys::kDeviceListFontSize, state.deviceListFontSize()).toInt());

    m_sshTimeout->setValue(qs.value(
        settings_keys::kSshTimeout,
        state.theme().value(QLatin1String(kSshTimeoutKey)).toInt(15)).toInt());

    const int encIdx = m_encoding->findText(
        qs.value(settings_keys::kEncoding, QStringLiteral("UTF-8")).toString());
    if (encIdx >= 0)
        m_encoding->setCurrentIndex(encIdx);

    m_scrollback->setValue(
        qs.value(settings_keys::kScrollback, state.scrollbackLines()).toInt());
}

int SettingsDialog::sshTimeoutSeconds() const
{
    return m_sshTimeout->value();
}

QString SettingsDialog::terminalEncoding() const
{
    return m_encoding->currentText();
}

// 对应Python: function/theme.py::MainWindow._set_appearance / apply_font_settings
void SettingsDialog::accept()
{
    GlobalState &state = GlobalState::instance();
    QSettings qs;

    // 主题：实时预览已应用，这里确认并落盘。
    QString appearance = m_originalAppearance;
    if (QListWidgetItem *item = m_themeList->currentItem())
        appearance = item->data(Qt::UserRole).toString();
    const bool appearanceDirty = appearance != state.appearance();
    state.setAppearance(appearance);
    ThemeManager::applyTheme(qApp, appearance);

    const QString family = m_fontFamily->currentFont().family();
    const int size = m_fontSize->value();
    const bool fontDirty = family != state.fontFamily() || size != state.fontSize();
    state.setFont(family, size);

    const int deviceListSize = m_deviceListFontSize->value();
    const bool deviceListFontDirty = deviceListSize != state.deviceListFontSize();
    state.setDeviceListFontSize(deviceListSize);

    QJsonObject theme = state.theme();
    theme[QLatin1String(kSshTimeoutKey)] = m_sshTimeout->value();
    state.setTheme(theme);

    const int scrollback = m_scrollback->value();
    const bool scrollbackDirty = scrollback != state.scrollbackLines();
    state.setScrollbackLines(scrollback);

    // QSettings 持久化（本对话框全部设置项）。
    qs.setValue(settings_keys::kTheme, appearance);
    qs.setValue(settings_keys::kLanguage, m_language->currentData().toString());
    qs.setValue(settings_keys::kFontFamily, family);
    qs.setValue(settings_keys::kFontSize, size);
    qs.setValue(settings_keys::kDeviceListFontSize, deviceListSize);
    qs.setValue(settings_keys::kSshTimeout, m_sshTimeout->value());
    qs.setValue(settings_keys::kEncoding, m_encoding->currentText());
    qs.setValue(settings_keys::kScrollback, scrollback);

    // theme.json 同步写回（与 Python 版共享配置）。
    QString err;
    if (!state.saveTheme(&err) && !err.isEmpty())
        QMessageBox::warning(this, windowTitle(), tr("保存设置失败: %1").arg(err));

    if (appearanceDirty)
        emit appearanceChanged(appearance);
    if (fontDirty)
        emit fontChanged(family, size);
    if (deviceListFontDirty)
        emit deviceListFontSizeChanged(deviceListSize);
    if (scrollbackDirty)
        emit scrollbackLinesChanged(scrollback);

    // 语言切换（LanguageManager 内部会写回 GlobalState 并持久化）。
    const QString langCode = m_language->currentData().toString();
    if (langCode != LanguageManager::instance().currentLanguage()) {
        LanguageManager::instance().setLanguage(langCode);
        // 对应Python: show_language_settings 里的重启提示
        QMessageBox::information(this, tr("语言设置"),
                                 tr("语言设置已更改，请重启应用程序以生效。"));
    }

    QDialog::accept();
}

// Cancel/Esc：撤销主题实时预览，恢复打开对话框时的外观。
void SettingsDialog::reject()
{
    QListWidgetItem *item = m_themeList->currentItem();
    const QString previewed =
        item ? item->data(Qt::UserRole).toString() : m_originalAppearance;
    if (previewed != m_originalAppearance)
        ThemeManager::applyTheme(qApp, m_originalAppearance);
    QDialog::reject();
}

} // namespace cubeshell
