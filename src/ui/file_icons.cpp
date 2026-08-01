#include "file_icons.h"

#include <QFileIconProvider>
#include <QHash>
#include <QVector>

namespace cubeshell {

// 扩展名 → 图标资源路径映射（与 Python 版一比一）。
// 对应Python: function/util.py::_EXT_ICON_MAP
static QHash<QString, QString> makeExtIconMap()
{
    return {
        {QStringLiteral(".sh"), QStringLiteral(":/icons8-ssh-48.png")},
        {QStringLiteral(".sql"), QStringLiteral(":/icons8-sql-48.png")},
        {QStringLiteral(".py"), QStringLiteral(":/icons8-python-48.png")},
        {QStringLiteral(".java"), QStringLiteral(":/icons8-java-48.png")},
        {QStringLiteral(".go"), QStringLiteral(":/icons8-golang-48.png")},
        {QStringLiteral(".c"), QStringLiteral(":/icons8-c-48.png")},
        {QStringLiteral(".cpp"), QStringLiteral(":/icons8-c-48.png")},
        {QStringLiteral(".js"), QStringLiteral(":/icons8-js-48.png")},
        {QStringLiteral(".vue"), QStringLiteral(":/icons8-vuejs-48.png")},
        {QStringLiteral(".html"), QStringLiteral(":/icons8-html-48.png")},
        {QStringLiteral(".css"), QStringLiteral(":/icons8-css-48.png")},
        {QStringLiteral(".exe"), QStringLiteral(":/icons8-windows-48.png")},
        {QStringLiteral(".dmg"), QStringLiteral(":/icons8-dmg-48.png")},
        {QStringLiteral(".bat"), QStringLiteral(":/icons8-bat-48.png")},
        {QStringLiteral(".vbs"), QStringLiteral(":/icons8-bat-48.png")},
        {QStringLiteral(".ini"), QStringLiteral(":/icons8-ini-48.png")},
        {QStringLiteral(".tsx"), QStringLiteral(":/icons8-react-48.png")},
        {QStringLiteral(".ts"), QStringLiteral(":/icons8-ts-48.png")},
        {QStringLiteral(".editorconfig"), QStringLiteral(":/icons8-editorconfig-48.png")},
        {QStringLiteral(".jar"), QStringLiteral(":/icons8-jar-48.png")},
        {QStringLiteral(".so"), QStringLiteral(":/icons8-linux-48.png")},
        {QStringLiteral(".tar"), QStringLiteral(":/icons8-zip-48.png")},
        {QStringLiteral(".gz"), QStringLiteral(":/icons8-zip-48.png")},
        {QStringLiteral(".zip"), QStringLiteral(":/icons8-zip-48.png")},
        {QStringLiteral(".cfg"), QStringLiteral(":/icons8-settings-40.png")},
        {QStringLiteral(".gitconfig"), QStringLiteral(":/icons8-settings-40.png")},
        {QStringLiteral(".conf"), QStringLiteral(":/icons8-settings-40.png")},
        {QStringLiteral(".png"), QStringLiteral(":/icons8-png-48.png")},
        {QStringLiteral(".gif"), QStringLiteral(":/icons8-gif-48.png")},
        {QStringLiteral(".jpg"), QStringLiteral(":/icons8-jpg-48.png")},
        {QStringLiteral(".jpeg"), QStringLiteral(":/icons8-jpg-48.png")},
        {QStringLiteral(".license"), QStringLiteral(":/icons8-license-48.png")},
        {QStringLiteral(".json"), QStringLiteral(":/icons8-json-48.png")},
        {QStringLiteral(".txt"), QStringLiteral(":/icons8-txt-48.png")},
        {QStringLiteral(".gitignore"), QStringLiteral(":/icons8-gitignore-48.png")},
        {QStringLiteral(".md"), QStringLiteral(":/icons8-md-48.png")},
        {QStringLiteral(".yaml"), QStringLiteral(":/icons8-yaml-48.png")},
        {QStringLiteral(".yml"), QStringLiteral(":/icons8-yaml-48.png")},
        {QStringLiteral(".properties"), QStringLiteral(":/icons8-properties-48.png")},
        {QStringLiteral(".log"), QStringLiteral(":/icons-log-48.png")},
        {QStringLiteral(".toml"), QStringLiteral(":/icons-toml-48.png")},
        {QStringLiteral(".xml"), QStringLiteral(":/xml-48.png")},
        {QStringLiteral(".swift"), QStringLiteral(":/icons8-swift-48.png")},
        {QStringLiteral(".svg"), QStringLiteral(":/icons8-svg-48.png")},
        {QStringLiteral(".db"), QStringLiteral(":/icons8-db-48.png")},
        {QStringLiteral(".lock"), QStringLiteral(":/icons8-lock-48.png")},
    };
}

QIcon iconForFile(const QString &name, bool isDir)
{
    // 目录：系统默认文件夹图标（带缓存）。
    // 对应Python: util.get_default_folder_icon（QFileIconProvider.Folder）
    if (isDir) {
        static const QIcon folderIcon = QFileIconProvider().icon(QFileIconProvider::IconType::Folder);
        return folderIcon;
    }

    static const QHash<QString, QString> extMap = makeExtIconMap();
    // QIcon 对象缓存：避免每次 QIcon(path) 重复加载。对应Python: _FILE_ICON_CACHE
    static QHash<QString, QIcon> iconCache;

    // 1) 先按文件名前缀匹配（.eslintrc 必须先于 .env，避免 .eslintrc.js 走到 .env）。
    // 对应Python: _PREFIX_ICON_MAP
    static const QVector<QPair<QString, QString>> prefixMap = {
        {QStringLiteral(".eslintrc"), QStringLiteral(":/icons8-eslintrc-48.png")},
        {QStringLiteral(".env"), QStringLiteral(":/icons8-env-48.png")},
    };
    for (const auto &p : prefixMap) {
        if (name.startsWith(p.first)) {
            auto it = iconCache.find(p.first);
            if (it == iconCache.end())
                it = iconCache.insert(p.first, QIcon(p.second));
            return it.value();
        }
    }

    // 2) 按扩展名查表（含点，最后一个 '.' 起，小写）。
    // 对应Python: qt_str[qt_str.rfind('.'):].lower()
    const int dotIdx = name.lastIndexOf(QLatin1Char('.'));
    const QString ext = dotIdx != -1 ? name.mid(dotIdx).toLower() : QString();

    auto cached = iconCache.constFind(ext);
    if (cached != iconCache.constEnd())
        return cached.value();

    const QString path = extMap.value(ext);
    if (!path.isEmpty()) {
        const QIcon icon(path);
        iconCache.insert(ext, icon);
        return icon;
    }

    // 3) 兜底：系统默认文件图标。对应Python: QFileIconProvider.IconType.File
    static const QIcon defaultFileIcon = QFileIconProvider().icon(QFileIconProvider::IconType::File);
    return defaultFileIcon;
}

} // namespace cubeshell
