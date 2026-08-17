// DshSettingsPanel.cpp — 设置管理 Tab。见 DshSettingsPanel.h 的说明。

#include "dsh/DshSettingsPanel.h"

#include <QFont>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

// 通用的 header-only YAML 高亮器（无 hermes 依赖，那边也只是引用者之一）。
// 直接复用而不另写一份，是为了让本页与 Hermes 的 config.yaml 编辑器配色一致。
#include "hermes/HermesHighlighters.h"

namespace cubeshell {

DshSettingsPanel::DshSettingsPanel(DshManager *manager, QWidget *parent)
    : QWidget(parent)
    , m_manager(manager)
{
    buildUi();
}

void DshSettingsPanel::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    // --- 顶部：文件路径 ---
    m_pathLabel = new QLabel(this);
    m_pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_pathLabel);

    // --- 关键项摘要 ---
    auto *sumBox = new QGroupBox(tr("关键设置"), this);
    auto *sumLay = new QVBoxLayout(sumBox);
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sumLay->addWidget(m_summaryLabel);
    root->addWidget(sumBox);

    // --- 全文编辑器 ---
    auto *editBox = new QGroupBox(tr("settings.yaml"), this);
    auto *editLay = new QVBoxLayout(editBox);
    m_editor = new QPlainTextEdit(this);
    // 等宽字体：YAML 靠缩进表达层级，等宽才看得准。
    m_editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setTabChangesFocus(false);
    // 语法高亮（键名/字符串/数字/布尔/注释）。挂在 document 上，由其接管生命周期；
    // 后续 setPlainText 会自动触发重新着色。
    m_highlighter = new YamlHighlighter(m_editor->document());
    editLay->addWidget(m_editor);
    root->addWidget(editBox, 1);

    connect(m_editor, &QPlainTextEdit::textChanged, this, &DshSettingsPanel::onTextChanged);

    // --- 操作行 ---
    auto *opRow = new QHBoxLayout();
    opRow->addStretch();
    m_reloadBtn = new QPushButton(tr("重新载入"), this);
    m_saveBtn = new QPushButton(tr("保存"), this);
    opRow->addWidget(m_reloadBtn);
    opRow->addWidget(m_saveBtn);
    root->addLayout(opRow);

    connect(m_reloadBtn, &QPushButton::clicked, this, &DshSettingsPanel::onReloadClicked);
    connect(m_saveBtn, &QPushButton::clicked, this, &DshSettingsPanel::onSaveClicked);

    m_saveBtn->setEnabled(false);
}

// 在 "section:" 块内找 "  key: value"（两级、空格缩进的常见形态）。
QString DshSettingsPanel::yamlValue(const QString &yaml, const QString &section,
                                    const QString &key)
{
    const QStringList lines = yaml.split(QLatin1Char('\n'));
    bool inSection = false;
    for (const QString &raw : lines) {
        const QString line = raw;
        if (line.trimmed().isEmpty() || line.trimmed().startsWith(QLatin1Char('#')))
            continue;
        // 顶层键（无前导空格）
        if (!line.startsWith(QLatin1Char(' ')) && !line.startsWith(QLatin1Char('\t'))) {
            inSection = line.trimmed().startsWith(section + QLatin1Char(':'));
            continue;
        }
        if (!inSection)
            continue;
        const QString t = line.trimmed();
        if (t.startsWith(key + QLatin1Char(':'))) {
            QString v = t.mid(key.size() + 1).trimmed();
            // 去掉可能的引号
            if (v.size() >= 2 && ((v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')))
                                  || (v.startsWith(QLatin1Char('\'')) && v.endsWith(QLatin1Char('\'')))))
                v = v.mid(1, v.size() - 2);
            return v;
        }
    }
    return QString();
}

void DshSettingsPanel::updateSummary(const QString &yaml)
{
    if (yaml.trimmed().isEmpty()) {
        m_summaryLabel->setText(
            tr("settings.yaml 尚未生成或为空。首次运行 dsh（web/tui）后会自动创建。"));
        return;
    }
    const QString provider =
        yamlValue(yaml, QStringLiteral("agent-default-model"), QStringLiteral("provider"));
    const QString model =
        yamlValue(yaml, QStringLiteral("agent-default-model"), QStringLiteral("model"));
    const QString effort =
        yamlValue(yaml, QStringLiteral("agent-default-model"), QStringLiteral("reasoningEffort"));
    const QString theme =
        yamlValue(yaml, QStringLiteral("ui-theme"), QStringLiteral("preference"));
    const QString preset =
        yamlValue(yaml, QStringLiteral("agent-presets"), QStringLiteral("default"));

    const auto orDash = [](const QString &s) {
        return s.isEmpty() ? QStringLiteral("-") : s;
    };
    m_summaryLabel->setText(tr("默认模型: %1 / %2 · 推理强度: %3\n"
                               "界面主题: %4 · 默认预设: %5")
                                .arg(orDash(provider), orDash(model), orDash(effort),
                                     orDash(theme), orDash(preset)));
}

void DshSettingsPanel::refresh()
{
    m_pathLabel->setText(tr("配置文件：%1").arg(DshManager::settingsPath()));

    QString error;
    const QString yaml = DshManager::readSettings(&error);
    if (!error.isEmpty()) {
        m_summaryLabel->setText(error);
        return;
    }
    {
        // 载入不算用户改动。
        const QSignalBlocker blocker(m_editor);
        m_editor->setPlainText(yaml);
    }
    m_dirty = false;
    m_saveBtn->setEnabled(false);
    updateSummary(yaml);
}

void DshSettingsPanel::onTextChanged()
{
    m_dirty = true;
    m_saveBtn->setEnabled(true);
}

void DshSettingsPanel::onReloadClicked()
{
    if (m_dirty) {
        const auto btn = QMessageBox::question(
            this, tr("重新载入"),
            tr("当前有未保存的修改，重新载入会丢弃它们。继续?"));
        if (btn != QMessageBox::Yes)
            return;
    }
    refresh();
}

void DshSettingsPanel::onSaveClicked()
{
    const QString content = m_editor->toPlainText();
    // 最小校验：YAML 禁止用 Tab 缩进，误用会让 dsh 启动即报解析错。
    const QStringList lines = content.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        const int firstNonSpace = int(line.size() - QStringView(line).trimmed().size());
        Q_UNUSED(firstNonSpace);
        if (line.startsWith(QLatin1Char('\t'))
            || line.left(line.size() - line.trimmed().size()).contains(QLatin1Char('\t'))) {
            QMessageBox::warning(
                this, tr("保存失败"),
                tr("第 %1 行用 Tab 缩进了。YAML 只允许空格缩进，请改掉后再保存。")
                    .arg(i + 1));
            return;
        }
    }

    QString error;
    if (!DshManager::writeSettings(content, &error)) {
        QMessageBox::warning(this, tr("保存失败"), error);
        return;
    }
    m_dirty = false;
    m_saveBtn->setEnabled(false);
    updateSummary(content);
    QMessageBox::information(
        this, tr("已保存"),
        tr("settings.yaml 已保存。已在运行的 dsh 需重启才会读到新配置。"));
}

} // namespace cubeshell
