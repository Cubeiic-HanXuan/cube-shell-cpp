// NatDialog.cpp — 内网穿透（FRP）对话框。见 NatDialog.h。
//
// 对应Python: cube-shell.py::_ensure_nat_dialog / on_NAT_traversal /
//             _on_frp_connect_finished / nat_lod / nat_traversal

#include "NatDialog.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QIcon>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSize>
#include <QVBoxLayout>
#include <QVariantList>
#include <QVariantMap>

#include "config/ConfigUtil.h"
#include "config/DeviceConfigStore.h"
#include "forwarder/FrpConnectWorker.h"
#include "forwarder/FrpManager.h"

Q_LOGGING_CATEGORY(natDialogLog, "cubeshell.ui.nat")

namespace cubeshell {

namespace {
// worker 收口等待上限。run() 里的单次阻塞调用（SSH 握手、远端 exec、下载）
// 无法被打断，最坏情况需要等它自然返回；这里给 3 秒是「阶段之间的中断检查
// 点足够密（最长一段是 2 秒的 time.sleep 等价物，已改成分片可中断）」的估算。
// 超时后走摘出父子关系 + 自删的兜底路径，绝不在运行中析构 QThread。
const int kWorkerShutdownWaitMs = 3000;
} // namespace

// 对应Python: _ensure_nat_dialog
NatDialog::NatDialog(FrpManager *manager, const DeviceConfigStore *store, QWidget *parent)
    : QDialog(parent)
    , m_manager(manager)
    , m_store(store)
{
    setWindowTitle(tr("内网穿透"));       // 对应Python: dlg.setWindowTitle
    setMinimumSize(500, 300);             // 对应Python: dlg.setMinimumSize(500, 300)
    setModal(false);                      // 对应Python: dlg.setModal(False)

    auto *layout = new QVBoxLayout(this); // 对应Python: QVBoxLayout(dlg)
    auto *form = new QFormLayout();       // 对应Python: QFormLayout()

    m_device = new QComboBox(this);       // 对应Python: combo
    m_token = new QLineEdit(this);        // 对应Python: line1（明文，无 EchoMode）
    m_localPort = new QLineEdit(this);    // 对应Python: line2
    m_serverPort = new QLineEdit(this);   // 对应Python: line3
    m_antType = new QComboBox(this);      // 对应Python: combo3
    m_antType->addItems({QStringLiteral("TCP"), QStringLiteral("UDP"),
                         QStringLiteral("HTTP"), QStringLiteral("HTTPS"),
                         QStringLiteral("STCP"), QStringLiteral("SUDP"),
                         QStringLiteral("XTCP")});
    m_antType->setMinimumWidth(160);
    m_antType->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_button = new QPushButton(tr("连接 / 停止"), this); // 对应Python: btn

    form->addRow(tr("设备："), m_device);
    form->addRow(tr("Token："), m_token);
    form->addRow(tr("本地端口："), m_localPort);
    form->addRow(tr("服务端口："), m_serverPort);
    form->addRow(tr("协议类型："), m_antType);
    layout->addLayout(form);
    layout->addWidget(m_button);

    connect(m_button, &QPushButton::clicked, this, &NatDialog::onConnectClicked);

    // 先填充设备列表，再加载已保存的配置（Python 侧各自 try/except 容错）
    populateDevices();
    natLod();
}

NatDialog::~NatDialog()
{
    // C++ 特有安全修复：worker 以本对话框为 parent，若不先收口，QObject 析构
    // 链会在线程仍运行时删掉 QThread → 未定义行为。
    shutdownWorker();
}

// C++ 特有安全修复：Python 版关闭对话框时后台线程仍在跑（线程对象挂在
// MainWindow 上、进度框 parent 是主窗口），关掉对话框会留下一个无人回收的
// 进度框，且线程结果无处可去。C++ 侧必须在这里收口：请求中断 + 关进度框。
void NatDialog::closeEvent(QCloseEvent *event)
{
    shutdownWorker();
    QDialog::closeEvent(event);
}

void NatDialog::closeProgress()
{
    // 对应Python: self._frp_progress.close(); self._frp_progress = None
    if (m_progress) {
        m_progress->close();
        m_progress->deleteLater();
        m_progress = nullptr;
    }
}

void NatDialog::shutdownWorker()
{
    if (m_worker && m_worker->isRunning()) {
        // FrpConnectWorker 重写了 run() 且没有事件循环，quit()/exit() 对它无效，
        // 只能靠 requestInterruption() + run() 内的检查点提前返回。
        m_worker->requestInterruption();
        if (!m_worker->wait(kWorkerShutdownWaitMs)) {
            // 卡在无法打断的阻塞调用里（SSH 握手 / 远端 exec / 下载）。
            // 摘掉父子关系并断开指向本对象的连接，让它跑完自行销毁 ——
            // 宁可短暂泄漏一个线程对象，也不能在运行中析构 QThread。
            qCWarning(natDialogLog)
                << "FRP worker did not stop within" << kWorkerShutdownWaitMs
                << "ms; detaching it to finish on its own";
            FrpConnectWorker *orphan = m_worker;
            m_worker = nullptr;
            disconnect(orphan, nullptr, this, nullptr);
            orphan->setParent(nullptr);
            // finished → deleteLater 的连接在 onConnectClicked 里已建立，
            // 线程结束后会在主线程事件循环中自删。
        }
    }
    closeProgress();
}

QWidget *NatDialog::hostWindow() const
{
    // Python 侧进度框/消息框的 parent 是主窗口（self）。
    if (QWidget *p = parentWidget())
        return p->window();
    return const_cast<NatDialog *>(this);
}

// 对应Python: nat_traversal（跳过 RDP 设备，条目带 SSH 图标）
void NatDialog::populateDevices()
{
    if (!m_store)
        return;
    QIcon iconSsh;
    // 对应Python: icon_ssh.addFile(u":icons8-ssh-48.png", QSize(),
    //                              QIcon.Mode.Selected, QIcon.State.On)
    iconSsh.addFile(QStringLiteral(":icons8-ssh-48.png"), QSize(), QIcon::Selected,
                    QIcon::On);
    for (const DeviceEntry &e : m_store->devices()) {
        // 对应Python: if device_protocol(dic.get(k)) == "rdp": continue
        //
        // frp 内网穿透在这里只支持 SSH 设备（要用它的凭据登录去部署 frpc），
        // 故显式按协议过滤，非 SSH 一律跳过。
        //
        // 早先这里用 e.host.isEmpty() 当"是不是 RDP"的替身判定（RDP 条目经
        // PickleReader 后确实没有 host）。TCP/Telnet 条目**有** host，会被
        // 误当成 SSH 设备列进来；串口条目则是碰巧被这个判据滤掉的。
        if (!e.isSsh())
            continue;
        m_device->addItem(iconSsh, e.name);
    }
}

// 对应Python: nat_lod
void NatDialog::natLod()
{
    QString err;
    const QVariantMap config = ConfigUtil::readToml(FrpConnectWorker::frpcConfigPath(), &err);
    if (!err.isEmpty()) {
        // 对应Python: try: self.nat_lod() except: pass（文件不存在时静默）
        qCWarning(natDialogLog) << "nat_lod failed:" << err;
        return;
    }
    // 对应Python: if 'auth' in config
    if (!config.contains(QStringLiteral("auth")))
        return;

    const QVariantMap auth = config.value(QStringLiteral("auth")).toMap();
    m_device->setCurrentText(config.value(QStringLiteral("serverAddr")).toString());
    m_token->setText(auth.value(QStringLiteral("token")).toString());

    // 对应Python: for proxy in proxies: ... break（只取第一条）
    const QVariantList proxies = config.value(QStringLiteral("proxies")).toList();
    for (const QVariant &item : proxies) {
        const QVariantMap proxy = item.toMap();
        m_antType->setCurrentText(proxy.value(QStringLiteral("type")).toString().toUpper());
        m_localPort->setText(proxy.value(QStringLiteral("localPort")).toString());
        if (proxy.contains(QStringLiteral("remotePort")))
            m_serverPort->setText(proxy.value(QStringLiteral("remotePort")).toString());
        break;
    }
}

// 对应Python: on_NAT_traversal
void NatDialog::onConnectClicked()
{
    // Python 侧无重入保护；这里避免重复起线程（同一时刻只允许一个流程）。
    if (m_worker && m_worker->isRunning())
        return;

    FrpConnectParams params;
    params.token = m_token->text();          // 对应Python: token = lineEdit.text()
    params.antType = m_antType->currentText();
    params.localPort = m_localPort->text();
    params.serverPort = m_serverPort->text(); // 对应Python: server_prot = lineEdit_3.text()

    // 对应Python: pickle.loads(config.dat)[device] 取 username/password/host
    //             /key_type/key_file（3 字段与 5 字段两种格式）
    const QString device = m_device->currentText();
    // resolved()：内网穿透要真的建 SSH 连接，需要密码（find() 拿到的条目不带）。
    const DeviceEntry entry = m_store ? m_store->resolved(device) : DeviceEntry{};
    if (entry.name.isEmpty()) {
        // Python 在此抛 KeyError（槽内未捕获）；C++ 侧给出可见提示。
        QMessageBox::warning(hostWindow(), tr("错误"),
                             tr("未找到设备“%1”的连接配置。").arg(device));
        return;
    }
    params.host = entry.host;
    params.username = entry.username;
    params.password = entry.password;
    params.keyType = entry.keyType;
    params.keyFile = entry.keyFile;

    // 显示进度对话框（对应Python: QProgressDialog(..., None, 0, 0, self)）
    closeProgress(); // C++ 特有：清掉上一轮可能残留的进度框，避免悬挂窗口
    m_progress = new QProgressDialog(m_nat ? tr("正在停止服务...") : tr("正在连接服务器..."),
                                     QString(), 0, 0, hostWindow());
    m_progress->setCancelButton(nullptr); // 对应Python: setCancelButton(None)
    m_progress->setWindowTitle(tr("内网穿透"));
    m_progress->setWindowModality(Qt::WindowModal);
    m_progress->setMinimumDuration(0);
    m_progress->setMinimumWidth(300);
    m_progress->show();
    QApplication::processEvents(); // 对应Python: QApplication.processEvents()

    // 启动后台线程处理连接和服务（对应Python: FRPConnectThread(...).start()）
    m_worker = new FrpConnectWorker(params, m_nat /* is_stop */, this);
    connect(m_worker, &FrpConnectWorker::statusUpdated, this, &NatDialog::onStatusUpdated);
    connect(m_worker, &FrpConnectWorker::progressUpdated, this, &NatDialog::onProgressUpdated);
    connect(m_worker, &FrpConnectWorker::finishedSignal, this, &NatDialog::onConnectFinished);
    connect(m_worker, &FrpConnectWorker::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &QObject::destroyed, this, [this] { m_worker = nullptr; });
    m_worker->start();
}

// 对应Python: _on_frp_status_updated
void NatDialog::onStatusUpdated(const QString &message)
{
    if (m_progress)
        m_progress->setLabelText(message);
}

// 对应Python: _on_frp_progress_updated
void NatDialog::onProgressUpdated(int percent)
{
    if (m_progress)
        m_progress->setValue(percent);
}

// 对应Python: _on_frp_connect_finished
void NatDialog::onConnectFinished(bool success, const QString &errorMessage, bool isStart)
{
    // 对应Python: self._frp_progress.close(); self._frp_progress = None
    closeProgress();

    if (!success) {
        // 对应Python: QMessageBox.warning(self, self.tr("错误"), error_msg)
        QMessageBox::warning(hostWindow(), tr("错误"), errorMessage);
        return;
    }

    if (isStart) {
        // 本地 frpc 的启动：Python 在 worker 线程用 os.system/subprocess 起裸
        // 进程；C++ 侧交给 FrpManager 的 QProcess，必须在主线程执行。
        // 对应Python: cd frp_dir && nohup frpc -c frpc.toml > frpc.log 2>&1 &
        QString startError;
        if (m_manager) {
            m_manager->stopFrpc(1000); // 复位可能残留的托管进程
            if (!m_manager->startFrpc(FrpConnectWorker::frpcConfigPath(), &startError)) {
                // Python 侧 os.system 无返回值检查，这里如实反馈失败原因。
                QMessageBox::warning(hostWindow(), tr("错误"), startError);
                return;
            }
        }

        // 启动成功（对应Python: icon1.addFile(u":off.png", ...)）
        QIcon icon1;
        icon1.addFile(QStringLiteral(":off.png"), QSize(), QIcon::Normal, QIcon::Off);
        m_button->setIcon(icon1);
        m_nat = true;
        natLod(); // 对应Python: try: self.nat_lod() except: pass
        QMessageBox::information(hostWindow(), tr("完成"), tr("FRP 内网穿透已成功启动！"));
    } else {
        // 停止本地 frpc（对应Python: worker 里的 pkill -9 frpc / taskkill）
        if (m_manager)
            m_manager->stopFrpc();
        FrpConnectWorker::killLocalFrpc(); // 清掉非托管的游离进程

        // 停止成功（对应Python: icon1.addFile(u":open.png", ...)）
        QIcon icon1;
        icon1.addFile(QStringLiteral(":open.png"), QSize(), QIcon::Normal, QIcon::Off);
        m_button->setIcon(icon1);
        m_nat = false;
        natLod(); // 对应Python: try: self.nat_lod() except: pass
    }
}

} // namespace cubeshell
