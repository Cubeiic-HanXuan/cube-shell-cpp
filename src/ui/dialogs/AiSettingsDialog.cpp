#include "AiSettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

namespace cubeshell {

namespace {

// Python 用 str(float) 填充输入框（1.0 → "1.0"、0.7 → "0.7"）；
// QString::number 默认会把 1.0 写成 "1"，这里补回小数点保持显示一致。
// 对应Python: core/ai/ui.py:78 str(self._prefs.temperature)
QString pythonFloatText(double value)
{
    QString text = QString::number(value, 'g', 12);
    if (!text.contains(QLatin1Char('.')) && !text.contains(QLatin1Char('e'))
        && !text.contains(QLatin1Char('n')))   // nan / inf 不补
        text += QStringLiteral(".0");
    return text;
}

} // namespace

// 对应Python: core/ai/ui.py:36 AISettingsDialog.__init__
AiSettingsDialog::AiSettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("AI 设置"));
    setModal(true);
    setFixedWidth(520);

    m_prefs = AiPreferences::load();

    auto *layout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("aiSettingsTabs"));
    layout->addWidget(tabs);
    auto *chatPage = new QWidget(tabs);
    auto *chatLayout = new QVBoxLayout(chatPage);
    tabs->addTab(chatPage, tr("聊天模型"));

    auto *form = new QGridLayout();
    int row = 0;

    // ---- AI 服务提供商 ---- 对应Python: core/ai/ui.py:49-55
    form->addWidget(new QLabel(tr("AI 服务提供商"), this), row, 0);
    m_providerCombo = new QComboBox(this);
    m_providerCombo->setObjectName(QStringLiteral("chatProviderCombo"));
    const QList<ProviderPreset> presets = AiPreferences::providerPresets();
    for (const ProviderPreset &preset : presets)
        m_providerCombo->addItem(preset.name, preset.key);   // userData 存 key
    form->addWidget(m_providerCombo, row, 1);
    ++row;

    // ---- 模型（可编辑下拉框） ---- 对应Python: core/ai/ui.py:57-62
    form->addWidget(new QLabel(tr("模型"), this), row, 0);
    m_modelCombo = new QComboBox(this);
    m_modelCombo->setEditable(true);
    form->addWidget(m_modelCombo, row, 1);
    ++row;

    // ---- Base URL ---- 对应Python: core/ai/ui.py:64-68
    form->addWidget(new QLabel(tr("Base URL(可选)"), this), row, 0);
    m_baseUrlEdit = new QLineEdit(m_prefs.baseUrl, this);
    form->addWidget(m_baseUrlEdit, row, 1);
    ++row;

    // ---- max_tokens ---- 对应Python: core/ai/ui.py:70-74（标签未走 tr()）
    form->addWidget(new QLabel(QStringLiteral("max_tokens"), this), row, 0);
    m_maxTokensEdit = new QLineEdit(QString::number(m_prefs.maxTokens), this);
    form->addWidget(m_maxTokensEdit, row, 1);
    ++row;

    // ---- temperature ---- 对应Python: core/ai/ui.py:76-80（标签未走 tr()）
    form->addWidget(new QLabel(QStringLiteral("temperature"), this), row, 0);
    m_temperatureEdit = new QLineEdit(pythonFloatText(m_prefs.temperature), this);
    form->addWidget(m_temperatureEdit, row, 1);
    ++row;

    // ---- 深度思考 ---- 对应Python: core/ai/ui.py:82-86（只占 column 1）
    m_thinkingCheck = new QCheckBox(tr("启用深度思考"), this);
    m_thinkingCheck->setChecked(m_prefs.thinkingEnabled);
    form->addWidget(m_thinkingCheck, row, 1);
    ++row;

    // ---- 流式输出 ---- 对应Python: core/ai/ui.py:88-92（只占 column 1）
    m_streamCheck = new QCheckBox(tr("启用流式输出"), this);
    m_streamCheck->setChecked(m_prefs.stream);
    form->addWidget(m_streamCheck, row, 1);
    ++row;

    // ---- 系统提示词 ---- 对应Python: core/ai/ui.py:94-98
    form->addWidget(new QLabel(tr("系统提示词"), this), row, 0);
    m_systemPromptEdit = new QLineEdit(m_prefs.systemPrompt, this);
    form->addWidget(m_systemPromptEdit, row, 1);
    ++row;

    chatLayout->addLayout(form);

    // ---- API Key ---- 对应Python: core/ai/ui.py:102-109
    auto *keyBox = new QHBoxLayout();
    keyBox->addWidget(new QLabel(tr("API Key"), this));
    m_keyEdit = new QLineEdit(this);
    m_keyEdit->setObjectName(QStringLiteral("chatApiKeyEdit"));
    m_keyEdit->setEchoMode(QLineEdit::Password);
    m_keyEdit->setPlaceholderText(tr("使用系统钥匙串保存，不写入配置文件"));
    keyBox->addWidget(m_keyEdit);
    chatLayout->addLayout(keyBox);
    chatLayout->addStretch();

    // Speech credentials must not follow the selected chat provider.
    auto *voicePage = new QWidget(tabs);
    auto *voiceLayout = new QVBoxLayout(voicePage);
    tabs->addTab(voicePage, tr("语音识别"));
    auto *voiceHint = new QLabel(tr(
        "语音输入使用独立的语音转文字服务，不使用聊天模型的 API Key。\n"
        "默认使用智谱 GLM-ASR，请填写可调用该语音接口的智谱开放平台 API Key。"
        "也可配置兼容 OpenAI 音频转写协议的服务。"), voicePage);
    voiceHint->setWordWrap(true);
    voiceLayout->addWidget(voiceHint);

    auto *voiceForm = new QGridLayout();
    voiceForm->setColumnStretch(1, 1);
    m_voiceUrlEdit = new QLineEdit(m_prefs.voiceTranscriptionUrl, voicePage);
    m_voiceUrlEdit->setObjectName(QStringLiteral("voiceTranscriptionUrlEdit"));
    m_voiceUrlEdit->setToolTip(tr("完整转写接口地址，包含 /audio/transcriptions；不是聊天 Base URL"));
    voiceForm->addWidget(new QLabel(tr("转写接口 URL"), voicePage), 0, 0);
    voiceForm->addWidget(m_voiceUrlEdit, 0, 1);
    m_voiceModelEdit = new QLineEdit(m_prefs.voiceModel, voicePage);
    m_voiceModelEdit->setObjectName(QStringLiteral("voiceModelEdit"));
    voiceForm->addWidget(new QLabel(tr("语音模型"), voicePage), 1, 0);
    voiceForm->addWidget(m_voiceModelEdit, 1, 1);
    m_voiceKeyEdit = new QLineEdit(voicePage);
    m_voiceKeyEdit->setObjectName(QStringLiteral("voiceApiKeyEdit"));
    m_voiceKeyEdit->setEchoMode(QLineEdit::Password);
    const bool voiceKeyFromEnv = !qEnvironmentVariable("CUBESHELL_VOICE_API_KEY").trimmed().isEmpty();
    m_voiceKeyEdit->setEnabled(!voiceKeyFromEnv);
    m_voiceKeyEdit->setPlaceholderText(voiceKeyFromEnv
        ? tr("由 CUBESHELL_VOICE_API_KEY 环境变量提供")
        : m_prefs.voiceApiKey().isEmpty()
            ? tr("独立保存到系统钥匙串，不写入配置文件")
            : tr("已保存（留空保留，输入新值可覆盖）"));
    voiceForm->addWidget(new QLabel(tr("语音 API Key"), voicePage), 2, 0);
    voiceForm->addWidget(m_voiceKeyEdit, 2, 1);
    voiceLayout->addLayout(voiceForm);
    auto *endpointHint = new QLabel(tr(
        "请只填写可信的服务地址，录音和语音 API Key 将发送至此地址。"
        "保存后，下次点击语音按钮即可生效。"), voicePage);
    endpointHint->setWordWrap(true);
    voiceLayout->addWidget(endpointHint);
#ifndef CUBESHELL_WITH_VOICE
    auto *unavailableHint = new QLabel(tr("当前构建未启用语音输入，配置可保存，但无法录音。"), voicePage);
    unavailableHint->setWordWrap(true);
    voiceLayout->addWidget(unavailableHint);
#endif
    voiceLayout->addStretch();

    // ---- 按钮 ---- 对应Python: core/ai/ui.py:111-120
    auto *btns = new QHBoxLayout();
    auto *saveBtn = new QPushButton(tr("保存"), this);
    saveBtn->setCursor(Qt::PointingHandCursor);
    auto *cancelBtn = new QPushButton(tr("取消"), this);
    cancelBtn->setCursor(Qt::PointingHandCursor);
    btns->addStretch(1);
    btns->addWidget(saveBtn);
    btns->addWidget(cancelBtn);
    layout->addLayout(btns);

    // ---- 信号连接 ---- 对应Python: core/ai/ui.py:122-125
    connect(m_providerCombo, &QComboBox::currentIndexChanged,
            this, &AiSettingsDialog::onProviderChanged);
    connect(saveBtn, &QPushButton::clicked, this, &AiSettingsDialog::save);
    connect(cancelBtn, &QPushButton::clicked, this, &AiSettingsDialog::reject);

    // ---- 初始化：根据已保存的 provider 设置下拉框选中项 ----
    initProviderSelection();
}

// 对应Python: core/ai/ui.py:130 _init_provider_selection
void AiSettingsDialog::initProviderSelection()
{
    int idx = m_providerCombo->findData(m_prefs.provider);
    if (idx < 0)
        idx = 0;
    m_providerCombo->blockSignals(true);
    m_providerCombo->setCurrentIndex(idx);
    m_providerCombo->blockSignals(false);
    // 手动触发一次联动（信号被屏蔽，需显式调用）。
    onProviderChanged(idx);
    // 联动会把模型下拉重置到 preset 首项，这里恢复用户保存的模型名
    //（可能是不在列表里的自定义值）。对应Python: core/ai/ui.py:143
    m_modelCombo->setCurrentText(m_prefs.model);
    // Opening the voice tab must not reset a saved chat endpoint to its preset.
    if (!m_prefs.baseUrl.isEmpty())
        m_baseUrlEdit->setText(m_prefs.baseUrl);
}

// 对应Python: core/ai/ui.py:145 _on_provider_changed
void AiSettingsDialog::onProviderChanged(int index)
{
    const QString providerKey = m_providerCombo->itemData(index).toString();
    const ProviderPreset preset = AiPreferences::providerPreset(providerKey);

    // custom 保留用户当前填写的 Base URL（Python 同样不做清空）。
    // 对应Python: core/ai/ui.py:157-158
    if (providerKey != QLatin1String("custom"))
        m_baseUrlEdit->setText(preset.baseUrl);

    // 重填模型列表。对应Python: core/ai/ui.py:161-167
    m_modelCombo->blockSignals(true);
    m_modelCombo->clear();
    for (const QString &modelName : preset.models)
        m_modelCombo->addItem(modelName);
    if (!preset.models.isEmpty())
        m_modelCombo->setCurrentIndex(0);
    m_modelCombo->blockSignals(false);

    // 深度思考仅对支持的 provider 可用。对应Python: core/ai/ui.py:169-173
    m_thinkingCheck->setEnabled(preset.supportsThinking);
    if (!preset.supportsThinking)
        m_thinkingCheck->setChecked(false);

    // 按 provider 读取已存 Key，仅用于更新占位符提示（不回显明文）。
    // 对应Python: core/ai/ui.py:175-181
    AiPreferences probe;
    probe.provider = providerKey;
    if (!probe.apiKey().isEmpty())
        m_keyEdit->setPlaceholderText(tr("已保存（输入新值可覆盖）"));
    else
        m_keyEdit->setPlaceholderText(tr("使用系统钥匙串保存，不写入配置文件"));
    m_keyEdit->clear();
}

// 对应Python: core/ai/ui.py:183 _save
void AiSettingsDialog::save()
{
    const QString provider = m_providerCombo->currentData().toString();

    AiPreferences prefs = m_prefs;
    prefs.provider = provider;
    // 空值回退默认。对应Python: core/ai/ui.py:189-199 的 `or "..."`
    const QString model = m_modelCombo->currentText().trimmed();
    prefs.model = model.isEmpty() ? QStringLiteral("glm-4-plus") : model;
    prefs.baseUrl = m_baseUrlEdit->text().trimmed();
    prefs.thinkingEnabled = m_thinkingCheck->isChecked();
    prefs.stream = m_streamCheck->isChecked();

    // Python 用 int()/float() 转换，失败抛异常并落到 except 分支弹窗；
    // 这里用 toInt/toDouble(&ok) 等价实现（不关闭对话框）。
    // 对应Python: core/ai/ui.py:196-197 + 212-213
    QString maxTokensText = m_maxTokensEdit->text().trimmed();
    if (maxTokensText.isEmpty())
        maxTokensText = QStringLiteral("8192");
    bool ok = false;
    const int maxTokens = maxTokensText.toInt(&ok);
    if (!ok) {
        // 括号内文案照抄 Python ValueError 的原文，保持弹窗内容一致。
        QMessageBox::warning(
            this, tr("错误"),
            tr("保存失败: %1")
                .arg(QStringLiteral("invalid literal for int() with base 10: '%1'")
                         .arg(maxTokensText)));
        return;
    }
    prefs.maxTokens = maxTokens;

    QString temperatureText = m_temperatureEdit->text().trimmed();
    if (temperatureText.isEmpty())
        temperatureText = QStringLiteral("1.0");
    const double temperature = temperatureText.toDouble(&ok);
    if (!ok) {
        QMessageBox::warning(
            this, tr("错误"),
            tr("保存失败: %1")
                .arg(QStringLiteral("could not convert string to float: '%1'")
                         .arg(temperatureText)));
        return;
    }
    prefs.temperature = temperature;

    const QString systemPrompt = m_systemPromptEdit->text().trimmed();
    prefs.systemPrompt = systemPrompt.isEmpty()
        ? QStringLiteral("你是一个资深 Linux 运维与终端助手。输出尽量可执行、可复制。")
        : systemPrompt;

    prefs.voiceTranscriptionUrl = m_voiceUrlEdit->text().trimmed();
    prefs.voiceModel = m_voiceModelEdit->text().trimmed();
    const QString voiceError = prefs.voiceConfigurationError();
    if (!voiceError.isEmpty()) {
        QMessageBox::warning(this, tr("错误"), voiceError);
        return;
    }

    // API Key 非空才写钥匙串（绝不写入 ai.json）。对应Python: core/ai/ui.py:203-208
    const QString key = m_keyEdit->text().trimmed();
    if (!key.isEmpty()) {
        AiPreferences keyTarget;
        keyTarget.provider = provider;
        if (!keyTarget.setApiKey(key)) {
            QMessageBox::warning(this, tr("错误"), tr("保存 API Key 失败"));
            return;
        }
    }

    const QString voiceKey = m_voiceKeyEdit->text().trimmed();
    if (!voiceKey.isEmpty() && !prefs.setVoiceApiKey(voiceKey)) {
        QMessageBox::warning(this, tr("错误"), tr("保存语音 API Key 失败，请检查系统钥匙串是否可用"));
        return;
    }

    // Persist the endpoint only after its credentials were successfully stored.
    QString saveError;
    if (!prefs.save(&saveError)) {
        QMessageBox::warning(this, tr("错误"), tr("保存失败: %1").arg(saveError));
        return;
    }

    m_keyEdit->clear();
    m_voiceKeyEdit->clear();
    accept();
}

} // namespace cubeshell
