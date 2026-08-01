#pragma once

// LanguageManager.h — 应用多语言管理（语言列表 / 动态切换 QTranslator /
// 设置持久化到 GlobalState）。
// 对应Python: i18n/language_manager.py::LanguageManager
//
// 翻译文件复用 Python 侧已有的 i18n/app_<lang>.qm（qm 与加载方式跨语言通用，
// C++ 侧新增的 tr() 字符串通过 lupdate/lrelease 增量合入同一批 .ts，见
// ui/CMakeLists.txt 的说明注释）。

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QApplication;
class QTranslator;

namespace cubeshell {

// 对应Python: i18n/language_manager.py::SUPPORTED_LANGUAGES 的一项
struct LanguageInfo {
    QString code;        // "zh_CN"
    QString englishName; // "Chinese (Simplified)"
    QString nativeName;  // "简体中文"
};

class LanguageManager : public QObject {
    Q_OBJECT
public:
    // 对应Python: LanguageManager.instance()
    static LanguageManager &instance();

    LanguageManager(const LanguageManager &) = delete;
    LanguageManager &operator=(const LanguageManager &) = delete;

    // 对应Python: LanguageManager.initialize(app, translations_dir)
    // translationsDir 为空时自动探测（应用目录/i18n、工程 i18n/ 目录等）。
    void initialize(QApplication *app, const QString &translationsDir = QString());

    // 默认语言：无用户设置时使用简体中文（与 Python 版源码文案一致）。
    // 对应Python: LanguageManager.__init__ 里 _current_language = "zh_CN"
    static QString defaultLanguage() { return QStringLiteral("zh_CN"); }

    // 对应Python: get_supported_languages
    static QVector<LanguageInfo> supportedLanguages();
    // 对应Python: get_language_display_name
    static QString displayName(const QString &langCode);

    // 对应Python: get_current_language
    QString currentLanguage() const { return m_currentLanguage; }

    // 切换语言：卸载旧 QTranslator → 加载 app_<code>.qm →安装；
    // 成功后写入 GlobalState（theme.json 的 "language" 键）持久化。
    // 对应Python: set_language + theme.py 中语言项的写回
    bool setLanguage(const QString &langCode);

    // 启动时从配置恢复语言；langCode 为空（无用户设置）时回落到 defaultLanguage()。
    // 对应Python: load_from_config + 默认 zh_CN
    bool loadFromConfig(const QString &langCode);

    // 对应Python: get_system_language
    static QString systemLanguage();
    // 对应Python: is_rtl_language
    static bool isRtlLanguage(const QString &langCode);

signals:
    // 语言切换完成（UI 可据此 retranslate；本期重启后全量生效）。
    void languageChanged(const QString &langCode);

private:
    LanguageManager() = default;

    QApplication *m_app = nullptr;
    QTranslator *m_translator = nullptr;    // 应用翻译（app_*.qm）
    QTranslator *m_qtTranslator = nullptr;  // Qt 基础翻译（qtbase_*.qm）
    QString m_currentLanguage = QStringLiteral("zh_CN");   // 默认简体中文
    QString m_translationsDir;
};

} // namespace cubeshell
