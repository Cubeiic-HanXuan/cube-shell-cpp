// FinderIntegration_mac.mm — macOS Finder 快速操作安装（Objective-C++ 实现）。
// 对应Python: core/finder_integration.py
//
// 仅在 APPLE 下编译（由 cpp/src/core/CMakeLists.txt 的 if(APPLE) 分支加入
// 源列表）。写盘用 Qt（与其余 core 代码一致），Launch Services 注册与
// 快捷方式创建用 Cocoa API。

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#include <CoreServices/CoreServices.h>

#include "FinderIntegration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

namespace cubeshell {
namespace FinderIntegration {

namespace {

// 对应Python: WORKFLOW_NAME = "Open in CubeShell"
const QString kWorkflowName = QStringLiteral("Open in CubeShell");

// workflow 的 Contents/Info.plist：向 Finder"服务/快速操作"菜单注册
// "在 CubeShell 中打开终端" 菜单项，接收文件夹/文件 URL。
// 对应Python: install() 里拼接的 info_plist 字符串（内容逐字一致）
const char *kInfoPlist = R"PLIST(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>NSServices</key>
	<array>
		<dict>
			<key>NSMenuItem</key>
			<dict>
				<key>default</key>
				<string>在 CubeShell 中打开终端</string>
			</dict>
			<key>NSMessage</key>
			<string>runWorkflowAsService</string>
			<key>NSSendFileTypes</key>
			<array>
				<string>public.folder</string>
				<string>public.file-url</string>
			</array>
		</dict>
	</array>
</dict>
</plist>)PLIST";

// workflow 的 Contents/document.wflow：Automator "Run Shell Script" 动作。
// 内嵌脚本对每个选中的文件夹做 percent-encode 后 open
// "cubeshell://open-local?path=<encoded>"，即启动应用 + 传路径参数。
// 对应Python: install() 里拼接的 document_wflow / shell_script（内容逐字一致）
const char *kDocumentWflow = R"PLIST(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>AMApplicationBuild</key>
	<string>523</string>
	<key>AMApplicationVersion</key>
	<string>2.10</string>
	<key>AMDocumentVersion</key>
	<string>2</string>
	<key>actions</key>
	<array>
		<dict>
			<key>action</key>
			<dict>
				<key>AMAccepts</key>
				<dict>
					<key>Container</key>
					<string>List</string>
					<key>Optional</key>
					<true/>
					<key>Types</key>
					<array>
						<string>com.apple.cocoa.path</string>
					</array>
				</dict>
				<key>AMActionVersion</key>
				<string>1.0.2</string>
				<key>AMApplication</key>
				<array>
					<string>Automator</string>
				</array>
				<key>AMCategory</key>
				<string>AMCategoryUtilities</string>
				<key>AMIconName</key>
				<string>Run Shell Script</string>
				<key>AMKeywords</key>
				<array>
					<string>Shell</string>
					<string>Script</string>
				</array>
				<key>AMName</key>
				<string>Run Shell Script</string>
				<key>AMProvides</key>
				<dict>
					<key>Container</key>
					<string>List</string>
					<key>Types</key>
					<array>
						<string>com.apple.cocoa.path</string>
					</array>
				</dict>
				<key>AMRequiredResources</key>
				<array/>
				<key>ActionBundlePath</key>
				<string>/System/Library/Automator/Run Shell Script.action</string>
				<key>ActionName</key>
				<string>Run Shell Script</string>
				<key>ActionParameters</key>
				<dict>
					<key>COMMAND_STRING</key>
					<string>for f in "$@"; do
    if [ -d "$f" ]; then
        ENCODED=$(python3 -c "import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1], safe='/'))" "$f")
        open "cubeshell://open-local?path=$ENCODED"
    fi
done</string>
					<key>CheckedForUserDefaultShell</key>
					<true/>
					<key>inputMethod</key>
					<integer>1</integer>
					<key>shell</key>
					<string>/bin/bash</string>
					<key>source</key>
					<string></string>
				</dict>
				<key>BundleIdentifier</key>
				<string>com.apple.RunShellScript</string>
				<key>CFBundleVersion</key>
				<string>1.0.2</string>
				<key>CanShowSelectedItemsWhenRun</key>
				<false/>
				<key>CanShowWhenRun</key>
				<true/>
				<key>Category</key>
				<array>
					<string>AMCategoryUtilities</string>
				</array>
				<key>Class Name</key>
				<string>RunShellScriptAction</string>
				<key>InputUUID</key>
				<string>A7E1E2E1-4F5B-4C8D-9B1A-3E5F7A8C9D0E</string>
				<key>Keywords</key>
				<array>
					<string>Shell</string>
					<string>Script</string>
				</array>
				<key>OutputUUID</key>
				<string>B8F2F3F2-5G6C-5D9E-0C2B-4F6G8B9D0E1F</string>
				<key>UUID</key>
				<string>C9G3G4G3-6H7D-6E0F-1D3C-5G7H9C0E1F2G</string>
				<key>UnlocalizedApplications</key>
				<array>
					<string>Automator</string>
				</array>
			</dict>
			<key>isViewVisible</key>
			<integer>1</integer>
		</dict>
	</array>
	<key>connectors</key>
	<dict/>
	<key>workflowMetaData</key>
	<dict>
		<key>applicationBundleIDsByPath</key>
		<dict/>
		<key>applicationPaths</key>
		<array/>
		<key>inputTypeIdentifier</key>
		<string>com.apple.Automator.fileSystemObject</string>
		<key>outputTypeIdentifier</key>
		<string>com.apple.Automator.nothing</string>
		<key>presentationMode</key>
		<integer>15</integer>
		<key>processesInput</key>
		<integer>0</integer>
		<key>serviceApplicationGroupName</key>
		<string>Finder</string>
		<key>serviceApplicationPath</key>
		<string>/System/Library/CoreServices/Finder.app</string>
		<key>serviceInputTypeIdentifier</key>
		<string>com.apple.Automator.fileSystemObject</string>
		<key>serviceOutputTypeIdentifier</key>
		<string>com.apple.Automator.nothing</string>
		<key>workflowTypeIdentifier</key>
		<string>com.apple.Automator.servicesMenu</string>
	</dict>
</dict>
</plist>)PLIST";

// 把 UTF-8 内容原样写入文件；失败时填充 errorMessage 并返回 false。
bool writeTextFile(const QString &path, const char *content, QString *errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("无法写入 %1: %2").arg(path, file.errorString());
        return false;
    }
    file.write(content);
    file.close();
    return file.error() == QFile::NoError;
}

// 当前应用 bundle（.app）路径；裸可执行文件运行时返回可执行文件路径。
QString applicationBundlePath()
{
    NSBundle *bundle = [NSBundle mainBundle];
    if (bundle != nil && bundle.bundlePath != nil)
        return QString::fromNSString(bundle.bundlePath);
    return QCoreApplication::applicationFilePath();
}

// 把当前应用 bundle 注册到 Launch Services，使 workflow 里的
// `open "cubeshell://..."` 在开发构建（未安装到 /Applications）下也能
// 解析到本应用。Python 版依赖 Info.plist + 系统自动扫描，无此步骤。
void registerWithLaunchServices()
{
    NSURL *url = [NSURL fileURLWithPath:applicationBundlePath().toNSString()];
    if (url == nil)
        return;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    LSRegisterURL((__bridge CFURLRef)url, true);
#pragma clang diagnostic pop
}

} // namespace

QString workflowPath()
{
    // 对应Python: WORKFLOW_DIR = ~/Library/Services/Open in CubeShell.workflow
    return QDir::homePath()
        + QStringLiteral("/Library/Services/%1.workflow").arg(kWorkflowName);
}

bool isSupported()
{
    // 对应Python: platform.system() == 'Darwin'（本文件仅 APPLE 编译，恒 true）
    return true;
}

bool isInstalled()
{
    // 对应Python: os.path.isdir(WORKFLOW_DIR)
    return QFileInfo(workflowPath()).isDir();
}

bool installFinderExtension(QString *errorMessage)
{
    const QString workflowDir = workflowPath();
    const QString contentsDir = workflowDir + QStringLiteral("/Contents");

    // 对应Python: 如果已存在先删除
    QDir existing(workflowDir);
    if (existing.exists() && !existing.removeRecursively()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("无法删除旧的 workflow: %1").arg(workflowDir);
        return false;
    }

    if (!QDir().mkpath(contentsDir)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("无法创建目录: %1").arg(contentsDir);
        return false;
    }

    // 对应Python: 写入 Info.plist / document.wflow
    if (!writeTextFile(contentsDir + QStringLiteral("/Info.plist"), kInfoPlist, errorMessage))
        return false;
    if (!writeTextFile(contentsDir + QStringLiteral("/document.wflow"), kDocumentWflow,
                       errorMessage))
        return false;

    registerWithLaunchServices();
    return true;
}

bool uninstallFinderExtension(QString *errorMessage)
{
    // 对应Python: uninstall()（不存在视为成功）
    QDir dir(workflowPath());
    if (!dir.exists())
        return true;
    if (!dir.removeRecursively()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("无法删除 workflow: %1").arg(workflowPath());
        return false;
    }
    return true;
}

bool createDesktopShortcut(QString *errorMessage)
{
    @autoreleasepool {
        const QString target = applicationBundlePath();
        const QString linkPath = QDir::homePath() + QStringLiteral("/Desktop/")
            + QFileInfo(target).fileName();

        NSFileManager *fm = [NSFileManager defaultManager];
        NSString *link = linkPath.toNSString();
        // 已存在同名快捷方式先移除（保持幂等，与 install 的先删后建一致）
        if ([fm fileExistsAtPath:link])
            [fm removeItemAtPath:link error:nil];

        NSError *error = nil;
        if (![fm createSymbolicLinkAtPath:link
                      withDestinationPath:target.toNSString()
                                    error:&error]) {
            if (errorMessage)
                *errorMessage = error != nil
                    ? QString::fromNSString(error.localizedDescription)
                    : QStringLiteral("创建快捷方式失败: %1").arg(linkPath);
            return false;
        }
        return true;
    }
}

} // namespace FinderIntegration
} // namespace cubeshell

#endif // __APPLE__
