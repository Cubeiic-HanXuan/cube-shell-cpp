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
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include "LanguageManager.h"
#include "config/GlobalState.h"
#include "util/ThemeManager.h"

namespace cubeshell {

// theme.json 中 SSH 超时的键（C++ 侧新增，Python 侧未知键会被原样保留）。
static const char kSshTimeoutKey[] = "ssh_timeout";

// QSettings 键（组织/应用名见 app/main.cpp 的 setOrganizationName）。
namespace settings_keys {
static const char kTheme[]      = "settings/theme";
static const char kLanguage[]   = "settings/language";
static const char kFontFamily[] = "settings/font_family";
static const char kFontSize[]   = "settings/font_size";
static const char kSshTimeout[] = "settings/ssh_timeout";
static const char kEncoding[]   = "settings/terminal_encoding";
} // namespace settings_keys

// 对应Python: function/theme.py::MainWindow.__init__（设置窗布局）
SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("设置"));
    setMinimumSize(460, 380);

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
    form->addRow(tr("终端字体"), m_fontFamily);

    m_fontSize = new QSpinBox(page);
    m_fontSize->setRange(8, 40);
    form->addRow(tr("字体大小:"), m_fontSize);

    m_sshTimeout = new QSpinBox(page);
    m_sshTimeout->setRange(5, 600);
    m_sshTimeout->setSuffix(tr(" 秒"));
    form->addRow(tr("SSH 连接超时："), m_sshTimeout);

    m_encoding = new QComboBox(page);
    m_encoding->addItems({QStringLiteral("UTF-8"), QStringLiteral("GBK"),
                          QStringLiteral("GB2312")});
    form->addRow(tr("默认终端编码："), m_encoding);
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

    m_sshTimeout->setValue(qs.value(
        settings_keys::kSshTimeout,
        state.theme().value(QLatin1String(kSshTimeoutKey)).toInt(15)).toInt());

    const int encIdx = m_encoding->findText(
        qs.value(settings_keys::kEncoding, QStringLiteral("UTF-8")).toString());
    if (encIdx >= 0)
        m_encoding->setCurrentIndex(encIdx);
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

    QJsonObject theme = state.theme();
    theme[QLatin1String(kSshTimeoutKey)] = m_sshTimeout->value();
    state.setTheme(theme);

    // QSettings 持久化（本对话框全部设置项）。
    qs.setValue(settings_keys::kTheme, appearance);
    qs.setValue(settings_keys::kLanguage, m_language->currentData().toString());
    qs.setValue(settings_keys::kFontFamily, family);
    qs.setValue(settings_keys::kFontSize, size);
    qs.setValue(settings_keys::kSshTimeout, m_sshTimeout->value());
    qs.setValue(settings_keys::kEncoding, m_encoding->currentText());

    // theme.json 同步写回（与 Python 版共享配置）。
    QString err;
    if (!state.saveTheme(&err) && !err.isEmpty())
        QMessageBox::warning(this, windowTitle(), tr("保存设置失败: %1").arg(err));

    if (appearanceDirty)
        emit appearanceChanged(appearance);
    if (fontDirty)
        emit fontChanged(family, size);

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
