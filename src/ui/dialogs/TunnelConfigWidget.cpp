#include "TunnelConfigWidget.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include "forwarder/FrpManager.h"

namespace cubeshell {

// 对应Python: ui/tunnel_config.py::Ui_TunnelConfig.setupUi + cube-shell.py::Tunnel
TunnelConfigWidget::TunnelConfigWidget(TunnelPool *pool, QWidget *parent)
    : QWidget(parent)
    , m_pool(pool)
{
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({tr("名称"), tr("转发模式"), tr("SSH 服务器"),
                             tr("本地绑定地址"), tr("状态")});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);

    m_startStop = new QPushButton(tr("开启隧道"), this);
    m_add = new QPushButton(tr("新增"), this);
    m_remove = new QPushButton(tr("删除"), this);

    m_log = new QLabel(this);
    m_log->setStyleSheet(QStringLiteral("color: gray;"));
    m_log->setWordWrap(true);

    auto *btnRow = new QHBoxLayout;
    btnRow->addWidget(m_startStop);
    btnRow->addStretch(1);
    btnRow->addWidget(m_add);
    btnRow->addWidget(m_remove);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_tree, 1);
    layout->addLayout(btnRow);
    layout->addWidget(m_log);

    connect(m_startStop, &QPushButton::clicked, this, &TunnelConfigWidget::onStartStop);
    connect(m_add, &QPushButton::clicked, this, &TunnelConfigWidget::addTunnelRequested);
    connect(m_remove, &QPushButton::clicked, this, &TunnelConfigWidget::onRemove);

    // TunnelPool 的信号在本线程发射（内部已回投），DirectConnection 即可。
    connect(m_pool, &TunnelPool::tunnelStateChanged,
            this, &TunnelConfigWidget::onStateChanged);
    connect(m_pool, &TunnelPool::tunnelLog,
            this, &TunnelConfigWidget::onLog);

    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this]() {
        const QString name = selectedName();
        m_startStop->setText(m_pool->isRunning(name) ? tr("关闭隧道") : tr("开启隧道"));
    });

    refresh();
}

void TunnelConfigWidget::setFrpManager(FrpManager *frp)
{
    m_frp = frp;
    if (!m_frp)
        return;
    // FRP 进程日志同样打到面板日志行（QueuedConnection：与工程跨线程约定一致）。
    connect(m_frp, &FrpManager::logOutput, this,
            [this](const QString &name, const QString &line) {
                m_log->setText(QStringLiteral("[%1] %2").arg(name, line));
            }, Qt::QueuedConnection);
}

// 对应Python: cube-shell.py::tunnel_refresh
void TunnelConfigWidget::refresh()
{
    m_pool->loadConfig();
    m_tree->clear();
    const QStringList names = m_pool->tunnelNames();
    for (const QString &name : names) {
        const TunnelEntry e = m_pool->entry(name);
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, name);
        item->setText(1, e.tunnelType);
        item->setText(2, e.deviceName);
        item->setText(3, e.localBindAddress);
        item->setText(4, stateText(m_pool->state(name)));
    }
}

QString TunnelConfigWidget::selectedName() const
{
    QTreeWidgetItem *item = m_tree->currentItem();
    return item ? item->text(0) : QString();
}

QTreeWidgetItem *TunnelConfigWidget::itemForName(const QString &name) const
{
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        if (m_tree->topLevelItem(i)->text(0) == name)
            return m_tree->topLevelItem(i);
    }
    return nullptr;
}

QString TunnelConfigWidget::stateText(TunnelPool::TunnelState state)
{
    switch (state) {
    case TunnelPool::TunnelState::Stopped:      return tr("已停止");
    case TunnelPool::TunnelState::Connecting:   return tr("连接中");
    case TunnelPool::TunnelState::Running:      return tr("运行中");
    case TunnelPool::TunnelState::Reconnecting: return tr("重连中");
    case TunnelPool::TunnelState::Failed:       return tr("失败");
    }
    return QString();
}

// 对应Python: cube-shell.py::Tunnel.start_tunnel / stop_tunnel
void TunnelConfigWidget::onStartStop()
{
    const QString name = selectedName();
    if (name.isEmpty())
        return;
    if (m_pool->isRunning(name))
        m_pool->stopTunnel(name);
    else
        m_pool->startTunnel(name);
}

// 对应Python: cube-shell.py::Tunnel.delete_tunnel
void TunnelConfigWidget::onRemove()
{
    const QString name = selectedName();
    if (name.isEmpty())
        return;
    if (QMessageBox::question(this, tr("删除隧道"),
                              tr("确定要删除隧道“%1”吗？").arg(name)) != QMessageBox::Yes)
        return;
    m_pool->stopTunnel(name);
    m_pool->removeEntry(name);
    refresh();
}

void TunnelConfigWidget::onStateChanged(const QString &name, TunnelPool::TunnelState state)
{
    if (QTreeWidgetItem *item = itemForName(name))
        item->setText(4, stateText(state));
    if (name == selectedName())
        m_startStop->setText(state == TunnelPool::TunnelState::Running ? tr("关闭隧道") : tr("开启隧道"));
}

void TunnelConfigWidget::onLog(const QString &name, const QString &message)
{
    m_log->setText(QStringLiteral("[%1] %2").arg(name, message));
}

} // namespace cubeshell
