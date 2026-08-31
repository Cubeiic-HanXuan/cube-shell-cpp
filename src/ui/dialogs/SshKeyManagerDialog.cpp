// SshKeyManagerDialog.cpp — 见 SshKeyManagerDialog.h。

#include "SshKeyManagerDialog.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGuiApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "config/DeviceConfigStore.h"
#include "ssh/SshCopyIdWorker.h"

namespace cubeshell {

namespace {
// worker 关停等待上限：照 NatDialog::shutdownWorker 的先例。
constexpr int kWorkerShutdownWaitMs = 3000;

// 类型的展示文案：ssh-rsa + 2048 → "RSA 2048"；ecdsa-…nistp384 → "ECDSA nistp384"。
QString displayType(const SshKeyEntry &k)
{
    if (k.type == QLatin1String("ssh-ed25519"))
        return QStringLiteral("Ed25519");
    if (k.type == QLatin1String("ssh-rsa"))
        return QStringLiteral("RSA %1").arg(k.bits);
    if (k.type.startsWith(QLatin1String("ecdsa-sha2-")))
        return QStringLiteral("ECDSA %1").arg(k.type.mid(QStringLiteral("ecdsa-sha2-").size()));
    return k.type;
}
} // namespace

SshKeyManagerDialog::SshKeyManagerDialog(DeviceConfigStore *deviceStore, QWidget *parent)
    : QDialog(parent)
    , m_deviceStore(deviceStore)
{
    setWindowTitle(tr("SSH 密钥管理"));
    resize(760, 460);
    setModal(false);

    auto *layout = new QVBoxLayout(this);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({tr("名称"), tr("类型"), tr("指纹"), tr("备注"), tr("创建时间")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->header()->setStretchLastSection(true);
    m_tree->setRootIsDecorated(false);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    // 双击 = 看详情（完整指纹 + 公钥行）。
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *, int) {
        showDetails();
    });
    layout->addWidget(m_tree, 1);

    m_status = new QLabel(this);
    layout->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(this);
    QPushButton *genBtn = buttons->addButton(tr("生成密钥"), QDialogButtonBox::ActionRole);
    QPushButton *copyBtn = buttons->addButton(tr("复制公钥"), QDialogButtonBox::ActionRole);
    m_deployBtn = buttons->addButton(tr("部署到设备"), QDialogButtonBox::ActionRole);
    QPushButton *detailBtn = buttons->addButton(tr("查看详情"), QDialogButtonBox::ActionRole);
    QPushButton *delBtn = buttons->addButton(tr("删除"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    connect(genBtn, &QPushButton::clicked, this, &SshKeyManagerDialog::generateKey);
    connect(copyBtn, &QPushButton::clicked, this, &SshKeyManagerDialog::copyPublicKey);
    connect(m_deployBtn, &QPushButton::clicked, this, &SshKeyManagerDialog::deploySelected);
    connect(detailBtn, &QPushButton::clicked, this, &SshKeyManagerDialog::showDetails);
    connect(delBtn, &QPushButton::clicked, this, &SshKeyManagerDialog::removeSelected);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // 没有设备目录时禁用部署（理论上 MainWindow 总会传，防御一下）。
    m_deployBtn->setEnabled(m_deviceStore != nullptr);

    rebuildTree();
}

SshKeyManagerDialog::~SshKeyManagerDialog()
{
    shutdownWorker();
}

void SshKeyManagerDialog::closeEvent(QCloseEvent *event)
{
    shutdownWorker();
    QDialog::closeEvent(event);
}

void SshKeyManagerDialog::rebuildTree()
{
    m_tree->clear();
    const QList<SshKeyEntry> all = m_keyStore.load();
    for (const SshKeyEntry &k : all) {
        auto *item = new QTreeWidgetItem({k.name, displayType(k), k.fingerprint,
                                          k.comment, k.createdAt});
        item->setData(0, Qt::UserRole, k.id);
        item->setToolTip(2, k.fingerprint);
        item->setToolTip(3, k.publicKeyLine);
        m_tree->addTopLevelItem(item);
    }
    m_status->setText(all.isEmpty()
        ? tr("还没有密钥，点「生成密钥」创建第一把。")
        : tr("共 %1 把密钥").arg(all.size()));
}

bool SshKeyManagerDialog::selectedKey(SshKeyEntry *out) const
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item)
        return false;
    const QString id = item->data(0, Qt::UserRole).toString();
    if (id.isEmpty())
        return false;
    for (const SshKeyEntry &k : m_keyStore.load()) {
        if (k.id == id) {
            if (out)
                *out = k;
            return true;
        }
    }
    return false;
}

void SshKeyManagerDialog::generateKey()
{
    SshKeyGenParams params;
    QString name;
    if (!promptGenerate(&params, &name))
        return;

    // RSA-4096 生成可能约 1s，给个等待光标。
    setCursor(Qt::WaitCursor);
    SshKeyEntry entry;
    QString err;
    const bool ok = m_keyStore.createKey(name, params, &entry, &err);
    unsetCursor();
    if (!ok) {
        QMessageBox::warning(this, tr("生成失败"), err);
        return;
    }
    rebuildTree();
    m_status->setText(tr("已生成「%1」（%2）").arg(entry.name, displayType(entry)));
}

void SshKeyManagerDialog::copyPublicKey()
{
    SshKeyEntry k;
    if (!selectedKey(&k)) {
        m_status->setText(tr("请先选中一把密钥"));
        return;
    }
    QGuiApplication::clipboard()->setText(k.publicKeyLine);
    m_status->setText(tr("公钥已复制到剪贴板（%1）").arg(k.name));
}

void SshKeyManagerDialog::deploySelected()
{
    if (!m_deviceStore)
        return;
    SshKeyEntry k;
    if (!selectedKey(&k)) {
        m_status->setText(tr("请先选中一把密钥"));
        return;
    }
    if (m_deployWorker && m_deployWorker->isRunning()) {
        m_status->setText(tr("正在部署中，请稍候…"));
        return;
    }

    const QString devName = pickDeployDevice();
    if (devName.isEmpty())
        return;

    // resolved() 取回带密码/密钥的完整条目（首次调用可能解锁钥匙串）。
    const DeviceEntry dev = m_deviceStore->resolved(devName);
    if (dev.name.isEmpty()) {
        QMessageBox::warning(this, tr("部署公钥"), tr("找不到设备「%1」").arg(devName));
        return;
    }

    auto *worker = new SshCopyIdWorker(dev, k.publicKeyLine, this);
    m_deployWorker = worker;
    m_deployBtn->setEnabled(false);
    m_status->setText(tr("正在部署到 %1 …").arg(devName));
    connect(worker, &SshCopyIdWorker::finishedSignal,
            this, &SshKeyManagerDialog::onDeployFinished, Qt::QueuedConnection);
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void SshKeyManagerDialog::onDeployFinished(bool ok, const QString &msg, bool alreadyPresent)
{
    Q_UNUSED(alreadyPresent);
    m_deployBtn->setEnabled(true);
    m_status->setText(msg);
    if (!ok)
        QMessageBox::warning(this, tr("部署公钥"), msg);
    // worker 由 finished → deleteLater 自删；这里只清指针。
    m_deployWorker = nullptr;
}

void SshKeyManagerDialog::removeSelected()
{
    SshKeyEntry k;
    if (!selectedKey(&k)) {
        m_status->setText(tr("请先选中一把密钥"));
        return;
    }
    if (QMessageBox::question(this, tr("删除密钥"),
                              tr("确定删除密钥「%1」吗？\n私钥与公钥文件将一并删除，且无法恢复。")
                                  .arg(k.name))
        != QMessageBox::Yes)
        return;
    QString err;
    if (m_keyStore.remove(k.id, &err)) {
        rebuildTree();
    } else {
        QMessageBox::warning(this, tr("删除失败"), err);
    }
}

void SshKeyManagerDialog::showDetails()
{
    SshKeyEntry k;
    if (!selectedKey(&k)) {
        m_status->setText(tr("请先选中一把密钥"));
        return;
    }
    QMessageBox::information(this, tr("密钥详情 — %1").arg(k.name),
                             tr("类型：%1\n指纹：%2\n创建时间：%3\n私钥：%4\n\n公钥：\n%5")
                                 .arg(displayType(k), k.fingerprint, k.createdAt,
                                      k.privateKeyPath, k.publicKeyLine));
}

// 生成表单：名称、类型、（按类型变的）长度/曲线、注释、可选口令。
bool SshKeyManagerDialog::promptGenerate(SshKeyGenParams *params, QString *name)
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("生成密钥对"));
    auto *form = new QFormLayout(&dlg);

    auto *nameEdit = new QLineEdit(&dlg);
    nameEdit->setPlaceholderText(tr("可选，留空则自动命名"));

    auto *typeCombo = new QComboBox(&dlg);
    typeCombo->addItem(QStringLiteral("Ed25519"), QStringLiteral("ssh-ed25519"));
    typeCombo->addItem(QStringLiteral("RSA"), QStringLiteral("ssh-rsa"));
    typeCombo->addItem(QStringLiteral("ECDSA"), QStringLiteral("ecdsa"));

    auto *sizeCombo = new QComboBox(&dlg);
    auto refreshSizes = [sizeCombo, typeCombo]() {
        sizeCombo->clear();
        const QString t = typeCombo->currentData().toString();
        if (t == QLatin1String("ssh-rsa")) {
            sizeCombo->setEnabled(true);
            sizeCombo->addItem(QStringLiteral("2048"), 2048);
            sizeCombo->addItem(QStringLiteral("3072"), 3072);
            sizeCombo->addItem(QStringLiteral("4096"), 4096);
        } else if (t == QLatin1String("ecdsa")) {
            sizeCombo->setEnabled(true);
            sizeCombo->addItem(QStringLiteral("nistp256"), QStringLiteral("ecdsa-sha2-nistp256"));
            sizeCombo->addItem(QStringLiteral("nistp384"), QStringLiteral("ecdsa-sha2-nistp384"));
            sizeCombo->addItem(QStringLiteral("nistp521"), QStringLiteral("ecdsa-sha2-nistp521"));
        } else {
            sizeCombo->setEnabled(false);   // Ed25519 无可调参数
        }
    };
    connect(typeCombo, &QComboBox::currentIndexChanged, this, refreshSizes);
    refreshSizes();

    auto *commentEdit = new QLineEdit(&dlg);
    commentEdit->setPlaceholderText(tr("可选，如 user@host"));
    auto *passEdit = new QLineEdit(&dlg);
    passEdit->setEchoMode(QLineEdit::Password);
    passEdit->setPlaceholderText(tr("可选；留空则私钥不加密"));
    auto *pass2Edit = new QLineEdit(&dlg);
    pass2Edit->setEchoMode(QLineEdit::Password);
    pass2Edit->setPlaceholderText(tr("再次输入口令"));

    form->addRow(tr("名称："), nameEdit);
    form->addRow(tr("类型："), typeCombo);
    form->addRow(tr("长度/曲线："), sizeCombo);
    form->addRow(tr("注释："), commentEdit);
    form->addRow(tr("私钥口令："), passEdit);
    form->addRow(tr("确认口令："), pass2Edit);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(bb);

    if (dlg.exec() != QDialog::Accepted)
        return false;
    if (passEdit->text() != pass2Edit->text()) {
        QMessageBox::warning(this, tr("生成密钥对"), tr("两次输入的口令不一致。"));
        return false;
    }

    // 归一化类型：ECDSA 的具体曲线由 sizeCombo 决定。
    QString type = typeCombo->currentData().toString();
    int bits = 0;
    if (type == QLatin1String("ssh-rsa")) {
        bits = sizeCombo->currentData().toInt();
    } else if (type == QLatin1String("ecdsa")) {
        type = sizeCombo->currentData().toString();   // ecdsa-sha2-nistpN
    }

    params->type = type;
    params->bits = bits;
    params->comment = commentEdit->text();
    params->passphrase = passEdit->text();
    if (name)
        *name = nameEdit->text().trimmed();
    return true;
}

QString SshKeyManagerDialog::pickDeployDevice()
{
    // 只列 SSH 设备（部署公钥只对 SSH 有意义）。
    QStringList sshNames;
    for (const DeviceEntry &d : m_deviceStore->devices()) {
        if (d.isSsh())
            sshNames.append(d.name);
    }
    if (sshNames.isEmpty()) {
        QMessageBox::information(this, tr("部署公钥"), tr("没有可部署的 SSH 设备。"));
        return QString();
    }

    QDialog dlg(this);
    dlg.setWindowTitle(tr("部署到设备"));
    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel(tr("选择要部署公钥的设备："), &dlg));
    auto *list = new QListWidget(&dlg);
    list->addItems(sshNames);
    if (list->count() > 0)
        list->setCurrentRow(0);
    layout->addWidget(list);
    // 双击即选定。
    connect(list, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(bb);

    if (dlg.exec() != QDialog::Accepted || !list->currentItem())
        return QString();
    return list->currentItem()->text();
}

void SshKeyManagerDialog::shutdownWorker()
{
    if (m_deployWorker && m_deployWorker->isRunning()) {
        // run() 纯阻塞，quit() 无效，只能 requestInterruption() + 检查点提前返回。
        m_deployWorker->requestInterruption();
        if (!m_deployWorker->wait(kWorkerShutdownWaitMs)) {
            // 卡在无法打断的阻塞调用里：摘掉父子关系并断开指向本对象的连接，
            // 让它跑完自行销毁——宁可短暂泄漏，也不能在运行中析构 QThread。
            SshCopyIdWorker *orphan = m_deployWorker;
            m_deployWorker = nullptr;
            disconnect(orphan, nullptr, this, nullptr);
            orphan->setParent(nullptr);
            // finished → deleteLater 的连接在 deploySelected 里已建立。
        }
    }
}

} // namespace cubeshell
