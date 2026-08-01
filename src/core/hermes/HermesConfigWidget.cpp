// HermesConfigWidget.cpp — see HermesConfigWidget.h for the port map.
// 对应Python: core/hermes/config_widget.py

#include "hermes/HermesConfigWidget.h"

#include "hermes/HermesBackend.h"
#include "hermes/HermesHighlighters.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace cubeshell {

namespace {
// reasoning_effort / tool_use_enforcement / terminal.backend 的候选项
const QStringList kReasoningChoices = {
    QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")};
const QStringList kToolEnforceChoices = {
    QStringLiteral("auto"), QStringLiteral("required"), QStringLiteral("none")};
const QStringList kTerminalChoices = {
    QStringLiteral("local"), QStringLiteral("docker"),
    QStringLiteral("ssh"), QStringLiteral("modal")};

// 对应Python: _mono_font（等宽字体，跨平台回退）
QFont monoFont()
{
    QFont font;
    font.setStyleHint(QFont::Monospace);
    font.setFamilies({QStringLiteral("Consolas"), QStringLiteral("Menlo"),
                      QStringLiteral("DejaVu Sans Mono"),
                      QStringLiteral("Courier New"),
                      QStringLiteral("monospace")});
    font.setFixedPitch(true);
    return font;
}

// 计算行首空格缩进（tab 不做特殊处理——hermes 的 config.yaml 用空格缩进）
int indentOf(const QString &line)
{
    int indent = 0;
    while (indent < line.size() && line.at(indent) == QLatin1Char(' '))
        ++indent;
    return indent;
}
} // namespace

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

HermesConfigWidget::HermesConfigWidget(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
}

HermesConfigWidget::~HermesConfigWidget()
{
    for (QFuture<void> &f : m_futures)
        f.waitForFinished();
}

void HermesConfigWidget::schedule(std::function<void()> job)
{
    for (int i = m_futures.size() - 1; i >= 0; --i) {
        if (m_futures.at(i).isFinished())
            m_futures.removeAt(i);
    }
    m_futures.append(QtConcurrent::run(std::move(job)));
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

// 对应Python: _init_ui
void HermesConfigWidget::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(buildFormTab(), tr("常用设置"));
    m_tabs->addTab(buildRawTab(), tr("原始配置"));
    layout->addWidget(m_tabs);
}

// 对应Python: _build_form_tab
QWidget *HermesConfigWidget::buildFormTab()
{
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    // 去掉滚动区自带的边框，避免与 tab / GroupBox 边框叠成多层线框
    scroll->setFrameShape(QFrame::NoFrame);
    auto *container = new QWidget(scroll);
    auto *v = new QVBoxLayout(container);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(12);

    // --- 三个紧凑表单分两列并排：左列 模型配置+Terminal，右列 Agent 行为 ---

    // 模型配置
    auto *modelGroup = new QGroupBox(tr("模型配置"), container);
    auto *modelForm = new QFormLayout(modelGroup);
    modelForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    addLineField(modelForm, QStringLiteral("model.default"),
                 tr("模型 (default):"));
    addLineField(modelForm, QStringLiteral("model.provider"),
                 tr("Provider:"));
    addLineField(modelForm, QStringLiteral("model.base_url"),
                 tr("Base URL:"));

    // Agent 行为
    auto *agentGroup = new QGroupBox(tr("Agent 行为"), container);
    auto *agentForm = new QFormLayout(agentGroup);
    agentForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    addSpinField(agentForm, QStringLiteral("agent.max_turns"),
                 tr("最大轮数:"), 1, 999);
    addComboField(agentForm, QStringLiteral("agent.reasoning_effort"),
                  tr("推理强度:"), kReasoningChoices, true);
    addComboField(agentForm, QStringLiteral("agent.tool_use_enforcement"),
                  tr("工具调用约束:"), kToolEnforceChoices, true);
    addCheckField(agentForm, QStringLiteral("agent.verbose"),
                  tr("详细输出 (verbose):"));
    addCheckField(agentForm, QStringLiteral("agent.environment_probe"),
                  tr("环境探测:"));
    addCheckField(agentForm, QStringLiteral("agent.task_completion_guidance"),
                  tr("任务完成引导:"));

    // Terminal
    auto *termGroup = new QGroupBox(tr("Terminal"), container);
    auto *termForm = new QFormLayout(termGroup);
    termForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    addComboField(termForm, QStringLiteral("terminal.backend"),
                  tr("Backend:"), kTerminalChoices, false);
    addSpinField(termForm, QStringLiteral("terminal.timeout"),
                 tr("超时(秒):"), 0, 36000);
    addLineField(termForm, QStringLiteral("terminal.cwd"), tr("工作目录:"));

    // 统一三个表单的标签列宽度 → 所有输入框/下拉框宽度一致
    normalizeLabelWidths({modelForm, agentForm, termForm});

    // Agent 跨两行：高度自动等于左列(模型+Terminal)之和，底边对齐
    auto *grid = new QGridLayout();
    grid->setSpacing(12);
    grid->addWidget(modelGroup, 0, 0);
    grid->addWidget(termGroup, 1, 0);
    grid->addWidget(agentGroup, 0, 1, 2, 1);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    grid->setRowStretch(1, 1);
    v->addLayout(grid);

    // --- API Keys (.env) ---
    auto *keyGroup = new QGroupBox(tr("API Keys (.env)"), container);
    auto *keyLayout = new QVBoxLayout(keyGroup);
    m_providerTable = new QTableWidget(keyGroup);
    m_providerTable->setColumnCount(3);
    m_providerTable->setMinimumHeight(180);
    m_providerTable->setHorizontalHeaderLabels(
        {tr("环境变量"), tr("值"), tr("操作")});
    m_providerTable->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    m_providerTable->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    m_providerTable->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::ResizeToContents);
    m_providerTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_providerTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_providerTable->verticalHeader()->setVisible(false);
    keyLayout->addWidget(m_providerTable);
    auto *addKeyBtn = new QPushButton(tr("新增环境变量"), keyGroup);
    connect(addKeyBtn, &QPushButton::clicked,
            this, &HermesConfigWidget::onAddEnvKey);
    auto *keyRow = new QHBoxLayout();
    keyRow->addStretch();
    keyRow->addWidget(addKeyBtn);
    keyLayout->addLayout(keyRow);
    // API Keys 占据剩余纵向空间（stretch=1）
    v->addWidget(keyGroup, 1);

    // --- 底部操作按钮 ---
    auto *btnRow = new QHBoxLayout();
    m_checkBtn = new QPushButton(tr("配置检查"), container);
    connect(m_checkBtn, &QPushButton::clicked,
            this, &HermesConfigWidget::onCheck);
    btnRow->addWidget(m_checkBtn);
    btnRow->addStretch();
    m_refreshBtn = new QPushButton(tr("刷新配置"), container);
    connect(m_refreshBtn, &QPushButton::clicked,
            this, [this]() { loadConfig(); });
    btnRow->addWidget(m_refreshBtn);
    m_saveBtn = new QPushButton(tr("保存配置"), container);
    connect(m_saveBtn, &QPushButton::clicked,
            this, &HermesConfigWidget::onSaveForm);
    btnRow->addWidget(m_saveBtn);
    v->addLayout(btnRow);

    scroll->setWidget(container);
    return scroll;
}

// 对应Python: _build_raw_tab
QWidget *HermesConfigWidget::buildRawTab()
{
    auto *w = new QWidget(this);
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(8);

    const QFont mono = monoFont();

    // config.yaml
    v->addWidget(new QLabel(QStringLiteral("config.yaml"), w));
    m_rawConfigEdit = new QPlainTextEdit(w);
    m_rawConfigEdit->setPlaceholderText(tr("config.yaml 内容..."));
    m_rawConfigEdit->setFont(mono);
    m_rawConfigEdit->setTabStopDistance(
        m_rawConfigEdit->fontMetrics().horizontalAdvance(QLatin1Char(' '))
        * 2);
    // 高亮器以 document 为父对象，随编辑器销毁
    m_yamlHighlighter = new YamlHighlighter(m_rawConfigEdit->document());
    v->addWidget(m_rawConfigEdit, 3);
    auto *yamlRow = new QHBoxLayout();
    yamlRow->addStretch();
    auto *saveYamlBtn = new QPushButton(tr("保存 config.yaml"), w);
    connect(saveYamlBtn, &QPushButton::clicked,
            this, &HermesConfigWidget::onSaveRawYaml);
    yamlRow->addWidget(saveYamlBtn);
    v->addLayout(yamlRow);

    // .env
    v->addWidget(new QLabel(QStringLiteral(".env"), w));
    m_rawEnvEdit = new QPlainTextEdit(w);
    m_rawEnvEdit->setPlaceholderText(tr(".env 内容..."));
    m_rawEnvEdit->setFont(mono);
    m_envHighlighter = new DotenvHighlighter(m_rawEnvEdit->document());
    v->addWidget(m_rawEnvEdit, 2);
    auto *envRow = new QHBoxLayout();
    envRow->addStretch();
    auto *saveEnvBtn = new QPushButton(tr("保存 .env"), w);
    connect(saveEnvBtn, &QPushButton::clicked,
            this, &HermesConfigWidget::onSaveRawEnv);
    envRow->addWidget(saveEnvBtn);
    v->addLayout(envRow);

    return w;
}

// ── 表单控件添加助手（登记 key_path 供保存时 diff） ──

// 对应Python: _add_line
void HermesConfigWidget::addLineField(QFormLayout *form,
                                      const QString &keyPath,
                                      const QString &label)
{
    auto *w = new QLineEdit(this);
    w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    form->addRow(label, w);
    FormField field;
    field.path = keyPath;
    field.kind = FormField::Line;
    field.widget = w;
    m_fields.append(field);
}

// 对应Python: _add_spin
void HermesConfigWidget::addSpinField(QFormLayout *form,
                                      const QString &keyPath,
                                      const QString &label, int lo, int hi)
{
    auto *w = new QSpinBox(this);
    w->setRange(lo, hi);
    w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    form->addRow(label, w);
    FormField field;
    field.path = keyPath;
    field.kind = FormField::Spin;
    field.widget = w;
    m_fields.append(field);
}

// 对应Python: _add_combo
void HermesConfigWidget::addComboField(QFormLayout *form,
                                       const QString &keyPath,
                                       const QString &label,
                                       const QStringList &items,
                                       bool editable)
{
    auto *w = new QComboBox(this);
    w->setEditable(editable);
    w->addItems(items);
    w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    form->addRow(label, w);
    FormField field;
    field.path = keyPath;
    field.kind = FormField::Combo;
    field.widget = w;
    m_fields.append(field);
}

// 对应Python: _add_check
void HermesConfigWidget::addCheckField(QFormLayout *form,
                                       const QString &keyPath,
                                       const QString &label)
{
    auto *w = new QCheckBox(this);
    form->addRow(label, w);
    FormField field;
    field.path = keyPath;
    field.kind = FormField::Check;
    field.widget = w;
    m_fields.append(field);
}

// 对应Python: _normalize_label_widths
void HermesConfigWidget::normalizeLabelWidths(
    const QList<QFormLayout *> &forms)
{
    QList<QWidget *> labels;
    for (QFormLayout *form : forms) {
        for (int r = 0; r < form->rowCount(); ++r) {
            QLayoutItem *item = form->itemAt(r, QFormLayout::LabelRole);
            if (item && item->widget())
                labels.append(item->widget());
        }
    }
    if (labels.isEmpty())
        return;
    int maxWidth = 0;
    for (QWidget *label : labels)
        maxWidth = qMax(maxWidth, label->sizeHint().width());
    for (QWidget *label : labels)
        label->setMinimumWidth(maxWidth);
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

void HermesConfigWidget::setBackend(HermesBackend *backend)
{
    m_backend = backend;
}

void HermesConfigWidget::refresh()
{
    loadConfig();
}

// ---------------------------------------------------------------------------
// loading
// ---------------------------------------------------------------------------

// 对应Python: _load_config + ConfigLoader.run
void HermesConfigWidget::loadConfig()
{
    if (!m_backend || m_loading)
        return;
    m_loading = true;
    HermesBackend *backend = m_backend;
    schedule([this, backend]() {
        const QString home = backend->hermesHome();
        const QString configContent =
            backend->readFile(home + QStringLiteral("/config.yaml"));
        const QString envContent =
            backend->readFile(home + QStringLiteral("/.env"));
        QMetaObject::invokeMethod(this, [this, configContent, envContent]() {
            m_loading = false;
            onConfigLoaded(configContent, envContent);
        }, Qt::QueuedConnection);
    });
}

// 对应Python: _on_config_loaded
void HermesConfigWidget::onConfigLoaded(const QString &rawConfig,
                                        const QString &rawEnv)
{
    m_rawConfig = rawConfig;
    m_rawEnv = rawEnv;
    m_envData = parseEnv(rawEnv);
    populateForm();
    populateEnvTable();
    m_rawConfigEdit->setPlainText(rawConfig);
    m_rawEnvEdit->setPlainText(rawEnv);
}

// 对应Python: _populate_form
void HermesConfigWidget::populateForm()
{
    for (FormField &field : m_fields) {
        const QString raw = getNestedValue(m_rawConfig, field.path);
        switch (field.kind) {
        case FormField::Line: {
            auto *w = static_cast<QLineEdit *>(field.widget);
            w->setText(raw);
            field.loadedText = raw;
            break;
        }
        case FormField::Spin: {
            auto *w = static_cast<QSpinBox *>(field.widget);
            bool ok = false;
            int val = raw.toInt(&ok);
            if (!ok)
                val = w->minimum();
            w->setValue(val);
            field.loadedInt = val;
            break;
        }
        case FormField::Combo: {
            auto *w = static_cast<QComboBox *>(field.widget);
            if (w->isEditable()) {
                w->setCurrentText(raw);
            } else {
                const int idx = w->findText(raw);
                if (idx >= 0) {
                    w->setCurrentIndex(idx);
                } else if (!raw.isEmpty()) {
                    w->addItem(raw);
                    w->setCurrentText(raw);
                }
            }
            field.loadedText = w->currentText();
            break;
        }
        case FormField::Check: {
            auto *w = static_cast<QCheckBox *>(field.widget);
            const QString low = raw.toLower();
            const bool val = low == QLatin1String("true")
                || low == QLatin1String("yes")
                || low == QLatin1String("on")
                || low == QLatin1String("1");
            w->setChecked(val);
            field.loadedBool = val;
            break;
        }
        }
    }
}

// 对应Python: _populate_env_table
void HermesConfigWidget::populateEnvTable()
{
    m_providerTable->setRowCount(0);
    if (m_envData.isEmpty()) {
        m_providerTable->setRowCount(1);
        m_providerTable->setItem(0, 0, new QTableWidgetItem(tr("未配置")));
        m_providerTable->setItem(0, 1, new QTableWidgetItem(QString()));
        return;
    }
    const QStringList keys = m_envData.keys(); // QMap 已按键排序
    m_providerTable->setRowCount(keys.size());
    for (int row = 0; row < keys.size(); ++row) {
        const QString &key = keys.at(row);
        const QString value = m_envData.value(key);
        m_providerTable->setItem(row, 0, new QTableWidgetItem(key));
        // 敏感值（含 KEY/SECRET/TOKEN/PASSWORD）遮蔽，其余明文
        const QString display =
            isSecretKey(key) ? maskValue(value) : value;
        m_providerTable->setItem(row, 1, new QTableWidgetItem(display));
        auto *editBtn = new QPushButton(tr("编辑"), m_providerTable);
        connect(editBtn, &QPushButton::clicked,
                this, [this, key]() { editEnvKey(key); });
        m_providerTable->setCellWidget(row, 2, editBtn);
    }
}

// ---------------------------------------------------------------------------
// .env editing
// ---------------------------------------------------------------------------

// 对应Python: _on_add_env_key
void HermesConfigWidget::onAddEnvKey()
{
    bool ok = false;
    QString key = QInputDialog::getText(
        this, tr("新增环境变量"), tr("变量名 (如 OPENAI_API_KEY):"),
        QLineEdit::Normal, QString(), &ok);
    key = key.trimmed();
    if (!ok || key.isEmpty())
        return;
    const QString value = QInputDialog::getText(
        this, tr("新增环境变量"), tr("%1 的值:").arg(key),
        QLineEdit::Normal, QString(), &ok);
    if (!ok)
        return;
    m_envData.insert(key, value.trimmed());
    populateEnvTable();
}

// 对应Python: _on_edit_env_key
void HermesConfigWidget::editEnvKey(const QString &key)
{
    bool ok = false;
    const QString newValue = QInputDialog::getText(
        this, tr("编辑环境变量"), tr("%1 的值:").arg(key),
        QLineEdit::Normal, m_envData.value(key), &ok);
    if (ok) {
        m_envData.insert(key, newValue.trimmed());
        populateEnvTable();
    }
}

// ---------------------------------------------------------------------------
// save (form tab)
// ---------------------------------------------------------------------------

// 对应Python: _on_save_form + SaveWorker.run
void HermesConfigWidget::onSaveForm()
{
    if (!m_backend) {
        QMessageBox::warning(this, tr("警告"), tr("未连接后端"));
        return;
    }
    if (m_saving)
        return;

    const QList<QPair<QString, QString>> scalarChanges =
        collectScalarChanges();
    const QMap<QString, QString> envChanges = collectEnvChanges();

    if (scalarChanges.isEmpty() && envChanges.isEmpty()) {
        QMessageBox::information(this, tr("提示"), tr("没有需要保存的改动"));
        return;
    }

    m_saving = true;
    m_saveBtn->setEnabled(false);
    HermesBackend *backend = m_backend;
    const QString rawEnv = m_rawEnv;
    schedule([this, backend, scalarChanges, envChanges, rawEnv]() {
        QStringList okKeys;
        QStringList failedKeys;
        for (const auto &change : scalarChanges) {
            const QString output = backend->execCli(
                {QStringLiteral("config"), QStringLiteral("set"),
                 change.first, change.second});
            // hermes 成功时输出包含 "✓ Set"，失败/异常则无
            if (output.contains(QLatin1String("Set"))
                && output.contains(QStringLiteral("✓")))
                okKeys.append(change.first);
            else
                failedKeys.append(change.first);
        }

        bool envSaved = false;
        if (!envChanges.isEmpty()) {
            const QString newContent = updateEnvContent(rawEnv, envChanges);
            backend->writeFile(
                backend->hermesHome() + QStringLiteral("/.env"), newContent);
            envSaved = true;
        }

        QMetaObject::invokeMethod(this, [this, okKeys, failedKeys,
                                         envSaved]() {
            onSaveFinished(okKeys, failedKeys, envSaved);
        }, Qt::QueuedConnection);
    });
}

// 对应Python: _collect_scalar_changes
QList<QPair<QString, QString>> HermesConfigWidget::collectScalarChanges() const
{
    QList<QPair<QString, QString>> changes;
    for (const FormField &field : m_fields) {
        switch (field.kind) {
        case FormField::Line: {
            const QString cur =
                static_cast<QLineEdit *>(field.widget)->text().trimmed();
            if (cur != field.loadedText && !cur.isEmpty())
                changes.append({field.path, cur});
            break;
        }
        case FormField::Spin: {
            const int cur = static_cast<QSpinBox *>(field.widget)->value();
            if (cur != field.loadedInt)
                changes.append({field.path, QString::number(cur)});
            break;
        }
        case FormField::Combo: {
            const QString cur = static_cast<QComboBox *>(field.widget)
                                    ->currentText().trimmed();
            if (cur != field.loadedText && !cur.isEmpty())
                changes.append({field.path, cur});
            break;
        }
        case FormField::Check: {
            const bool cur =
                static_cast<QCheckBox *>(field.widget)->isChecked();
            if (cur != field.loadedBool)
                changes.append({field.path,
                                cur ? QStringLiteral("true")
                                    : QStringLiteral("false")});
            break;
        }
        }
    }
    return changes;
}

// 对应Python: _collect_env_changes
QMap<QString, QString> HermesConfigWidget::collectEnvChanges() const
{
    const QMap<QString, QString> original = parseEnv(m_rawEnv);
    QMap<QString, QString> changes;
    for (auto it = m_envData.constBegin(); it != m_envData.constEnd(); ++it) {
        if (original.value(it.key()) != it.value()
            || !original.contains(it.key()))
            changes.insert(it.key(), it.value());
    }
    return changes;
}

// 对应Python: _on_save_finished
void HermesConfigWidget::onSaveFinished(const QStringList &okKeys,
                                        const QStringList &failedKeys,
                                        bool envSaved)
{
    m_saving = false;
    m_saveBtn->setEnabled(true);
    QStringList parts;
    if (!okKeys.isEmpty())
        parts.append(tr("已保存 %1 项配置").arg(okKeys.size()));
    if (envSaved)
        parts.append(tr(".env 已更新"));
    if (!failedKeys.isEmpty()) {
        parts.append(tr("以下项保存失败，请改用「原始配置」手动编辑：\n%1")
                         .arg(failedKeys.join(QLatin1Char('\n'))));
        QMessageBox::warning(this, tr("部分失败"),
                             parts.join(QLatin1Char('\n')));
    } else {
        const QString message = parts.join(QLatin1Char('\n'));
        QMessageBox::information(
            this, tr("成功"),
            message.isEmpty() ? tr("配置已保存") : message);
    }
    // 重新加载以刷新 loaded 基准值和原始文本
    loadConfig();
}

// ---------------------------------------------------------------------------
// save (raw tab)
// ---------------------------------------------------------------------------

// 对应Python: _on_save_raw_yaml
void HermesConfigWidget::onSaveRawYaml()
{
    if (!m_backend) {
        QMessageBox::warning(this, tr("警告"), tr("未连接后端"));
        return;
    }
    const QString content = m_rawConfigEdit->toPlainText();
    // 语法校验：校验失败则拒绝写入
    QString error;
    if (!validateYamlBasic(content, &error)) {
        QMessageBox::critical(this, tr("YAML 语法错误"),
                              tr("无法保存，请修正后重试：\n%1").arg(error));
        return;
    }
    HermesBackend *backend = m_backend;
    schedule([this, backend, content]() {
        const bool ok = backend->writeFile(
            backend->hermesHome() + QStringLiteral("/config.yaml"), content);
        QMetaObject::invokeMethod(this, [this, ok]() {
            if (ok) {
                QMessageBox::information(this, tr("成功"),
                                         tr("config.yaml 已保存"));
                loadConfig();
            } else {
                QMessageBox::critical(this, tr("错误"),
                                      tr("保存失败: 写入 config.yaml 失败"));
            }
        }, Qt::QueuedConnection);
    });
}

// 对应Python: _on_save_raw_env
void HermesConfigWidget::onSaveRawEnv()
{
    if (!m_backend) {
        QMessageBox::warning(this, tr("警告"), tr("未连接后端"));
        return;
    }
    const QString content = m_rawEnvEdit->toPlainText();
    HermesBackend *backend = m_backend;
    schedule([this, backend, content]() {
        const bool ok = backend->writeFile(
            backend->hermesHome() + QStringLiteral("/.env"), content);
        QMetaObject::invokeMethod(this, [this, ok]() {
            if (ok) {
                QMessageBox::information(this, tr("成功"),
                                         tr(".env 已保存"));
                loadConfig();
            } else {
                QMessageBox::critical(this, tr("错误"),
                                      tr("保存失败: 写入 .env 失败"));
            }
        }, Qt::QueuedConnection);
    });
}

// ---------------------------------------------------------------------------
// config check
// ---------------------------------------------------------------------------

// 对应Python: _on_check + CheckWorker.run
void HermesConfigWidget::onCheck()
{
    if (!m_backend) {
        QMessageBox::warning(this, tr("警告"), tr("未连接后端"));
        return;
    }
    if (m_checking)
        return;
    m_checking = true;
    m_checkBtn->setEnabled(false);
    HermesBackend *backend = m_backend;
    schedule([this, backend]() {
        QString output = backend->execCli(
            {QStringLiteral("config"), QStringLiteral("check")}).trimmed();
        if (output.isEmpty())
            output = tr("(无输出)");
        QMetaObject::invokeMethod(this, [this, output]() {
            m_checking = false;
            m_checkBtn->setEnabled(true);
            showCheckResult(output);
        }, Qt::QueuedConnection);
    });
}

// 对应Python: _on_check_finished
void HermesConfigWidget::showCheckResult(const QString &output)
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("配置检查 (hermes config check)"));
    dlg.resize(560, 480);
    auto *v = new QVBoxLayout(&dlg);
    auto *text = new QPlainTextEdit(&dlg);
    text->setReadOnly(true);
    text->setPlainText(output);
    v->addWidget(text);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok, &dlg);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    v->addWidget(box);
    dlg.exec();
}

// ---------------------------------------------------------------------------
// pure helpers
// ---------------------------------------------------------------------------

// 文本级 YAML 点号路径取值：逐段沿缩进下钻。只覆盖 hermes config.yaml 的
// 浅层 key: value 结构（表单字段全部满足），不处理列表/多行标量/流式语法。
// 对应Python: _get_nested（yaml.safe_load 后的 dict 逐层取值）
QString HermesConfigWidget::getNestedValue(const QString &configText,
                                           const QString &dottedPath)
{
    const QStringList parts = dottedPath.split(QLatin1Char('.'));
    const QStringList lines = configText.split(QLatin1Char('\n'));
    int lineIdx = 0;
    int parentIndent = -1;
    for (int p = 0; p < parts.size(); ++p) {
        const QString &part = parts.at(p);
        bool found = false;
        for (; lineIdx < lines.size(); ++lineIdx) {
            const QString &raw = lines.at(lineIdx);
            const QString trimmed = raw.trimmed();
            if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')))
                continue;
            const int indent = indentOf(raw);
            // 缩进回落到父级或更浅 → 已离开父块，目标键不存在
            if (indent <= parentIndent)
                return QString();
            if (trimmed.startsWith(part + QLatin1Char(':'))) {
                if (p == parts.size() - 1) {
                    QString value = trimmed.mid(part.size() + 1).trimmed();
                    // 去除未加引号值的行内注释
                    if (!value.startsWith(QLatin1Char('"'))
                        && !value.startsWith(QLatin1Char('\''))) {
                        const int hash =
                            value.indexOf(QLatin1String(" #"));
                        if (hash >= 0)
                            value = value.left(hash).trimmed();
                        if (value.startsWith(QLatin1Char('#')))
                            value.clear();
                    }
                    // 去除成对的首尾引号
                    if (value.size() >= 2
                        && ((value.startsWith(QLatin1Char('"'))
                             && value.endsWith(QLatin1Char('"')))
                            || (value.startsWith(QLatin1Char('\''))
                                && value.endsWith(QLatin1Char('\''))))) {
                        value = value.mid(1, value.size() - 2);
                    }
                    return value;
                }
                // 中间段：进入子块继续找下一段
                parentIndent = indent;
                ++lineIdx;
                found = true;
                break;
            }
        }
        if (!found)
            return QString();
    }
    return QString();
}

// 对应Python: ConfigLoader._parse_env
QMap<QString, QString> HermesConfigWidget::parseEnv(const QString &content)
{
    QMap<QString, QString> env;
    const QStringList lines = content.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq > 0)
            env.insert(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
    }
    return env;
}

// 对应Python: _update_env_content
QString HermesConfigWidget::updateEnvContent(
    const QString &raw, const QMap<QString, QString> &changes)
{
    if (changes.isEmpty())
        return raw;
    QMap<QString, QString> remaining = changes;
    QStringList outLines;
    const QStringList lines = raw.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString stripped = line.trimmed();
        bool matched = false;
        if (!stripped.isEmpty() && !stripped.startsWith(QLatin1Char('#'))) {
            const int eq = stripped.indexOf(QLatin1Char('='));
            if (eq > 0) {
                const QString key = stripped.left(eq).trimmed();
                if (remaining.contains(key)) {
                    outLines.append(key + QLatin1Char('=')
                                    + remaining.take(key));
                    matched = true;
                }
            }
        }
        if (!matched)
            outLines.append(line);
    }
    // split('\n') 会因末尾换行产生一个空尾元素，去掉避免重复空行累积
    if (!outLines.isEmpty() && outLines.last().isEmpty())
        outLines.removeLast();
    // 追加新键
    for (auto it = remaining.constBegin(); it != remaining.constEnd(); ++it)
        outLines.append(it.key() + QLatin1Char('=') + it.value());
    QString content = outLines.join(QLatin1Char('\n'));
    if (!content.endsWith(QLatin1Char('\n')))
        content += QLatin1Char('\n');
    return content;
}

// 对应Python: _mask_key
QString HermesConfigWidget::maskValue(const QString &value)
{
    if (value.isEmpty())
        return QString();
    if (value.size() <= 8) {
        if (value.size() > 4)
            return value.left(2) + QStringLiteral("****") + value.right(2);
        return QStringLiteral("****");
    }
    return value.left(4) + QStringLiteral("****") + value.right(4);
}

// 对应Python: _is_secret
bool HermesConfigWidget::isSecretKey(const QString &key)
{
    static const QRegularExpression re(
        QStringLiteral("KEY|SECRET|TOKEN|PASSWORD"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(key).hasMatch();
}

// 基础 YAML 校验（启发式，非完整解析）：
//   * 每行引号必须配对；
//   * 非注释/非列表项的内容行必须是 "key: ..." 形式（含冒号）。
// 对应Python: yaml.safe_load 校验（这里避免引入 YAML 解析依赖）
bool HermesConfigWidget::validateYamlBasic(const QString &content,
                                           QString *error)
{
    const QStringList lines = content.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString trimmed = lines.at(i).trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')))
            continue;

        // 引号配对检查（忽略注释后的内容需要引号状态，因此先数引号）
        int doubleQuotes = 0;
        int singleQuotes = 0;
        bool inDouble = false;
        bool inSingle = false;
        for (int c = 0; c < trimmed.size(); ++c) {
            const QChar ch = trimmed.at(c);
            if (ch == QLatin1Char('\\')) {
                ++c; // 跳过转义字符
                continue;
            }
            if (ch == QLatin1Char('"') && !inSingle) {
                ++doubleQuotes;
                inDouble = !inDouble;
            } else if (ch == QLatin1Char('\'') && !inDouble) {
                ++singleQuotes;
                inSingle = !inSingle;
            } else if (ch == QLatin1Char('#') && !inDouble && !inSingle) {
                break; // 行内注释开始，其后不计
            }
        }
        if (doubleQuotes % 2 != 0 || singleQuotes % 2 != 0) {
            if (error)
                *error = tr("第 %1 行：引号未配对").arg(i + 1);
            return false;
        }

        // 列表项 / 文档分隔符不强制要求冒号
        if (trimmed.startsWith(QLatin1Char('-'))
            || trimmed == QLatin1String("---")
            || trimmed == QLatin1String("..."))
            continue;
        // 内容行须形如 "key: value" 或 "key:"（冒号在注释之前）
        int colon = -1;
        bool quoted = false;
        for (int c = 0; c < trimmed.size(); ++c) {
            const QChar ch = trimmed.at(c);
            if (ch == QLatin1Char('"') || ch == QLatin1Char('\''))
                quoted = !quoted;
            else if (ch == QLatin1Char('#') && !quoted)
                break;
            else if (ch == QLatin1Char(':') && !quoted) {
                colon = c;
                break;
            }
        }
        if (colon <= 0) {
            if (error)
                *error = tr("第 %1 行：缺少键名后的冒号").arg(i + 1);
            return false;
        }
    }
    return true;
}

} // namespace cubeshell
