#pragma once

// RdpPanel.h — RDP 连接面板（凭据表单 + 会话控制条 + 状态显示 + 画面）。
//
// C++ port of core/rdp/rdp_client.py::RDPWidget 的 UI 部分（简化版）：
// Python 版是纯画面控件（参数由 ShellTab 传入）；C++ 版按任务约定合并为
// "表单 + 状态 + 画面" 的独立面板。FreeRDP 后端时画面帧显示在面板内的
// 画布上（保持宽高比缩放，同 Python _flush_frame）；命令行后备时画面在
// 外部客户端窗口，面板只展示连接状态。
// 仅在 CUBESHELL_WITH_RDP=ON 时编译。
//
// 分层：凭据表单（主机/端口/用户名/密码/域）连上后隐藏，会话控制条
//（分辨率 + 适应窗口 + 应用 + 连接 + 断开 + 状态）始终可见——分辨率必须能在
// 会话中调整，"连上就没地方改" 是用户明确反馈过的问题。同 SerialTerminalWidget
// 的取向（那里也是工具栏连接后不隐藏）。
//
// 分辨率只有一个来源：本面板。设备配置与 rdp:// URL 里都没有分辨率字段，
// 外部（main_window）只喂凭据、随后调 beginConnect()，避免出现 "下拉框显示一个值、
// 实际连接用另一个值" 的分裂状态（历史 bug：算出的分辨率不在固定档位里就被
// findText() 静默丢弃，用户改了下拉框却看不到变化）。

#include <QPixmap>
#include <QPoint>
#include <QSize>
#include <QWidget>

#include "RdpClient.h"

class QCheckBox;
class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QMouseEvent;
class QPushButton;
class QResizeEvent;
class QSpinBox;
class QTimer;
class QWheelEvent;

namespace cubeshell {

class RdpPanel : public QWidget {
    Q_OBJECT
public:
    explicit RdpPanel(QWidget *parent = nullptr);

    RdpClient *client() const { return m_client; }
    // 表单当前内容 → 连接参数（分辨率取自 targetResolution()）。
    RdpSettings currentSettings() const;
    // 外部（设备列表 / URL 分发）预填凭据。不含分辨率——见文件头说明。
    void setSettings(const RdpSettings &settings);

    // 唯一建连入口："连接" 按钮与外部（main_window::openRdpTab）共用，
    // 保证两条路径用的是同一份分辨率。外部调用前须让面板完成一次布局
    //（openRdpTab 里用 QTimer::singleShot(0, …) 延后），否则 "适应窗口"
    // 量到的是控件的初始假尺寸。
    void beginConnect();

    // 配置里没存密码时调用：把焦点放到密码框并在状态栏提示。
    // RDP 是图形画面，没有终端可以就地问密码（SSH 走 TerminalPrompt），
    // 所以复用面板本来就有的连接前表单，不额外弹对话框。
    void promptForPassword();

    // 分辨率工具（静态部分对外可见便于单测）。
    // 夹进 [640x480, 4096x2304] 并按协议要求对齐（宽 4 的倍数、高 2 的倍数）。
    static QSize alignResolution(QSize size);
    // 容错解析 "1920x1080" / "1920X1080" / 带空格；非法返回无效 QSize。
    static QSize parseResolution(const QString &text);

protected:
    void resizeEvent(QResizeEvent *event) override;
    // --- 输入事件（对应Python: RDPWidget 的 keyPressEvent/mouse*/wheelEvent/drop） ---
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    // 会话中按新分辨率重连（RDP 改不了活动会话的分辨率，见 .cpp 注释）。
    void onApplyResolutionClicked();
    void onStateChanged(cubeshell::RdpClient::State state);
    void onError(const QString &message);
    // 对应Python: RDPWidget.updateImage + _flush_frame（帧显示，等比缩放）
    void onFrameUpdated(const QImage &frame);
    // 对应Python: _flush_frame（定时器回调，合并小矩形为单次渲染）
    void flushFrame();

private:
    void setFormEnabled(bool enabled);

    // "适应窗口" 的目标分辨率：画布逻辑尺寸（远端 1 像素 = 本地 1 逻辑像素）。
    QSize fitResolution() const;
    // 实际要用的分辨率：勾了 "适应窗口" 取 fitResolution()，否则取下拉框值。
    QSize targetResolution() const;
    // 把 targetResolution() 回填到下拉框，让用户看得见将要用（或正在用）的值。
    void updateResolutionDisplay();
    // 状态栏文案：已连接时带上分辨率/实际画面尺寸。
    void updateConnectedStatus();
    // 分辨率选择的跨会话记忆（QSettings）。
    void loadResolutionPrefs();
    void saveResolutionPrefs() const;

    // 键盘输入。对应Python: RDPWidget.send_key (lines 779-848)
    void sendKey(QKeyEvent *event, bool isPressed);

    // 控件坐标 → 画面分辨率坐标（扣除 letterbox 黑边）。
    // 对应Python: RDPWidget._map_pos (lines 857-875)
    QPoint mapPos(const QPointF &widgetPos) const;
    // 对应Python: RDPWidget.send_mouse (lines 877-892)
    void sendMouse(const QPointF &pos, quint16 flags);

    RdpClient *m_client = nullptr;

    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QLineEdit *m_userEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QLineEdit *m_domainEdit = nullptr;
    QComboBox *m_resolutionCombo = nullptr;   // 可编辑：档位之外可手填 "宽x高"
    QCheckBox *m_fitCheck = nullptr;          // 分辨率跟随画布尺寸
    QPushButton *m_applyButton = nullptr;     // 按新分辨率重连
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_disconnectButton = nullptr;
    QLabel *m_statusLabel = nullptr;
    QWidget *m_formPanel = nullptr;    // 凭据表单，连接后隐藏
    QWidget *m_controlBar = nullptr;   // 分辨率/连接控制 + 状态，始终可见
    QLabel *m_canvas = nullptr;        // FreeRDP 后端的画面显示区

    // 重连中（onApplyResolutionClicked 的断开→连接之间）：抑制 Disconnected
    // 分支把凭据表单闪出来。
    bool m_reconnecting = false;

    // --- 帧渲染管线（对应Python: _repaint_timer + _buffer + _flush_frame） ---
    QTimer *m_repaintTimer = nullptr;
    bool m_frameDirty = false;
    QPixmap m_buffer;           // 原始分辨率帧缓冲（对应Python: self._buffer）
    int m_videoWidth = 0;       // 远程桌面宽度（对应Python: self._video_w）
    int m_videoHeight = 0;      // 远程桌面高度（对应Python: self._video_h）
    bool m_hasFrame = false;    // 是否已收到首帧（对应Python: self._has_frame）

    // 滚轮残差累积（对应Python: self._wheel_resid，每满 120 发一次滚轮事件）
    int m_wheelResid = 0;
};

} // namespace cubeshell
