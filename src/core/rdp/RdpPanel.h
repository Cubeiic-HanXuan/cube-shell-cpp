#pragma once

// RdpPanel.h — RDP 连接面板（连接参数表单 + 连接/断开按钮 + 状态显示）。
//
// C++ port of core/rdp/rdp_client.py::RDPWidget 的 UI 部分（简化版）：
// Python 版是纯画面控件（参数由 ShellTab 传入）；C++ 版按任务约定合并为
// "表单 + 状态 + 画面" 的独立面板。FreeRDP 后端时画面帧显示在面板内的
// 画布上（保持宽高比缩放，同 Python _flush_frame）；命令行后备时画面在
// 外部客户端窗口，面板只展示连接状态。
// 仅在 CUBESHELL_WITH_RDP=ON 时编译。

#include <QPixmap>
#include <QPoint>
#include <QWidget>

#include "RdpClient.h"

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
    // 表单当前内容 → 连接参数（分辨率取自下拉框 "宽x高"）。
    RdpSettings currentSettings() const;
    // 外部（如 URL 分发）预填表单。
    void setSettings(const RdpSettings &settings);

    // 配置里没存密码时调用：把焦点放到密码框并在状态栏提示。
    // RDP 是图形画面，没有终端可以就地问密码（SSH 走 TerminalPrompt），
    // 所以复用面板本来就有的连接前表单，不额外弹对话框。
    void promptForPassword();

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
    void onStateChanged(cubeshell::RdpClient::State state);
    void onError(const QString &message);
    // 对应Python: RDPWidget.updateImage + _flush_frame（帧显示，等比缩放）
    void onFrameUpdated(const QImage &frame);
    // 对应Python: _flush_frame（定时器回调，合并小矩形为单次渲染）
    void flushFrame();

private:
    void setFormEnabled(bool enabled);

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
    QComboBox *m_resolutionCombo = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_disconnectButton = nullptr;
    QLabel *m_statusLabel = nullptr;
    QWidget *m_formPanel = nullptr;  // 表单+按钮+状态容器，连接后整体隐藏
    QLabel *m_canvas = nullptr;      // FreeRDP 后端的画面显示区

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
