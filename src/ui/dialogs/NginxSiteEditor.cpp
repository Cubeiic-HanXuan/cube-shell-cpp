// NginxSiteEditor.cpp — see NginxSiteEditor.h.

#include "NginxSiteEditor.h"

#include "editors/CodeEditor.h"
#include "nginx/NginxConfigParser.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace cubeshell {

namespace {

QStringList splitTokens(const QString &text)
{
    return text.split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts);
}

QString locationKindLabel(const QString &kind)
{
    if (kind == QLatin1String("proxy"))
        return QObject::tr("反向代理");
    if (kind == QLatin1String("static"))
        return QObject::tr("静态目录");
    if (kind == QLatin1String("spa"))
        return QObject::tr("SPA 单页");
    if (kind == QLatin1String("redirect"))
        return QObject::tr("重定向");
    return QObject::tr("原始");
}

QString locationTargetText(const nginx::LocationSpec &s)
{
    if (s.kind == QLatin1String("proxy"))
        return s.proxyPass;
    if (s.kind == QLatin1String("static"))
        return !s.alias.isEmpty() ? (QStringLiteral("alias ") + s.alias) : s.root;
    if (s.kind == QLatin1String("spa"))
        return s.tryFiles;
    if (s.kind == QLatin1String("redirect"))
        return s.returnTarget;
    return s.extra.split(QLatin1Char('\n')).value(0).trimmed();
}

// 判断一个 server 块是否为「80 → 443 纯跳转」服务器（本工具 forceHttps 生成）。
bool isHttpsRedirectServer(const nginx::Directive &s)
{
    bool has80 = false;
    bool hasReturn = false;
    bool hasLocation = false;
    for (const nginx::Directive &c : s.children) {
        if (c.name == QLatin1String("listen")) {
            const QList<int> ports = nginx::NginxConfigParser::listenPortsOf(c);
            if (ports.contains(80) && !nginx::NginxConfigParser::listenHasFlag(c, QStringLiteral("ssl")))
                has80 = true;
        } else if (c.name == QLatin1String("return")) {
            hasReturn = true;
        } else if (c.name == QLatin1String("location")) {
            hasLocation = true;
        }
    }
    return has80 && hasReturn && !hasLocation;
}

// 表单里所有字段（输入框 / 下拉框 / 数字框）统一为「水平方向尽量拉伸」策略，
// 使同一张表单里各类控件宽度一致、观感整齐（QComboBox/QSpinBox/QCheckBox 默认
// 的水平策略各不相同，不统一就会有的宽有的窄）。
void makeFieldsConsistent(QFormLayout *form, int labelMinWidth = 0)
{
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    for (int row = 0; row < form->rowCount(); ++row) {
        const QFormLayout::ItemRole roles[] = {QFormLayout::FieldRole,
                                               QFormLayout::SpanningRole};
        for (QFormLayout::ItemRole role : roles) {
            if (QLayoutItem *item = form->itemAt(row, role)) {
                if (QWidget *w = item->widget()) {
                    QSizePolicy sp = w->sizePolicy();
                    sp.setHorizontalPolicy(QSizePolicy::Expanding);
                    w->setSizePolicy(sp);
                }
            }
        }
        if (labelMinWidth > 0) {
            if (QLayoutItem *lbl = form->itemAt(row, QFormLayout::LabelRole)) {
                if (QWidget *w = lbl->widget())
                    w->setMinimumWidth(labelMinWidth);
            }
        }
    }
}

// 模态编辑一条 location；返回 true 表示用户确认。
bool editLocationSpec(QWidget *parent, nginx::LocationSpec *spec, bool isNew)
{
    if (!spec)
        return false;

    QDialog dlg(parent);
    dlg.setWindowTitle(isNew ? QObject::tr("新增 Location") : QObject::tr("编辑 Location"));
    auto *form = new QFormLayout(&dlg);

    auto *modCombo = new QComboBox(&dlg);
    modCombo->addItem(QObject::tr("前缀匹配（默认）"), QString());
    modCombo->addItem(QObject::tr("精确 ="), QStringLiteral("="));
    modCombo->addItem(QObject::tr("区分大小写正则 ~"), QStringLiteral("~"));
    modCombo->addItem(QObject::tr("忽略大小写正则 ~*"), QStringLiteral("~*"));
    modCombo->addItem(QObject::tr("前缀优先 ^~"), QStringLiteral("^~"));

    auto *patternEdit = new QLineEdit(spec->pattern, &dlg);
    patternEdit->setPlaceholderText(QStringLiteral("/api/"));

    auto *kindCombo = new QComboBox(&dlg);
    kindCombo->addItem(QObject::tr("反向代理"), QStringLiteral("proxy"));
    kindCombo->addItem(QObject::tr("静态目录"), QStringLiteral("static"));
    kindCombo->addItem(QObject::tr("SPA 单页回退"), QStringLiteral("spa"));
    kindCombo->addItem(QObject::tr("重定向/返回"), QStringLiteral("redirect"));
    kindCombo->addItem(QObject::tr("原始指令"), QStringLiteral("raw"));

    auto *proxyEdit = new QLineEdit(spec->proxyPass, &dlg);
    proxyEdit->setPlaceholderText(QStringLiteral("http://127.0.0.1:8080"));
    auto *rootEdit = new QLineEdit(spec->root, &dlg);
    auto *aliasEdit = new QLineEdit(spec->alias, &dlg);
    auto *tryFilesEdit = new QLineEdit(spec->tryFiles, &dlg);
    tryFilesEdit->setPlaceholderText(QStringLiteral("$uri $uri/ /index.html"));
    auto *returnEdit = new QLineEdit(spec->returnTarget, &dlg);
    returnEdit->setPlaceholderText(QStringLiteral("301 https://$host$request_uri"));
    auto *extraEdit = new QPlainTextEdit(spec->extra, &dlg);

    form->addRow(QObject::tr("匹配方式"), modCombo);
    form->addRow(QObject::tr("匹配路径/模式"), patternEdit);
    form->addRow(QObject::tr("类型"), kindCombo);
    form->addRow(QObject::tr("proxy_pass"), proxyEdit);
    form->addRow(QObject::tr("root"), rootEdit);
    form->addRow(QObject::tr("alias"), aliasEdit);
    form->addRow(QObject::tr("try_files"), tryFilesEdit);
    form->addRow(QObject::tr("return"), returnEdit);
    form->addRow(QObject::tr("附加指令"), extraEdit);

    auto enableFor = [&](const QString &kind) {
        proxyEdit->setEnabled(kind == QLatin1String("proxy"));
        rootEdit->setEnabled(kind == QLatin1String("static") || kind == QLatin1String("spa"));
        aliasEdit->setEnabled(kind == QLatin1String("static"));
        tryFilesEdit->setEnabled(kind == QLatin1String("spa"));
        returnEdit->setEnabled(kind == QLatin1String("redirect"));
    };
    QObject::connect(kindCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg,
                     [&enableFor, kindCombo](int) { enableFor(kindCombo->currentData().toString()); });

    int mi = modCombo->findData(spec->modifier);
    modCombo->setCurrentIndex(mi < 0 ? 0 : mi);
    int ki = kindCombo->findData(spec->kind.isEmpty() ? QStringLiteral("proxy") : spec->kind);
    kindCombo->setCurrentIndex(ki < 0 ? 0 : ki);
    enableFor(kindCombo->currentData().toString());

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(bb);
    makeFieldsConsistent(form, 100);
    dlg.setMinimumWidth(480);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted)
        return false;

    spec->modifier = modCombo->currentData().toString();
    spec->pattern = patternEdit->text().trimmed();
    spec->kind = kindCombo->currentData().toString();
    spec->proxyPass = proxyEdit->text().trimmed();
    spec->root = rootEdit->text().trimmed();
    spec->alias = aliasEdit->text().trimmed();
    spec->tryFiles = tryFilesEdit->text().trimmed();
    spec->returnTarget = returnEdit->text().trimmed();
    spec->extra = extraEdit->toPlainText().trimmed();
    return true;
}

} // namespace

NginxSiteEditor::NginxSiteEditor(QWidget *parent)
    : QDialog(parent)
{
    setModal(false);
    resize(860, 680);
    buildUi();
}

void NginxSiteEditor::buildUi()
{
    auto *root = new QVBoxLayout(this);

    m_modeTabs = new QTabWidget(this);
    m_modeTabs->addTab(buildFormPage(), tr("表单模式"));

    m_textEditor = new CodeEditor(this);
    m_modeTabs->addTab(m_textEditor, tr("配置文本"));

    root->addWidget(m_modeTabs, 1);

    auto *bb = new QDialogButtonBox(this);
    auto *saveBtn = bb->addButton(tr("保存"), QDialogButtonBox::AcceptRole);
    auto *closeBtn = bb->addButton(tr("关闭"), QDialogButtonBox::RejectRole);
    root->addWidget(bb);

    connect(saveBtn, &QPushButton::clicked, this, &NginxSiteEditor::onSaveClicked);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
    connect(m_modeTabs, &QTabWidget::currentChanged, this, &NginxSiteEditor::onModeChanged);
}

QWidget *NginxSiteEditor::buildFormPage()
{
    auto *page = new QWidget(this);
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);

    auto *split = new QSplitter(Qt::Vertical, page);
    m_fieldTabs = new QTabWidget(split);
    m_fieldTabs->addTab(buildBaseTab(), tr("基本"));
    m_fieldTabs->addTab(buildStaticTab(), tr("静态前端"));
    m_fieldTabs->addTab(buildProxyTab(), tr("反向代理"));
    m_fieldTabs->addTab(buildSslTab(), tr("HTTPS"));
    m_fieldTabs->addTab(buildLocationTab(), tr("Location 规则"));
    m_fieldTabs->addTab(buildAdvancedTab(), tr("高级"));
    split->addWidget(m_fieldTabs);

    auto *prevWrap = new QWidget(split);
    auto *pv = new QVBoxLayout(prevWrap);
    pv->setContentsMargins(0, 0, 0, 0);
    pv->addWidget(new QLabel(tr("配置预览（随表单实时生成）"), prevWrap));
    m_preview = new CodeEditor(prevWrap);
    m_preview->setReadOnly(true);
    pv->addWidget(m_preview, 1);
    split->addWidget(prevWrap);

    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    v->addWidget(split, 1);
    return page;
}

QWidget *NginxSiteEditor::buildBaseTab()
{
    auto *w = new QWidget(this);
    auto *f = new QFormLayout(w);
    m_nameEdit = new QLineEdit(w);
    m_nameEdit->setPlaceholderText(QStringLiteral("app.conf"));
    m_dirLabel = new QLabel(w);
    m_dirLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_portSpin = new QSpinBox(w);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(80);
    m_namesEdit = new QLineEdit(w);
    m_namesEdit->setPlaceholderText(QStringLiteral("example.com www.example.com"));
    m_defaultChk = new QCheckBox(tr("设为默认站点（default_server）"), w);
    m_bodySizeEdit = new QLineEdit(w);
    m_bodySizeEdit->setPlaceholderText(tr("如 50m（留空为默认）"));

    f->addRow(tr("文件名"), m_nameEdit);
    f->addRow(tr("目录"), m_dirLabel);
    f->addRow(tr("监听端口"), m_portSpin);
    f->addRow(tr("域名 server_name"), m_namesEdit);
    f->addRow(QString(), m_defaultChk);
    f->addRow(tr("请求体上限"), m_bodySizeEdit);

    connect(m_nameEdit, &QLineEdit::textEdited, this, &NginxSiteEditor::onFieldChanged);
    connect(m_namesEdit, &QLineEdit::textEdited, this, &NginxSiteEditor::onFieldChanged);
    connect(m_bodySizeEdit, &QLineEdit::textEdited, this, &NginxSiteEditor::onFieldChanged);
    connect(m_portSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &NginxSiteEditor::onFieldChanged);
    connect(m_defaultChk, &QCheckBox::toggled, this, &NginxSiteEditor::onFieldChanged);
    makeFieldsConsistent(f, 110);
    return w;
}

QWidget *NginxSiteEditor::buildStaticTab()
{
    auto *w = new QWidget(this);
    auto *f = new QFormLayout(w);
    m_rootEdit = new QLineEdit(w);
    m_rootEdit->setPlaceholderText(QStringLiteral("/var/www/myapp/dist"));
    m_indexEdit = new QLineEdit(w);
    m_indexEdit->setPlaceholderText(QStringLiteral("index.html"));
    m_spaChk = new QCheckBox(tr("单页应用回退（try_files $uri $uri/ /index.html）"), w);

    f->addRow(tr("站点根目录 root"), m_rootEdit);
    f->addRow(tr("首页文件 index"), m_indexEdit);
    f->addRow(QString(), m_spaChk);

    connect(m_rootEdit, &QLineEdit::textEdited, this, &NginxSiteEditor::onFieldChanged);
    connect(m_indexEdit, &QLineEdit::textEdited, this, &NginxSiteEditor::onFieldChanged);
    connect(m_spaChk, &QCheckBox::toggled, this, &NginxSiteEditor::onFieldChanged);
    makeFieldsConsistent(f, 110);
    return w;
}

QWidget *NginxSiteEditor::buildProxyTab()
{
    auto *w = new QWidget(this);
    auto *f = new QFormLayout(w);
    m_proxyAllChk = new QCheckBox(tr("整站反向代理（location / → proxy_pass）"), w);
    m_proxyPassEdit = new QLineEdit(w);
    m_proxyPassEdit->setPlaceholderText(QStringLiteral("http://127.0.0.1:8080"));
    m_wsChk = new QCheckBox(tr("透传 WebSocket（Upgrade/Connection 头）"), w);
    m_wsChk->setChecked(true);

    f->addRow(QString(), m_proxyAllChk);
    f->addRow(tr("后端地址 proxy_pass"), m_proxyPassEdit);
    f->addRow(QString(), m_wsChk);

    connect(m_proxyAllChk, &QCheckBox::toggled, this, &NginxSiteEditor::onFieldChanged);
    connect(m_proxyPassEdit, &QLineEdit::textEdited, this, &NginxSiteEditor::onFieldChanged);
    connect(m_wsChk, &QCheckBox::toggled, this, &NginxSiteEditor::onFieldChanged);
    makeFieldsConsistent(f, 110);
    return w;
}

QWidget *NginxSiteEditor::buildSslTab()
{
    auto *w = new QWidget(this);
    auto *f = new QFormLayout(w);
    m_sslChk = new QCheckBox(tr("启用 HTTPS（listen ... ssl）"), w);
    m_certEdit = new QLineEdit(w);
    m_certEdit->setPlaceholderText(QStringLiteral("/etc/nginx/ssl/example.com.crt"));
    m_keyEdit = new QLineEdit(w);
    m_keyEdit->setPlaceholderText(QStringLiteral("/etc/nginx/ssl/example.com.key"));
    m_forceHttpsChk = new QCheckBox(tr("80 端口强制跳转到 HTTPS"), w);

    f->addRow(QString(), m_sslChk);
    f->addRow(tr("证书 ssl_certificate"), m_certEdit);
    f->addRow(tr("私钥 ssl_certificate_key"), m_keyEdit);
    f->addRow(QString(), m_forceHttpsChk);

    connect(m_sslChk, &QCheckBox::toggled, this, &NginxSiteEditor::onFieldChanged);
    connect(m_certEdit, &QLineEdit::textEdited, this, &NginxSiteEditor::onFieldChanged);
    connect(m_keyEdit, &QLineEdit::textEdited, this, &NginxSiteEditor::onFieldChanged);
    connect(m_forceHttpsChk, &QCheckBox::toggled, this, &NginxSiteEditor::onFieldChanged);
    makeFieldsConsistent(f, 110);
    return w;
}

QWidget *NginxSiteEditor::buildLocationTab()
{
    auto *w = new QWidget(this);
    auto *v = new QVBoxLayout(w);
    v->addWidget(new QLabel(tr("除主 location / 之外的额外 location 规则："), w));

    m_locTable = new QTableWidget(w);
    m_locTable->setColumnCount(4);
    m_locTable->setHorizontalHeaderLabels(
        {tr("匹配方式"), tr("路径 / 模式"), tr("类型"), tr("目标")});
    m_locTable->horizontalHeader()->setStretchLastSection(true);
    m_locTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_locTable->verticalHeader()->setVisible(false);
    m_locTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_locTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_locTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    v->addWidget(m_locTable, 1);

    auto *row = new QHBoxLayout();
    auto *addBtn = new QPushButton(tr("添加"), w);
    auto *editBtn = new QPushButton(tr("编辑"), w);
    auto *delBtn = new QPushButton(tr("删除"), w);
    row->addWidget(addBtn);
    row->addWidget(editBtn);
    row->addWidget(delBtn);
    row->addStretch(1);
    v->addLayout(row);

    connect(addBtn, &QPushButton::clicked, this, &NginxSiteEditor::onAddLocation);
    connect(editBtn, &QPushButton::clicked, this, &NginxSiteEditor::onEditLocation);
    connect(delBtn, &QPushButton::clicked, this, &NginxSiteEditor::onRemoveLocation);
    connect(m_locTable, &QTableWidget::doubleClicked, this,
            [this](const QModelIndex &) { onEditLocation(); });
    return w;
}

QWidget *NginxSiteEditor::buildAdvancedTab()
{
    auto *w = new QWidget(this);
    auto *v = new QVBoxLayout(w);
    v->addWidget(new QLabel(tr("server 级追加指令（每行一条，如 error_page / client_body_timeout）："), w));
    m_extraEdit = new QPlainTextEdit(w);
    m_extraEdit->setPlaceholderText(QStringLiteral("client_body_timeout 60s;\nkeepalive_timeout 65;"));
    v->addWidget(m_extraEdit, 1);
    connect(m_extraEdit, &QPlainTextEdit::textChanged, this, &NginxSiteEditor::onFieldChanged);
    return w;
}

// ---------------------------------------------------------------------------
// 载入 / 收集
// ---------------------------------------------------------------------------

void NginxSiteEditor::setSite(const QString &path, const QString &currentText, bool isNew)
{
    m_switching = true;
    m_path = path;
    m_isNew = isNew;
    m_originalText = currentText;
    m_fileName = QFileInfo(path).fileName();
    m_formDirty = false;
    m_textInitialized = false;
    m_parseFailed = false;
    m_spec = nginx::SiteSpec();
    m_spec.fileName = m_fileName;
    m_locations.clear();

    if (!currentText.trimmed().isEmpty()) {
        QList<nginx::Directive> top;
        QString err;
        if (nginx::NginxConfigParser::parse(currentText, &top, &err)) {
            const QList<nginx::Directive> servers =
                nginx::NginxConfigParser::extractServerDirectives(top);
            const nginx::Directive *main = nullptr;
            bool forceHttps = false;
            for (const nginx::Directive &s : servers) {
                if (isHttpsRedirectServer(s)) {
                    forceHttps = true;
                    continue;
                }
                if (!main)
                    main = &s;
            }
            if (!main && !servers.isEmpty())
                main = &servers.first();
            if (main)
                nginx::NginxConfigParser::fillSpecFromServer(*main, &m_spec);
            m_spec.fileName = m_fileName;
            m_spec.forceHttps = forceHttps;
            m_locations = m_spec.locations;
        } else {
            m_parseFailed = true;
        }
    } else {
        m_spec.listenPort = 80;
        m_spec.indexFiles = QStringList{QStringLiteral("index.html")};
        m_spec.proxyWebSocket = true;
    }

    loadSpecIntoForm();
    rebuildLocationTable();
    refreshPreview();

    setWindowTitle(isNew ? tr("新建站点") : tr("编辑站点 - %1").arg(m_fileName));

    if (m_parseFailed) {
        m_textEditor->setPlainText(currentText);
        m_textInitialized = true;
        m_modeTabs->setCurrentIndex(1);
    } else {
        m_modeTabs->setCurrentIndex(0);
    }
    m_switching = false;
}

void NginxSiteEditor::loadSpecIntoForm()
{
    m_nameEdit->setText(m_spec.fileName.isEmpty() ? m_fileName : m_spec.fileName);
    m_nameEdit->setReadOnly(!m_isNew);
    m_dirLabel->setText(QFileInfo(m_path).absolutePath());
    m_portSpin->setValue(m_spec.listenPort > 0 ? m_spec.listenPort : 80);
    m_namesEdit->setText(m_spec.serverNames.join(QLatin1Char(' ')));
    m_defaultChk->setChecked(m_spec.defaultServer);
    m_bodySizeEdit->setText(m_spec.clientMaxBodySize);
    m_rootEdit->setText(m_spec.root);
    m_indexEdit->setText(m_spec.indexFiles.join(QLatin1Char(' ')));
    m_spaChk->setChecked(m_spec.spaFallback);
    m_proxyAllChk->setChecked(m_spec.proxyAll);
    m_proxyPassEdit->setText(m_spec.proxyPass);
    m_wsChk->setChecked(m_spec.proxyWebSocket);
    m_sslChk->setChecked(m_spec.listenSsl);
    m_certEdit->setText(m_spec.sslCertificate);
    m_keyEdit->setText(m_spec.sslCertificateKey);
    m_forceHttpsChk->setChecked(m_spec.forceHttps);
    m_extraEdit->setPlainText(m_spec.extraServerDirectives.join(QLatin1Char('\n')));
}

void NginxSiteEditor::collectSpecFromForm()
{
    m_spec.fileName = m_isNew ? m_nameEdit->text().trimmed() : m_fileName;
    m_spec.serverNames = splitTokens(m_namesEdit->text());
    m_spec.listenPort = m_portSpin->value();
    m_spec.defaultServer = m_defaultChk->isChecked();
    m_spec.clientMaxBodySize = m_bodySizeEdit->text().trimmed();
    m_spec.root = m_rootEdit->text().trimmed();
    m_spec.indexFiles = splitTokens(m_indexEdit->text());
    m_spec.spaFallback = m_spaChk->isChecked();
    m_spec.proxyAll = m_proxyAllChk->isChecked();
    m_spec.proxyPass = m_proxyPassEdit->text().trimmed();
    m_spec.proxyWebSocket = m_wsChk->isChecked();
    m_spec.listenSsl = m_sslChk->isChecked();
    m_spec.sslCertificate = m_certEdit->text().trimmed();
    m_spec.sslCertificateKey = m_keyEdit->text().trimmed();
    m_spec.forceHttps = m_forceHttpsChk->isChecked();
    m_spec.extraServerDirectives =
        m_extraEdit->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    m_spec.locations = m_locations;
}

void NginxSiteEditor::rebuildLocationTable()
{
    m_locTable->setRowCount(m_locations.size());
    for (int i = 0; i < m_locations.size(); ++i) {
        const nginx::LocationSpec &s = m_locations.at(i);
        m_locTable->setItem(i, 0, new QTableWidgetItem(s.modifier.isEmpty() ? tr("前缀")
                                                                           : s.modifier));
        m_locTable->setItem(i, 1, new QTableWidgetItem(s.pattern));
        m_locTable->setItem(i, 2, new QTableWidgetItem(locationKindLabel(s.kind)));
        m_locTable->setItem(i, 3, new QTableWidgetItem(locationTargetText(s)));
    }
}

// ---------------------------------------------------------------------------
// 预览 / 编辑动作 / 保存
// ---------------------------------------------------------------------------

void NginxSiteEditor::refreshPreview()
{
    if (m_switching || m_modeTabs->currentIndex() != 0 || !m_preview)
        return;
    m_preview->setPlainText(nginx::NginxConfigParser::renderSite(m_spec));
}

QString NginxSiteEditor::renderedText()
{
    if (m_modeTabs->currentIndex() == 1)
        return m_textEditor->toPlainText();
    collectSpecFromForm();
    return nginx::NginxConfigParser::renderSite(m_spec);
}

void NginxSiteEditor::onFieldChanged()
{
    if (m_switching)
        return;
    collectSpecFromForm();
    m_formDirty = true;
    refreshPreview();
}

void NginxSiteEditor::onAddLocation()
{
    nginx::LocationSpec s;
    s.kind = QStringLiteral("proxy");
    s.modifier = QString();
    s.pattern = QStringLiteral("/api/");
    if (editLocationSpec(this, &s, true)) {
        m_locations.append(s);
        rebuildLocationTable();
        onFieldChanged();
    }
}

void NginxSiteEditor::onEditLocation()
{
    editLocationAt(m_locTable->currentRow());
}

void NginxSiteEditor::editLocationAt(int row)
{
    if (row < 0 || row >= m_locations.size())
        return;
    nginx::LocationSpec s = m_locations.at(row);
    if (editLocationSpec(this, &s, false)) {
        m_locations[row] = s;
        rebuildLocationTable();
        onFieldChanged();
    }
}

void NginxSiteEditor::onRemoveLocation()
{
    const int row = m_locTable->currentRow();
    if (row < 0 || row >= m_locations.size())
        return;
    m_locations.removeAt(row);
    rebuildLocationTable();
    onFieldChanged();
}

void NginxSiteEditor::onModeChanged(int index)
{
    if (m_switching)
        return;
    m_switching = true;
    if (index == 1) {
        // 进入文本模式：首次载入原文（表单有改动则用生成结果）。
        if (!m_textInitialized) {
            collectSpecFromForm();
            const QString generated = nginx::NginxConfigParser::renderSite(m_spec);
            const QString content = m_formDirty
                                        ? generated
                                        : (m_originalText.isEmpty() ? generated : m_originalText);
            m_textEditor->setPlainText(content);
            m_textInitialized = true;
        }
    } else {
        // 回到表单模式：尽力把文本解析回表单（失败则保留表单并提示）。
        if (m_textInitialized) {
            const QString text = m_textEditor->toPlainText();
            QList<nginx::Directive> top;
            QString err;
            if (nginx::NginxConfigParser::parse(text, &top, &err)) {
                const QList<nginx::Directive> servers =
                    nginx::NginxConfigParser::extractServerDirectives(top);
                const nginx::Directive *main = nullptr;
                bool forceHttps = false;
                for (const nginx::Directive &s : servers) {
                    if (isHttpsRedirectServer(s)) {
                        forceHttps = true;
                        continue;
                    }
                    if (!main)
                        main = &s;
                }
                if (!main && !servers.isEmpty())
                    main = &servers.first();
                if (main) {
                    nginx::SiteSpec spec;
                    nginx::NginxConfigParser::fillSpecFromServer(*main, &spec);
                    spec.fileName = m_isNew ? m_nameEdit->text().trimmed() : m_fileName;
                    spec.forceHttps = forceHttps;
                    m_spec = spec;
                    m_locations = spec.locations;
                    loadSpecIntoForm();
                    rebuildLocationTable();
                    refreshPreview();
                }
            } else {
                QMessageBox::warning(
                    this, tr("解析失败"),
                    tr("文本模式内容无法解析为 nginx 配置，表单未更新：\n%1").arg(err));
            }
        }
    }
    m_switching = false;
}

void NginxSiteEditor::onSaveClicked()
{
    const bool textMode = (m_modeTabs->currentIndex() == 1);
    QString fileName;
    if (textMode) {
        fileName = m_isNew ? m_nameEdit->text().trimmed() : m_fileName;
    } else {
        collectSpecFromForm();
        fileName = m_spec.fileName;
    }
    if (fileName.isEmpty()) {
        QMessageBox::warning(this, tr("无效文件名"), tr("请填写站点配置文件名（如 app.conf）。"));
        if (!textMode)
            m_fieldTabs->setCurrentIndex(0);
        return;
    }
    QString path = m_path;
    if (m_isNew) {
        const QString dir = QFileInfo(m_path).absolutePath();
        path = dir + QLatin1Char('/') + fileName;
    }
    const QString text = textMode ? m_textEditor->toPlainText()
                                  : nginx::NginxConfigParser::renderSite(m_spec);
    emit saveRequested(path, text);
}

void NginxSiteEditor::markSaved()
{
    accept();
}

} // namespace cubeshell
