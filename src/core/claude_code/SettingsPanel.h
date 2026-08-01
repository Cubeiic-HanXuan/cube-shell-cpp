#pragma once

// SettingsPanel.h — Claude Code 设置 Tab：模型/提供商/权限/提示词配置的
// 可视化管理（读写 ~/.claude/settings.json）。
// 对应Python: core/claude_code/settings_widget.py::SettingsWidget

#include <QJsonObject>
#include <QWidget>

#include "ai/AiPreferences.h"

class QComboBox;
class QLineEdit;
class QPushButton;
class QTextEdit;

namespace cubeshell {

class ClaudeCodeBackend;

class SettingsPanel : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPanel(ClaudeCodeBackend *backend,
                           QWidget *parent = nullptr);

public slots:
    // 对应Python: SettingsWidget.refresh（Tab 激活/模式切换时调用）
    void refresh();

private slots:
    void onProviderChanged(int index);
    void onSave();
    void onSettingsLoaded(const QJsonObject &settings);
    void onSettingsSaved(bool ok, const QString &message);

private:
    void buildUi();
    void loadSettings();
    // 对应Python: _populate_ui（settings dict → UI 控件）
    void populateUi(const QJsonObject &settings);
    // 对应Python: _build_settings_dict（UI 控件 → settings dict，
    // env 的 ANTHROPIC_* 约定见 Python 行 347-396）
    QJsonObject buildSettingsJson() const;

    ClaudeCodeBackend *m_backend = nullptr; // not owned（Panel 持有）
    QList<ProviderPreset> m_presets;        // 对应Python: _PROVIDER_PRESETS
    QJsonObject m_settingsData;             // 保留未展示字段
    bool m_loadPending = false;
    bool m_savePending = false;

    QComboBox *m_modelCombo = nullptr;
    QComboBox *m_effortCombo = nullptr;
    QComboBox *m_providerCombo = nullptr;
    QLineEdit *m_baseUrlEdit = nullptr;
    QLineEdit *m_apiKeyEdit = nullptr;
    QComboBox *m_providerModelCombo = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QLineEdit *m_allowedToolsEdit = nullptr;
    QTextEdit *m_promptEdit = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QPushButton *m_saveBtn = nullptr;
};

} // namespace cubeshell
