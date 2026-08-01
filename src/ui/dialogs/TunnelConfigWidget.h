#pragma once

// TunnelConfigWidget.h — 隧道配置面板（列表 + 启停 + CRUD）。
// 对应Python: ui/tunnel_config.py + cube-shell.py::Tunnel/tunnel_refresh
// 对接 TunnelPool（配置读写 + 生命周期）；FRP 状态经 FrpManager（只读展示）。

#include <QWidget>

#include "ssh/TunnelPool.h"

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace cubeshell {

class FrpManager;

class TunnelConfigWidget : public QWidget {
    Q_OBJECT
public:
    // pool 由主窗口拥有（credential resolver 也在主窗口设置）；不转移所有权。
    explicit TunnelConfigWidget(TunnelPool *pool, QWidget *parent = nullptr);

    // 可选注入 FrpManager 用于展示 frpc/frps 状态（Phase 5 全量接入）。
    void setFrpManager(FrpManager *frp);

    // 重新加载 tunnel.json 并刷新列表。
    // 对应Python: cube-shell.py::tunnel_refresh
    void refresh();

signals:
    // 需要新增隧道（由主窗口弹出 AddTunnelDialog——设备列表在主窗口手里）。
    void addTunnelRequested();

private slots:
    void onStartStop();
    void onRemove();
    void onStateChanged(const QString &name, cubeshell::TunnelPool::TunnelState state);
    void onLog(const QString &name, const QString &message);

private:
    QString selectedName() const;
    QTreeWidgetItem *itemForName(const QString &name) const;
    static QString stateText(TunnelPool::TunnelState state);

    TunnelPool *m_pool;              // 不拥有
    FrpManager *m_frp = nullptr;     // 不拥有

    QTreeWidget *m_tree = nullptr;
    QPushButton *m_startStop = nullptr;
    QPushButton *m_add = nullptr;
    QPushButton *m_remove = nullptr;
    QLabel *m_log = nullptr;
};

} // namespace cubeshell
