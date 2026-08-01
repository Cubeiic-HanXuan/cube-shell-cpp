// SettingsPanel.cpp — see SettingsPanel.h for the port map.
// 对应Python: core/claude_code/settings_widget.py

#include "claude_code/SettingsPanel.h"

#include "claude_code/ClaudeCodeBackend.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

namespace cubeshell {

// 鉴权 token / 模型相关的所有可能键（对应Python: 行 353-359）
static const char *kTokenKeys[] = {"ANTHROPIC_AUTH_TOKEN",
                                   "ANTHROPIC_API_KEY"};
static const char *kModelKeys[] = {"ANTHROPIC_MODEL",
                                   "ANTHROPIC_DEFAULT_OPUS_MODEL",
                                   "ANTHROPIC_DEFAULT_SONNET_MODEL",
                                   "ANTHROPIC_DEFAULT_HAIKU_MODEL"};

SettingsPanel::SettingsPanel(ClaudeCodeBackend *backend, QWidget *parent)
    : QWidget(parent)
    , m_backend(backend)
    , m_presets(AiPreferences::providerPresets()) // 对应Python: _PROVIDER_PRESETS
{
    buildUi();
    // backend 信号来自工作线程，必须 QueuedConnection 切回 UI 线程
    connect(m_backend, &ClaudeCodeBackend::settingsLoaded,
            this, &SettingsPanel::onSettingsLoaded, Qt::QueuedConnection);
    connect(m_backend, &ClaudeCodeBackend::settingsSaved,
            this, &SettingsPanel::onSettingsSaved, Qt::QueuedConnection);
}

// 对应Python: SettingsWidget._init_ui（行 72-172）
void SettingsPanel::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(12);

    // --- 模型配置 ---
    auto *modelGroup = new QGroupBox(tr("模型配置"), this);
    auto *modelLayout = new QFormLayout(modelGroup);

    m_modelCombo = new QComboBox(modelGroup);
    m_modelCombo->setEditable(true);
    m_modelCombo->addItems({QStringLiteral("sonnet"), QStringLiteral("opus"),
                            QStringLiteral("haiku")});
    m_modelCombo->setMinimumWidth(250);
    modelLayout->addRow(tr("模型选择:"), m_modelCombo);

    m_effortCombo = new QComboBox(modelGroup);
    m_effortCombo->addItems({QStringLiteral("low"), QStringLiteral("medium"),
                             QStringLiteral("high"), QStringLiteral("xhigh"),
                             QStringLiteral("max")});
    m_effortCombo->setCurrentText(QStringLiteral("high"));
    modelLayout->addRow(tr("Effort 级别:"), m_effortCombo);

    layout->addWidget(modelGroup);

    // --- 模型提供商 ---
    auto *providerGroup = new QGroupBox(tr("模型提供商"), this);
    auto *providerLayout = new QFormLayout(providerGroup);

    m_providerCombo = new QComboBox(providerGroup);
    for (const ProviderPreset &preset : m_presets)
        m_providerCombo->addItem(preset.name);
    m_providerCombo->setMinimumWidth(250);
    connect(m_providerCombo, &QComboBox::currentIndexChanged,
            this, &SettingsPanel::onProviderChanged);
    providerLayout->addRow(tr("提供商预设:"), m_providerCombo);

    m_baseUrlEdit = new QLineEdit(providerGroup);
    m_baseUrlEdit->setPlaceholderText(tr("API 地址"));
    m_baseUrlEdit->setMinimumWidth(400);
    m_baseUrlEdit->setEnabled(false);
    providerLayout->addRow(tr("API 地址:"), m_baseUrlEdit);

    m_apiKeyEdit = new QLineEdit(providerGroup);
    m_apiKeyEdit->setEchoMode(QLineEdit::Password);
    m_apiKeyEdit->setPlaceholderText(tr("输入 API Key"));
    m_apiKeyEdit->setMinimumWidth(400);
    m_apiKeyEdit->setEnabled(false);
    providerLayout->addRow(tr("API 密钥:"), m_apiKeyEdit);

    m_providerModelCombo = new QComboBox(providerGroup);
    m_providerModelCombo->setEditable(true);
    m_providerModelCombo->setMinimumWidth(250);
    m_providerModelCombo->setEnabled(false);
    providerLayout->addRow(tr("模型名称:"), m_providerModelCombo);

    layout->addWidget(providerGroup);

    // --- 权限配置 ---
    auto *permGroup = new QGroupBox(tr("权限配置"), this);
    auto *permLayout = new QFormLayout(permGroup);

    m_modeCombo = new QComboBox(permGroup);
    m_modeCombo->addItems({QStringLiteral("default"),
                           QStringLiteral("acceptEdits"),
                           QStringLiteral("plan"), QStringLiteral("auto"),
                           QStringLiteral("dontAsk"),
                           QStringLiteral("bypassPermissions")});
    permLayout->addRow(tr("权限模式:"), m_modeCombo);

    m_allowedToolsEdit = new QLineEdit(permGroup);
    m_allowedToolsEdit->setPlaceholderText(QStringLiteral("Read,Edit,Bash"));
    permLayout->addRow(tr("Allowed Tools:"), m_allowedToolsEdit);

    layout->addWidget(permGroup);

    // --- 提示词配置 ---
    auto *promptGroup = new QGroupBox(tr("提示词配置"), this);
    auto *promptLayout = new QVBoxLayout(promptGroup);

    promptLayout->addWidget(new QLabel(tr("自定义系统提示词:"), promptGroup));

    m_promptEdit = new QTextEdit(promptGroup);
    m_promptEdit->setPlaceholderText(
        tr("在此输入追加到系统提示词末尾的自定义内容..."));
    m_promptEdit->setMinimumHeight(100);
    promptLayout->addWidget(m_promptEdit);

    layout->addWidget(promptGroup);

    // --- 操作按钮 ---
    auto *btnLayout = new QHBoxLayout;
    btnLayout->addStretch();

    m_refreshBtn = new QPushButton(tr("加载设置"), this);
    connect(m_refreshBtn, &QPushButton::clicked,
            this, &SettingsPanel::refresh);
    btnLayout->addWidget(m_refreshBtn);

    m_saveBtn = new QPushButton(tr("保存设置"), this);
    connect(m_saveBtn, &QPushButton::clicked, this, &SettingsPanel::onSave);
    btnLayout->addWidget(m_saveBtn);

    layout->addLayout(btnLayout);
    layout->addStretch();
}

// 对应Python: refresh / _load_settings（行 178-193）
void SettingsPanel::refresh()
{
    loadSettings();
}

void SettingsPanel::loadSettings()
{
    if (!m_backend || m_loadPending)
        return;
    m_loadPending = true;
    m_refreshBtn->setEnabled(false);
    m_backend->refreshSettings();
}

// 对应Python: _on_settings_loaded（行 195-201）
void SettingsPanel::onSettingsLoaded(const QJsonObject &settings)
{
    m_loadPending = false;
    m_refreshBtn->setEnabled(true);
    m_settingsData = settings;
    populateUi(settings);
}

// 对应Python: _populate_ui（行 203-278）
void SettingsPanel::populateUi(const QJsonObject &settings)
{
    // 模型
    const QString model = settings.value(QStringLiteral("model")).toString();
    if (!model.isEmpty()) {
        const int idx = m_modelCombo->findText(model);
        if (idx >= 0)
            m_modelCombo->setCurrentIndex(idx);
        else
            m_modelCombo->setCurrentText(model);
    }

    // Effort 级别
    const QString effort = settings.value(QStringLiteral("effortLevel"))
                               .toString(QStringLiteral("high"));
    int idx = m_effortCombo->findText(effort);
    if (idx >= 0)
        m_effortCombo->setCurrentIndex(idx);

    // 模型提供商 (从 env 加载，兼容 AUTH_TOKEN / API_KEY 两种鉴权约定，
    // 以及 ANTHROPIC_MODEL / ANTHROPIC_DEFAULT_*_MODEL 两种模型写法)
    const QJsonObject env = settings.value(QStringLiteral("env")).toObject();
    const QString baseUrl =
        env.value(QStringLiteral("ANTHROPIC_BASE_URL")).toString();
    QString apiKey =
        env.value(QStringLiteral("ANTHROPIC_AUTH_TOKEN")).toString();
    if (apiKey.isEmpty())
        apiKey = env.value(QStringLiteral("ANTHROPIC_API_KEY")).toString();
    QString envModel;
    for (const char *key : kModelKeys) {
        envModel = env.value(QLatin1String(key)).toString();
        if (!envModel.isEmpty())
            break;
    }

    if (!baseUrl.isEmpty()) {
        // 反向匹配预设提供商（优先匹配 anthropic_base_url，其次 base_url）
        bool matched = false;
        for (int i = 0; i < m_presets.size(); ++i) {
            const ProviderPreset &preset = m_presets.at(i);
            if (!preset.anthropicBaseUrl.isEmpty()
                && preset.anthropicBaseUrl == baseUrl) {
                m_providerCombo->setCurrentIndex(i);
                matched = true;
                break;
            }
            if (!preset.baseUrl.isEmpty() && preset.baseUrl == baseUrl) {
                m_providerCombo->setCurrentIndex(i);
                matched = true;
                break;
            }
        }
        if (!matched) {
            // 选择"自定义"（预设列表末项）
            m_providerCombo->setCurrentIndex(m_presets.size() - 1);
            m_baseUrlEdit->setText(baseUrl);
        }
    } else {
        m_providerCombo->setCurrentIndex(0);
    }

    if (!apiKey.isEmpty())
        m_apiKeyEdit->setText(apiKey);
    if (!envModel.isEmpty()) {
        idx = m_providerModelCombo->findText(envModel);
        if (idx >= 0)
            m_providerModelCombo->setCurrentIndex(idx);
        else
            m_providerModelCombo->setCurrentText(envModel);
    }

    // 权限模式
    const QString defaultMode = settings.value(QStringLiteral("defaultMode"))
                                    .toString(QStringLiteral("default"));
    idx = m_modeCombo->findText(defaultMode);
    if (idx >= 0)
        m_modeCombo->setCurrentIndex(idx);

    // Allowed Tools
    const QJsonValue allowValue = settings.value(QStringLiteral("permissions"))
                                      .toObject()
                                      .value(QStringLiteral("allow"));
    if (allowValue.isArray()) {
        QStringList tools;
        const QJsonArray arr = allowValue.toArray();
        for (const QJsonValue &v : arr)
            tools.append(v.toString());
        m_allowedToolsEdit->setText(tools.join(QStringLiteral(", ")));
    } else if (allowValue.isString()) {
        m_allowedToolsEdit->setText(allowValue.toString());
    }

    // 系统提示词
    m_promptEdit->setPlainText(
        settings.value(QStringLiteral("appendSystemPrompt")).toString());
}

// 对应Python: _on_provider_changed（行 280-303）
void SettingsPanel::onProviderChanged(int index)
{
    if (index < 0 || index >= m_presets.size())
        return;
    const ProviderPreset &preset = m_presets.at(index);
    const bool isAnthropic = (index == 0);
    const bool isCustom = (index == m_presets.size() - 1);

    // 启用/禁用控件
    m_baseUrlEdit->setEnabled(!isAnthropic);
    m_apiKeyEdit->setEnabled(!isAnthropic);
    m_providerModelCombo->setEnabled(!isAnthropic);

    // 填充 API 地址（优先使用 anthropic_base_url）
    if (isAnthropic) {
        m_baseUrlEdit->clear();
        m_apiKeyEdit->clear();
    } else if (!isCustom) {
        m_baseUrlEdit->setText(preset.anthropicBaseUrl.isEmpty()
                                   ? preset.baseUrl
                                   : preset.anthropicBaseUrl);
    }
    // 自定义时保持用户已输入的内容

    // 更新模型列表
    m_providerModelCombo->clear();
    if (!preset.models.isEmpty())
        m_providerModelCombo->addItems(preset.models);
}

// 对应Python: _on_save（行 317-332）
void SettingsPanel::onSave()
{
    if (!m_backend) {
        QMessageBox::warning(this, tr("警告"), tr("未连接后端"));
        return;
    }
    if (m_savePending)
        return;
    const QJsonObject settings = buildSettingsJson();
    m_savePending = true;
    m_saveBtn->setEnabled(false);
    m_backend->saveSettings(settings);
}

// 对应Python: _build_settings_dict（行 334-418）
QJsonObject SettingsPanel::buildSettingsJson() const
{
    // 以现有 settings 为基础，避免丢失未展示的字段
    QJsonObject settings = m_settingsData;

    // 模型
    const QString modelText = m_modelCombo->currentText().trimmed();
    if (!modelText.isEmpty())
        settings.insert(QStringLiteral("model"), modelText);

    // Effort 级别
    settings.insert(QStringLiteral("effortLevel"),
                    m_effortCombo->currentText());

    // 模型提供商 env 配置
    const int providerIndex = m_providerCombo->currentIndex();
    const bool isAnthropic = (providerIndex == 0);
    QJsonObject env = settings.value(QStringLiteral("env")).toObject();

    if (isAnthropic) {
        // 移除提供商相关环境变量（对应Python: 行 361-371）
        env.remove(QStringLiteral("ANTHROPIC_BASE_URL"));
        for (const char *key : kTokenKeys)
            env.remove(QLatin1String(key));
        for (const char *key : kModelKeys)
            env.remove(QLatin1String(key));
        if (!env.isEmpty())
            settings.insert(QStringLiteral("env"), env);
        else
            settings.remove(QStringLiteral("env"));
    } else {
        const QString baseUrl = m_baseUrlEdit->text().trimmed();
        const QString apiKey = m_apiKeyEdit->text().trimmed();
        const QString providerModel =
            m_providerModelCombo->currentText().trimmed();
        if (!baseUrl.isEmpty())
            env.insert(QStringLiteral("ANTHROPIC_BASE_URL"), baseUrl);
        if (!apiKey.isEmpty()) {
            // 保留已有的 token 键名约定，缺省用 ANTHROPIC_AUTH_TOKEN
            // （第三方中转站常用），并清除另一个以避免冲突。
            const QString tokenKey =
                env.contains(QStringLiteral("ANTHROPIC_API_KEY"))
                    ? QStringLiteral("ANTHROPIC_API_KEY")
                    : QStringLiteral("ANTHROPIC_AUTH_TOKEN");
            for (const char *key : kTokenKeys) {
                if (QLatin1String(key) != tokenKey)
                    env.remove(QLatin1String(key));
            }
            env.insert(tokenKey, apiKey);
        }
        if (!providerModel.isEmpty()) {
            // 若已有 DEFAULT_*_MODEL 约定则沿用，否则写 ANTHROPIC_MODEL
            const bool hasDefaultKeys =
                env.contains(QStringLiteral("ANTHROPIC_DEFAULT_OPUS_MODEL"))
                || env.contains(
                    QStringLiteral("ANTHROPIC_DEFAULT_SONNET_MODEL"))
                || env.contains(
                    QStringLiteral("ANTHROPIC_DEFAULT_HAIKU_MODEL"));
            if (hasDefaultKeys) {
                env.remove(QStringLiteral("ANTHROPIC_MODEL"));
                env.insert(QStringLiteral("ANTHROPIC_DEFAULT_OPUS_MODEL"),
                           providerModel);
                env.insert(QStringLiteral("ANTHROPIC_DEFAULT_SONNET_MODEL"),
                           providerModel);
                env.insert(QStringLiteral("ANTHROPIC_DEFAULT_HAIKU_MODEL"),
                           providerModel);
            } else {
                env.insert(QStringLiteral("ANTHROPIC_MODEL"), providerModel);
            }
        }
        settings.insert(QStringLiteral("env"), env);
    }

    // 权限模式
    settings.insert(QStringLiteral("defaultMode"),
                    m_modeCombo->currentText());

    // Allowed Tools
    const QString toolsText = m_allowedToolsEdit->text().trimmed();
    QJsonObject permissions =
        settings.value(QStringLiteral("permissions")).toObject();
    if (!toolsText.isEmpty()) {
        QJsonArray allowList;
        const QStringList parts = toolsText.split(QLatin1Char(','));
        for (const QString &part : parts) {
            const QString t = part.trimmed();
            if (!t.isEmpty())
                allowList.append(t);
        }
        permissions.insert(QStringLiteral("allow"), allowList);
        settings.insert(QStringLiteral("permissions"), permissions);
    } else if (settings.contains(QStringLiteral("permissions"))) {
        // 清空
        permissions.insert(QStringLiteral("allow"), QJsonArray());
        settings.insert(QStringLiteral("permissions"), permissions);
    }

    // 系统提示词
    const QString promptText = m_promptEdit->toPlainText().trimmed();
    if (!promptText.isEmpty())
        settings.insert(QStringLiteral("appendSystemPrompt"), promptText);
    else
        settings.remove(QStringLiteral("appendSystemPrompt"));

    return settings;
}

// 对应Python: _on_settings_saved（行 420-428）
void SettingsPanel::onSettingsSaved(bool ok, const QString &message)
{
    m_savePending = false;
    m_saveBtn->setEnabled(true);
    if (ok) {
        QMessageBox::information(this, tr("成功"), tr("设置已保存"));
    } else {
        QMessageBox::critical(this, tr("错误"),
                              tr("保存设置失败: %1").arg(message));
    }
}

} // namespace cubeshell
