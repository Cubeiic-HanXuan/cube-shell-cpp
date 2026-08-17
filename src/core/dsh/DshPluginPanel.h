#pragma once

// DshPluginPanel.h — DeepSeek Harness 插件管理 Tab。
//
// dsh 的架构核心是「Everything is a Plugin」：一个 profile 由若干 bundle 组成，
// 插件即 profile 目录下的 npm 依赖。本页对应 `dsh plugin --profile <p> add|remove`
// （dsh 内部把 plugin 子命令转发给该 profile 目录里的 pnpm）。
//
// 说明：dsh 官方没有内置 MCP（不存在 @deepseek-ai/dsh-mcp），MCP 能力由第三方
// 插件提供（如 dsh-mcp-adapter），因此不单列 MCP 页 —— 装它就是装一个插件。
//
// 布局：profile 下拉（切换后即时重列）+ 已装插件表（名/版本）+ 安装/卸载操作
//       + bundles 摘要 + 操作日志。
// 插件列表读 <DSH_HOME>/profiles/<p>/package.json 的 dependencies（同步、无子进程）；
// 安装/卸载走 DshManager 的异步 QProcess。

#include <QList>
#include <QWidget>

#include "dsh/DshManager.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

namespace cubeshell {

class DshPluginPanel : public QWidget {
    Q_OBJECT
public:
    // manager 不被本页持有，须比本页存活更久（由 DshPanel 提供）。
    explicit DshPluginPanel(DshManager *manager, QWidget *parent = nullptr);

public slots:
    // 重新列举 profile 与当前 profile 的插件（Tab 激活时由容器调用）。
    void refresh();

private slots:
    void onProfileChanged(int index);
    void onInstallClicked();
    void onRemoveClicked();
    void onSelectionChanged();
    void onPluginLog(const QString &line);
    void onPluginOpFinished(bool ok, const QString &message);

private:
    void buildUi();
    void reloadPlugins();          // 按当前 profile 填表
    QString currentProfile() const;
    void setBusy(bool busy);       // 安装/卸载期间锁操作
    void appendLog(const QString &text);

    DshManager *m_manager = nullptr; // not owned

    QComboBox *m_profileCombo = nullptr;
    QLabel *m_bundlesLabel = nullptr;
    QTableWidget *m_table = nullptr;
    QLineEdit *m_packageEdit = nullptr;
    QPushButton *m_installBtn = nullptr;
    QPushButton *m_removeBtn = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QPlainTextEdit *m_logView = nullptr;
};

} // namespace cubeshell
