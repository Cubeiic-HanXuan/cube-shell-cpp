#include "SettingsDialog.h"

#include <QApplication>
#include <QCheckBox>
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

// theme.json 中 SSH 超时的键现由 GlobalState::sshConnectTimeoutSeconds() 持有
// （之前是本文件的 kSshTimeoutKey）。挪过去是因为要有人**读**它：SshClient
// 建连时取这个值当预算，在此之前这个设置项写了没人看，纯装饰。

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
static const char kCommandCompletion[] = "settings/command_completion";
static const char kHostKeyVerification[] = "settings/host_key_verification";
static const char kKeepaliveEnabled[] = "settings/ssh_keepalive_enabled";
static const char kKeepaliveInterval[] = "settings/ssh_keepalive_interval_sec";
static const char kKeepaliveGrace[] = "settings/ssh_keepalive_grace_sec";
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
    // 代理页排在最后：既有的 setCurrentTab 调用点用的是 0/1/2 三个字面量
    // （见其声明处的注释），插在中间会把那些入口全部错位。
    m_tabWidget->addTab(createProxyTab(), tr("代理"));

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

    // 命令补全总开关：关掉后终端输入时不再弹候选窗、Ctrl+Space 也不唤起。
    // 默认开启，保持加开关之前的行为；历史命令始终照常记录。
    m_commandCompletion = new QCheckBox(tr("启用（输入时自动弹出候选，Ctrl+Space 手动唤起）"), page);
    m_commandCompletion->setToolTip(tr("开启后在终端输入时自动弹出命令补全候选（历史命令 + 常用命令）。\n"
                                       "关闭则完全不弹候选窗，Ctrl+Space 也不再唤起。"));
    form->addRow(tr("终端命令补全："), m_commandCompletion);

    // 主机密钥校验策略：决定首次连接或密钥变更时如何提示/阻断。
    m_hostKeyVerification = new QComboBox(page);
    m_hostKeyVerification->addItem(tr("询问（首次连接确认，变更时阻断）"), 1); // Ask
    m_hostKeyVerification->addItem(tr("自动接受新密钥并保存"), 2);            // AcceptNew
    m_hostKeyVerification->addItem(tr("严格（拒绝未知和变更）"), 0);           // Strict
    m_hostKeyVerification->addItem(tr("关闭校验"), 3);                        // Off
    m_hostKeyVerification->setToolTip(tr("严格模式会拒绝任何未知或已变更密钥的主机；\n"
                                         "询问模式会在首次连接时弹窗确认；\n"
                                         "自动接受新模式会静默记录首次密钥但拒绝变更。"));
    unifyFieldWidth(m_hostKeyVerification);
    addFullTextToolTip(m_hostKeyVerification);
    form->addRow(tr("SSH 主机密钥校验："), m_hostKeyVerification);

    m_keepaliveEnabled = new QCheckBox(tr("启用 SSH 保活"), page);
    m_keepaliveEnabled->setToolTip(tr("周期性发送 SSH keepalive，防止 NAT/防火墙切断空闲连接。"));
    form->addRow(tr("SSH 保活："), m_keepaliveEnabled);

    m_keepaliveInterval = new QSpinBox(page);
    m_keepaliveInterval->setRange(5, 600);
    m_keepaliveInterval->setSuffix(tr(" 秒"));
    m_keepaliveInterval->setToolTip(tr("向服务器发送 keepalive 的间隔。"));
    unifyFieldWidth(m_keepaliveInterval);
    form->addRow(tr("保活间隔："), m_keepaliveInterval);

    m_keepaliveGrace = new QSpinBox(page);
    m_keepaliveGrace->setRange(10, 300);
    m_keepaliveGrace->setSuffix(tr(" 秒"));
    m_keepaliveGrace->setToolTip(tr("连续多久未收到 keepalive 应答后判定连接已断开。"));
    unifyFieldWidth(m_keepaliveGrace);
    form->addRow(tr("无响应判定断开："), m_keepaliveGrace);
    return page;
}

// 代理 Tab：那份「全局代理」。设备的代理类型选「全局代理」时取的就是这里。
// Python 版没有代理功能，故无对应实现。
QWidget *SettingsDialog::createProxyTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *tip = new QLabel(
        tr("这里配置的是「全局代理」：在设备的代理类型里选「全局代理」的设备都走这一份，\n"
           "改一处即全部生效。设备也可以各自配自己的代理，与这份互不影响。"),
        page);
    tip->setWordWrap(true);
    tip->setStyleSheet(QStringLiteral("color: gray;"));
    layout->addWidget(tip);

    // 关掉「全局代理」这一项：这一页**自己就是**全局代理，选出 Global→Global
    // 是唯一的无限递归入口（见 ProxySettingsWidget::setGlobalOptionEnabled）。
    m_proxy = new ProxySettingsWidget(page);
    m_proxy->setGlobalOptionEnabled(false);
    layout->addWidget(m_proxy);
    layout->addStretch(1);
    return page;
}

void SettingsDialog::setProxyDeviceCatalog(const QList<ProxyDeviceItem> &devices)
{
    // excludeId 留空：全局代理不属于任何设备，没有"自己"要排除。
    // （成环由连接期的 flattenJumpChain 兜住——某台跳板机自己又选了「全局代理」
    //   而全局代理正是跳转服务器时会绕回来。）
    m_proxy->setDeviceCatalog(devices);
}

void SettingsDialog::setHasStoredProxyPassword(bool has) { m_proxy->setHasStoredPassword(has); }
bool SettingsDialog::proxyPasswordEdited() const { return m_proxy->passwordEdited(); }
QString SettingsDialog::proxyPassword() const { return m_proxy->config().password; }

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
        state.sshConnectTimeoutSeconds()).toInt());

    const int encIdx = m_encoding->findText(
        qs.value(settings_keys::kEncoding, QStringLiteral("UTF-8")).toString());
    if (encIdx >= 0)
        m_encoding->setCurrentIndex(encIdx);

    m_scrollback->setValue(
        qs.value(settings_keys::kScrollback, state.scrollbackLines()).toInt());

    m_commandCompletion->setChecked(
        qs.value(settings_keys::kCommandCompletion,
                 state.commandCompletionEnabled()).toBool());

    const int hkv = qs.value(settings_keys::kHostKeyVerification,
                             state.hostKeyVerification()).toInt();
    const int hkvIdx = m_hostKeyVerification->findData(hkv);
    if (hkvIdx >= 0)
        m_hostKeyVerification->setCurrentIndex(hkvIdx);

    m_keepaliveEnabled->setChecked(
        qs.value(settings_keys::kKeepaliveEnabled, state.sshKeepaliveEnabled()).toBool());
    m_keepaliveInterval->setValue(
        qs.value(settings_keys::kKeepaliveInterval, state.sshKeepaliveIntervalSeconds()).toInt());
    m_keepaliveGrace->setValue(
        qs.value(settings_keys::kKeepaliveGrace, state.sshKeepaliveGraceSeconds()).toInt());

    // 全局代理：**只从 GlobalState 读**，不走 QSettings。
    //
    // 上面每一项都是双写（QSettings + theme.json），因为那些键 Python 版也在用。
    // 代理是 C++ 独有的，而真正读它的只有 GlobalState::sshProxyConfig()
    //（SshClient 建连时取），再存一份 QSettings 副本就是造出第二个真相源——
    // 两边一旦不一致，连接期用的是哪一份完全看不出来。
    m_proxy->setConfig(state.sshProxyConfig());
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

    // 代理页先校验，不通过就停在原地。填了一半的代理配置比压根不填更糟：
    // 连接期只会得到一句"连接超时"，没人能从那句话看出是代理地址空着。
    QString proxyErr;
    if (!m_proxy->validate(&proxyErr)) {
        if (QWidget *proxyPage = m_proxy->parentWidget())
            m_tabWidget->setCurrentIndex(m_tabWidget->indexOf(proxyPage));
        QMessageBox::warning(this, tr("代理设置"), proxyErr);
        return;   // 刻意不调 QDialog::accept()：对话框留着让用户改
    }

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

    state.setSshConnectTimeoutSeconds(m_sshTimeout->value());
    state.setHostKeyVerification(m_hostKeyVerification->currentData().toInt());
    state.setSshKeepaliveEnabled(m_keepaliveEnabled->isChecked());
    state.setSshKeepaliveIntervalSeconds(m_keepaliveInterval->value());
    state.setSshKeepaliveGraceSeconds(m_keepaliveGrace->value());

    // 全局代理：只写 GlobalState（理由见 loadCurrentSettings 里那段）。
    // 口令不在这里——writeJson 不写它，落钥匙串由 MainWindow 在本对话框
    // 返回之后办（见 proxyPasswordEdited()）。
    state.setSshProxyConfig(m_proxy->config());

    const int scrollback = m_scrollback->value();
    const bool scrollbackDirty = scrollback != state.scrollbackLines();
    state.setScrollbackLines(scrollback);

    const bool completion = m_commandCompletion->isChecked();
    const bool completionDirty = completion != state.commandCompletionEnabled();
    state.setCommandCompletionEnabled(completion);

    // QSettings 持久化（本对话框全部设置项）。
    qs.setValue(settings_keys::kTheme, appearance);
    qs.setValue(settings_keys::kLanguage, m_language->currentData().toString());
    qs.setValue(settings_keys::kFontFamily, family);
    qs.setValue(settings_keys::kFontSize, size);
    qs.setValue(settings_keys::kDeviceListFontSize, deviceListSize);
    qs.setValue(settings_keys::kSshTimeout, m_sshTimeout->value());
    qs.setValue(settings_keys::kEncoding, m_encoding->currentText());
    qs.setValue(settings_keys::kScrollback, scrollback);
    qs.setValue(settings_keys::kCommandCompletion, completion);
    qs.setValue(settings_keys::kHostKeyVerification, m_hostKeyVerification->currentData().toInt());
    qs.setValue(settings_keys::kKeepaliveEnabled, m_keepaliveEnabled->isChecked());
    qs.setValue(settings_keys::kKeepaliveInterval, m_keepaliveInterval->value());
    qs.setValue(settings_keys::kKeepaliveGrace, m_keepaliveGrace->value());

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
    if (completionDirty)
        emit commandCompletionEnabledChanged(completion);

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
