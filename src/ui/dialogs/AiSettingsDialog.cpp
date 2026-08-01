#include "AiSettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
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

    auto *form = new QGridLayout();
    int row = 0;

    // ---- AI 服务提供商 ---- 对应Python: core/ai/ui.py:49-55
    form->addWidget(new QLabel(tr("AI 服务提供商"), this), row, 0);
    m_providerCombo = new QComboBox(this);
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

    layout->addLayout(form);

    // ---- API Key ---- 对应Python: core/ai/ui.py:102-109
    auto *keyBox = new QHBoxLayout();
    keyBox->addWidget(new QLabel(tr("API Key"), this));
    m_keyEdit = new QLineEdit(this);
    m_keyEdit->setEchoMode(QLineEdit::Password);
    m_keyEdit->setPlaceholderText(tr("使用系统钥匙串保存，不写入配置文件"));
    keyBox->addWidget(m_keyEdit);
    layout->addLayout(keyBox);

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

    AiPreferences prefs;
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

    // Python 侧 save_ai_prefs 内部吞掉异常只记日志；C++ 侧 save() 返回写入结果，
    // 这里复用 Python 的错误文案把真实 I/O 失败暴露给用户（差异 D1）。
    QString saveError;
    if (!prefs.save(&saveError)) {
        QMessageBox::warning(this, tr("错误"), tr("保存失败: %1").arg(saveError));
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

    m_keyEdit->clear();
    accept();
}

} // namespace cubeshell
