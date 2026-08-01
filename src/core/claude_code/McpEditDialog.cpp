// McpEditDialog.cpp — see McpEditDialog.h for the port map.
// 对应Python: core/claude_code/mcp_widget.py::McpEditDialog（行 50-149）

#include "claude_code/McpEditDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QTextEdit>
#include <QVBoxLayout>

namespace cubeshell {

McpEditDialog::McpEditDialog(QWidget *parent, const QString &serverName,
                             const QJsonObject &serverConfig)
    : QDialog(parent)
{
    setWindowTitle(tr("编辑 MCP Server"));
    setMinimumWidth(560);
    setMinimumHeight(420);
    buildUi(serverName, serverConfig);
}

// 对应Python: McpEditDialog._init_ui（行 61-114）
void McpEditDialog::buildUi(const QString &serverName,
                            const QJsonObject &config)
{
    auto *layout = new QVBoxLayout(this);

    auto *formLayout = new QFormLayout;
    // 让输入框随对话框拉伸，避免宽度过窄
    formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    // Server 名称
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setMinimumWidth(380);
    m_nameEdit->setText(serverName);
    m_nameEdit->setPlaceholderText(QStringLiteral("server-name"));
    formLayout->addRow(tr("Server 名称:"), m_nameEdit);

    // Command
    m_commandEdit = new QLineEdit(this);
    m_commandEdit->setMinimumWidth(380);
    m_commandEdit->setText(config.value(QStringLiteral("command")).toString());
    m_commandEdit->setPlaceholderText(QStringLiteral("npx"));
    formLayout->addRow(tr("Command:"), m_commandEdit);

    // Args
    m_argsEdit = new QLineEdit(this);
    m_argsEdit->setMinimumWidth(380);
    const QJsonValue argsValue = config.value(QStringLiteral("args"));
    if (argsValue.isArray()) {
        QStringList parts;
        const QJsonArray arr = argsValue.toArray();
        for (const QJsonValue &v : arr)
            parts.append(v.isString() ? v.toString()
                                      : v.toVariant().toString());
        m_argsEdit->setText(parts.join(QStringLiteral(", ")));
    }
    m_argsEdit->setPlaceholderText(
        QStringLiteral("@some/mcp-server, --port, 3000"));
    formLayout->addRow(tr("Args (逗号分隔):"), m_argsEdit);

    // Env
    formLayout->addRow(new QLabel(tr("Env (KEY=VALUE，每行一个):"), this));

    m_envEdit = new QTextEdit(this);
    m_envEdit->setMinimumHeight(140);
    const QJsonValue envValue = config.value(QStringLiteral("env"));
    if (envValue.isObject()) {
        QStringList envLines;
        const QJsonObject envObj = envValue.toObject();
        for (auto it = envObj.begin(); it != envObj.end(); ++it) {
            const QString value = it.value().isString()
                                      ? it.value().toString()
                                      : it.value().toVariant().toString();
            envLines.append(QStringLiteral("%1=%2").arg(it.key(), value));
        }
        m_envEdit->setPlainText(envLines.join(QLatin1Char('\n')));
    }
    m_envEdit->setPlaceholderText(QStringLiteral("API_KEY=xxx\nDEBUG=true"));
    formLayout->addRow(m_envEdit);

    layout->addLayout(formLayout);

    // 按钮
    auto *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttonBox);
}

// 对应Python: get_server_name（行 116-118）
QString McpEditDialog::serverName() const
{
    return m_nameEdit->text().trimmed();
}

// 对应Python: get_server_config（行 120-145）
QJsonObject McpEditDialog::serverConfig() const
{
    QJsonObject config;

    const QString command = m_commandEdit->text().trimmed();
    if (!command.isEmpty())
        config.insert(QStringLiteral("command"), command);

    // 解析 args（逗号分隔）
    const QString argsText = m_argsEdit->text().trimmed();
    if (!argsText.isEmpty()) {
        QJsonArray args;
        const QStringList parts = argsText.split(QLatin1Char(','));
        for (const QString &part : parts) {
            const QString a = part.trimmed();
            if (!a.isEmpty())
                args.append(a);
        }
        config.insert(QStringLiteral("args"), args);
    }

    // 解析 env（KEY=VALUE 每行一个）
    const QString envText = m_envEdit->toPlainText().trimmed();
    if (!envText.isEmpty()) {
        QJsonObject envObj;
        const QStringList lines = envText.split(QLatin1Char('\n'));
        for (const QString &rawLine : lines) {
            const QString line = rawLine.trimmed();
            const int eq = line.indexOf(QLatin1Char('='));
            if (line.isEmpty() || eq < 0)
                continue;
            envObj.insert(line.left(eq).trimmed(),
                          line.mid(eq + 1).trimmed());
        }
        if (!envObj.isEmpty())
            config.insert(QStringLiteral("env"), envObj);
    }

    return config;
}

// 对应Python: set_name_readonly（行 147-149）
void McpEditDialog::setNameReadOnly(bool readOnly)
{
    m_nameEdit->setReadOnly(readOnly);
}

} // namespace cubeshell
