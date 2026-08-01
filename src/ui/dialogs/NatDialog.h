#pragma once

// NatDialog.h — 内网穿透（FRP）对话框。
//
// 对应Python: cube-shell.py::_ensure_nat_dialog（UI，L1143-1193）
//           + on_NAT_traversal（点击逻辑，L1401-1454）
//           + _on_frp_status_updated / _on_frp_progress_updated
//             / _on_frp_connect_finished（L1456-1496）
//           + nat_lod（回读 frpc.toml，L1499-1512）
//           + nat_traversal（设备下拉填充，L2207-2219）
//
// UI 与 Python 版一一对应：QFormLayout 五行（设备/Token/本地端口/服务端口/
// 协议类型）+ 底部单个「连接 / 停止」按钮，无状态标签、无日志面板、无多规则表。
//
// 线程分工：SSH 侧流程全部在 FrpConnectWorker 里跑；本地 frpc 进程由
// FrpManager（QProcess）在主线程启停 —— QProcess 必须与宿主对象同线程，
// 所以 startFrpc/stopFrpc 只能放在 finishedSignal 的主线程回调里。

#include <QDialog>
#include <QString>

class QComboBox;
class QLineEdit;
class QProgressDialog;
class QPushButton;

namespace cubeshell {

class DeviceConfigStore;
class FrpManager;
class FrpConnectWorker;

class NatDialog : public QDialog {
    Q_OBJECT
public:
    // manager / store 均不被本对话框持有，须比对话框存活更久。
    // store 提供设备下拉内容与 SSH 凭据（对应Python: config.dat 的 pickle 读取）。
    explicit NatDialog(FrpManager *manager, const DeviceConfigStore *store,
                       QWidget *parent = nullptr);
    // C++ 特有安全修复：worker 是重写 run() 的 QThread，若仍在运行时被销毁会
    // 触发 "QThread: Destroyed while thread is still running" 未定义行为。
    // Python 版靠 GC 语义不会出现（线程对象被 MainWindow 持有到进程退出）。
    ~NatDialog() override;

protected:
    // C++ 特有安全修复：关闭对话框时收口 worker 与进度框，避免留下悬挂窗口。
    void closeEvent(QCloseEvent *event) override;

private slots:
    // 对应Python: on_NAT_traversal（「连接 / 停止」按钮）
    void onConnectClicked();
    // 对应Python: _on_frp_status_updated
    void onStatusUpdated(const QString &message);
    // 对应Python: _on_frp_progress_updated
    void onProgressUpdated(int percent);
    // 对应Python: _on_frp_connect_finished
    void onConnectFinished(bool success, const QString &errorMessage, bool isStart);

private:
    // 填充设备下拉（跳过 RDP 设备）。对应Python: nat_traversal
    void populateDevices();
    // 回读 frpc.toml 填表（只取第一条 proxy）。对应Python: nat_lod
    void natLod();
    // 进度框/消息框的宿主窗口 —— Python 侧用的是主窗口（self）。
    QWidget *hostWindow() const;
    // C++ 特有安全修复：请求中断并等待 worker 退出，同时关掉进度框。
    // 超时未退出时把 worker 从父子关系中摘出、让它自行结束后自删，
    // 避免「运行中被销毁」；见实现里的超时说明。
    void shutdownWorker();
    // 关闭并释放进度框（对应Python: self._frp_progress.close() + = None）
    void closeProgress();

    FrpManager *m_manager;             // not owned
    const DeviceConfigStore *m_store;  // not owned

    // 对应Python: ui.comboBox / lineEdit / lineEdit_2 / lineEdit_3 /
    //             comboBox_3 / pushButton
    QComboBox *m_device = nullptr;
    QLineEdit *m_token = nullptr;
    QLineEdit *m_localPort = nullptr;
    QLineEdit *m_serverPort = nullptr;
    QComboBox *m_antType = nullptr;
    QPushButton *m_button = nullptr;

    // 对应Python: self._frp_progress / self._frp_connect_thread / self.NAT
    QProgressDialog *m_progress = nullptr;
    FrpConnectWorker *m_worker = nullptr;
    bool m_nat = false;
};

} // namespace cubeshell
