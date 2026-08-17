#pragma once

// DshSettingsPanel.h — DeepSeek Harness 设置管理 Tab。
//
// dsh 的用户设置是 <DSH_HOME>/settings.yaml，形如：
//     ui-theme:
//       preference: dark
//     agent-default-model:
//       provider: deepseek-official
//       model: deepseek-v4-pro
//       reasoningEffort: max
//
// 键集由各插件贡献（"Everything is a Plugin"），没有固定 schema —— 用表单化
// 控件去枚举字段必然覆盖不全且随插件升级失效。因此本页提供文本编辑 + 摘要：
//   * 上部：关键项摘要（从文本里按缩进路径提取，只读展示）
//   * 下部：settings.yaml 全文编辑器 + 保存 / 重载
// 保存前做一次最小校验（缩进用空格、不出现 Tab），避免写坏 YAML。
// 编辑器套 YamlHighlighter 做语法高亮，与 Hermes 的 config.yaml 编辑器同一份实现，
// 配色天然一致。
//
// 凭据在 <DSH_HOME>/.credentials.yaml，属敏感文件，本页不读取也不展示。

#include <QWidget>

#include "dsh/DshManager.h"

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace cubeshell {

class YamlHighlighter;

class DshSettingsPanel : public QWidget {
    Q_OBJECT
public:
    explicit DshSettingsPanel(DshManager *manager, QWidget *parent = nullptr);

public slots:
    // 从磁盘重新载入 settings.yaml（Tab 激活时由容器调用）。
    void refresh();

private slots:
    void onSaveClicked();
    void onReloadClicked();
    void onTextChanged();

private:
    void buildUi();
    void updateSummary(const QString &yaml);
    // 提取 "section: / key: value" 形式的值；未命中返回空。
    static QString yamlValue(const QString &yaml, const QString &section,
                             const QString &key);

    DshManager *m_manager = nullptr; // not owned

    QLabel *m_pathLabel = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QPlainTextEdit *m_editor = nullptr;
    YamlHighlighter *m_highlighter = nullptr; // owned by editor's document
    QPushButton *m_saveBtn = nullptr;
    QPushButton *m_reloadBtn = nullptr;
    bool m_dirty = false;
};

} // namespace cubeshell
