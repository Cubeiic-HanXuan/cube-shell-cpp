#pragma once

// AiSettingsDialog.h — AI 设置对话框（非敏感偏好写 ai.json，API Key 走系统钥匙串）。
// 对应Python: core/ai/ui.py::AISettingsDialog（行 27-214）

#include <QDialog>

#include "ai/AiPreferences.h"

class QCheckBox;
class QComboBox;
class QLineEdit;

namespace cubeshell {

class AiSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit AiSettingsDialog(QWidget *parent = nullptr);

private slots:
    // 对应Python: core/ai/ui.py:145 _on_provider_changed
    void onProviderChanged(int index);
    // 对应Python: core/ai/ui.py:183 _save
    void save();

private:
    // 对应Python: core/ai/ui.py:130 _init_provider_selection
    void initProviderSelection();

    AiPreferences m_prefs;                        // 对应Python: self._prefs

    QComboBox *m_providerCombo = nullptr;
    QComboBox *m_modelCombo = nullptr;
    QLineEdit *m_baseUrlEdit = nullptr;
    QLineEdit *m_maxTokensEdit = nullptr;
    QLineEdit *m_temperatureEdit = nullptr;
    QCheckBox *m_thinkingCheck = nullptr;
    QCheckBox *m_streamCheck = nullptr;
    QLineEdit *m_systemPromptEdit = nullptr;
    QLineEdit *m_keyEdit = nullptr;
};

} // namespace cubeshell
