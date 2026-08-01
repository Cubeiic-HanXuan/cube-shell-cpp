#include "LanguageManager.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QTranslator>

#include "config/GlobalState.h"

namespace cubeshell {

// 对应Python: i18n/language_manager.py::SUPPORTED_LANGUAGES
QVector<LanguageInfo> LanguageManager::supportedLanguages()
{
    static const QVector<LanguageInfo> kLanguages = {
        {QStringLiteral("zh_CN"), QStringLiteral("Chinese (Simplified)"), QStringLiteral("简体中文")},
        {QStringLiteral("zh_TW"), QStringLiteral("Chinese (Traditional)"), QStringLiteral("繁體中文")},
        {QStringLiteral("en_US"), QStringLiteral("English (US)"), QStringLiteral("English (US)")},
        {QStringLiteral("en_GB"), QStringLiteral("English (UK)"), QStringLiteral("English (UK)")},
        {QStringLiteral("ja_JP"), QStringLiteral("Japanese"), QStringLiteral("日本語")},
        {QStringLiteral("ko_KR"), QStringLiteral("Korean"), QStringLiteral("한국어")},
        {QStringLiteral("de_DE"), QStringLiteral("German"), QStringLiteral("Deutsch")},
        {QStringLiteral("fr_FR"), QStringLiteral("French"), QStringLiteral("Français")},
        {QStringLiteral("es_ES"), QStringLiteral("Spanish"), QStringLiteral("Español")},
        {QStringLiteral("pt_BR"), QStringLiteral("Portuguese (Brazil)"), QStringLiteral("Português (Brasil)")},
        {QStringLiteral("pt_PT"), QStringLiteral("Portuguese (Portugal)"), QStringLiteral("Português (Portugal)")},
        {QStringLiteral("ru_RU"), QStringLiteral("Russian"), QStringLiteral("Русский")},
        {QStringLiteral("it_IT"), QStringLiteral("Italian"), QStringLiteral("Italiano")},
        {QStringLiteral("nl_NL"), QStringLiteral("Dutch"), QStringLiteral("Nederlands")},
        {QStringLiteral("pl_PL"), QStringLiteral("Polish"), QStringLiteral("Polski")},
        {QStringLiteral("tr_TR"), QStringLiteral("Turkish"), QStringLiteral("Türkçe")},
        {QStringLiteral("ar_SA"), QStringLiteral("Arabic"), QStringLiteral("العربية")},
        {QStringLiteral("he_IL"), QStringLiteral("Hebrew"), QStringLiteral("עברית")},
        {QStringLiteral("th_TH"), QStringLiteral("Thai"), QStringLiteral("ไทย")},
        {QStringLiteral("vi_VN"), QStringLiteral("Vietnamese"), QStringLiteral("Tiếng Việt")},
        {QStringLiteral("id_ID"), QStringLiteral("Indonesian"), QStringLiteral("Bahasa Indonesia")},
        {QStringLiteral("ms_MY"), QStringLiteral("Malay"), QStringLiteral("Bahasa Melayu")},
        {QStringLiteral("hi_IN"), QStringLiteral("Hindi"), QStringLiteral("हिन्दी")},
        {QStringLiteral("uk_UA"), QStringLiteral("Ukrainian"), QStringLiteral("Українська")},
        {QStringLiteral("el_GR"), QStringLiteral("Greek"), QStringLiteral("Ελληνικά")},
    };
    return kLanguages;
}

LanguageManager &LanguageManager::instance()
{
    static LanguageManager s_instance;
    return s_instance;
}

// 对应Python: LanguageManager.initialize
void LanguageManager::initialize(QApplication *app, const QString &translationsDir)
{
    m_app = app;
    if (!translationsDir.isEmpty()) {
        m_translationsDir = translationsDir;
        return;
    }
    // 自动探测（不再指向 Python 版 i18n/ 目录）：
    //   1. .app bundle 的 Contents/Resources/translations/app（macOS）
    //   2. 可执行文件旁的 resources/translations/app（Linux / Windows）
    //   3. 编译期宏指向的源码树 cpp/resources/translations/app（开发期兜底）
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/../Resources/translations/app"),
        appDir + QStringLiteral("/resources/translations/app"),
#ifdef CUBESHELL_RESOURCE_DIR
        QStringLiteral(CUBESHELL_RESOURCE_DIR "/translations/app"),
#endif
    };
    for (const QString &dir : candidates) {
        if (QFileInfo::exists(dir + QStringLiteral("/app_zh_TW.qm"))) {
            m_translationsDir = QDir(dir).canonicalPath();
            return;
        }
    }
    m_translationsDir = appDir;
}

// 对应Python: get_language_display_name
QString LanguageManager::displayName(const QString &langCode)
{
    const auto langs = supportedLanguages();
    for (const LanguageInfo &info : langs) {
        if (info.code == langCode)
            return info.nativeName;
    }
    return langCode;
}

// 对应Python: LanguageManager.set_language
bool LanguageManager::setLanguage(const QString &langCode)
{
    if (!m_app)
        return false;

    // 移除旧的翻译器
    if (m_translator) {
        m_app->removeTranslator(m_translator);
        delete m_translator;
        m_translator = nullptr;
    }
    if (m_qtTranslator) {
        m_app->removeTranslator(m_qtTranslator);
        delete m_qtTranslator;
        m_qtTranslator = nullptr;
    }

    m_translator = new QTranslator(this);
    m_qtTranslator = new QTranslator(this);

    bool success = false;
    // 查找顺序: app_{lang_code}.qm -> app_{short_code}.qm
    const QString shortCode = langCode.section(QLatin1Char('_'), 0, 0);
    const QStringList candidates = {
        m_translationsDir + QStringLiteral("/app_%1.qm").arg(langCode),
        m_translationsDir + QStringLiteral("/app_%1.qm").arg(shortCode),
    };
    for (const QString &qmPath : candidates) {
        if (QFileInfo::exists(qmPath) && m_translator->load(qmPath)) {
            m_app->installTranslator(m_translator);
            success = true;
            break;
        }
    }

    // Qt 基础翻译（按钮文案等）
    if (m_qtTranslator->load(QStringLiteral("qtbase_%1").arg(langCode),
                             QCoreApplication::applicationDirPath()))
        m_app->installTranslator(m_qtTranslator);

    m_currentLanguage = langCode;

    // 持久化到 GlobalState（theme.json 的 "language" 键）。
    // 对应Python: theme.py 语言设置写回 util.THEME
    GlobalState::instance().setLanguage(langCode);
    GlobalState::instance().saveTheme();

    emit languageChanged(langCode);
    return success;
}

// 对应Python: LanguageManager.load_from_config
bool LanguageManager::loadFromConfig(const QString &langCode)
{
    // 无用户设置（空值）时默认简体中文：C++ 侧 UI 源字符串本身就是中文，
    // 不需要加载 .qm 即为中文界面，与 Python 版行为一致。
    const QString code = langCode.isEmpty() ? defaultLanguage() : langCode;
    return setLanguage(code);
}

// 对应Python: LanguageManager.get_system_language
QString LanguageManager::systemLanguage()
{
    const QString name = QLocale::system().name(); // "zh_CN" 形式
    return name.isEmpty() ? QStringLiteral("en_US") : name;
}

// 对应Python: LanguageManager.is_rtl_language
bool LanguageManager::isRtlLanguage(const QString &langCode)
{
    static const QStringList kRtl = {QStringLiteral("ar"), QStringLiteral("he"),
                                     QStringLiteral("fa"), QStringLiteral("ur")};
    return kRtl.contains(langCode.section(QLatin1Char('_'), 0, 0));
}

} // namespace cubeshell
