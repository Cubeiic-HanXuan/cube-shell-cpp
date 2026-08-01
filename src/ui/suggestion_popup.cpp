#include "suggestion_popup.h"

// suggestion_popup.cpp — 终端智能提示候选弹窗实现。
// 对应Python: cube-shell.py::_SuggestionPopup (L7083-7290)
//           + _SuggestionDelegate (L7042-7080)

#include <QColor>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <QVariantMap>

namespace cubeshell {

namespace {

// 候选列表自定义绘制代理 —— 左侧圆点 + 文本 + 右侧淡色类型标注。
// 对应Python: _SuggestionDelegate (L7042-7080)
class SuggestionDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        // 基类绘制背景和文本 (L7055)
        QStyledItemDelegate::paint(painter, option, index);
        // 叠加左侧彩色圆点 (L7056-7071)
        const QVariantMap payload = index.data(Qt::UserRole).toMap();
        if (payload.isEmpty())
            return;
        QString kind = payload.value(QStringLiteral("kind")).toString();
        if (kind.isEmpty())
            kind = QStringLiteral("token");
        // 类型 -> 圆点颜色（history 蓝、token/command 紫）(L7047-7051)
        const QColor dotColor(kind == QLatin1String("history")
                                  ? QStringLiteral("#4fc1ff")
                                  : QStringLiteral("#c586c0"));
        const QRect rect = option.rect;
        painter->save();
        const int dotX = rect.left() + 10;
        const int dotY = rect.top() + (rect.height() - 8) / 2;
        painter->setPen(Qt::NoPen);
        painter->setBrush(dotColor);
        painter->drawEllipse(dotX, dotY, 8, 8);
        painter->restore();
    }

    // 固定行高 28（与弹窗尺寸算法保持一致）(L7078-7079)
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return QSize(200, 28);
    }
};

} // namespace

// 对应Python: _SuggestionPopup.__init__ (L7084-7143)
SuggestionPopup::SuggestionPopup(QWidget *owner)
    : QFrame(nullptr)
    , m_owner(owner)
{
    // 轻量、非激活式的提示弹窗：展示补全候选但不抢占终端焦点，
    // 避免"弹窗抢焦点 -> 终端失焦 -> 弹窗关闭"的闪烁，并保证输入流畅。
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setFocusPolicy(Qt::NoFocus);
    setFrameShape(QFrame::Box);
    setLineWidth(1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(0);

    m_list = new QListWidget(this);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setItemDelegate(new SuggestionDelegate(m_list));
    // 鼠标点击某条候选时应用该候选。
    // 对应Python: _on_item_clicked (L7283-7290)
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (!item)
            return;
        m_hasUserSelection = true;
        const QVariantMap payload = item->data(Qt::UserRole).toMap();
        emit suggestionApplied(payload.value(QStringLiteral("kind")).toString(),
                               payload.value(QStringLiteral("text")).toString());
        hide();
    });
    layout->addWidget(m_list);

    // 样式表照抄 Python (L7121-7143)
    setStyleSheet(QStringLiteral(R"(
            QFrame {
                background-color: #252526;
                color: #cccccc;
                border: 1px solid #404040;
                border-radius: 6px;
            }
            QListWidget {
                background-color: transparent;
                border: 0px;
                outline: 0px;
            }
            QListWidget::item {
                padding: 2px 8px 2px 26px;
            }
            QListWidget::item:selected {
                background-color: #094771;
                color: #ffffff;
            }
            QListWidget::item:hover {
                background-color: #2a2d2e;
            }
        )"));
}

// 鼠标移入弹窗时标记为交互中，用于暂停候选自动刷新。
// 对应Python: enterEvent (L7145-7148)
void SuggestionPopup::enterEvent(QEnterEvent *event)
{
    m_interacting = true;
    QFrame::enterEvent(event);
}

// 鼠标移出弹窗时结束交互状态。
// 对应Python: leaveEvent (L7150-7153)
void SuggestionPopup::leaveEvent(QEvent *event)
{
    m_interacting = false;
    QFrame::leaveEvent(event);
}

// 对应Python: updateSuggestions (L7159-7202)
void SuggestionPopup::updateSuggestions(const QList<SuggestionItem> &items)
{
    // 候选集合没变时不重建列表，减少 UI 更新开销 (L7166-7168)
    QStringList sig;
    const int n = qMin(items.size(), 20);
    sig.reserve(n);
    for (int i = 0; i < n; ++i)
        sig << items[i].kind + QLatin1Char('\x1f') + items[i].text;
    if (sig == m_sig && isVisible())
        return;
    m_sig = sig;
    m_hasUserSelection = false;

    m_list->setUpdatesEnabled(false);
    m_list->clear();
    for (int i = 0; i < n; ++i) {
        const SuggestionItem &it = items[i];
        auto *item = new QListWidgetItem(it.text);
        QVariantMap payload;
        payload.insert(QStringLiteral("kind"), it.kind);
        payload.insert(QStringLiteral("text"), it.text);
        item->setData(Qt::UserRole, payload);
        m_list->addItem(item);
    }
    // 不默认选中第一条，只有用户显式上下选择/点击后才选中 (L7184-7185)
    m_list->setCurrentRow(-1);
    m_list->setUpdatesEnabled(true);

    // 尺寸算法照抄 Python (L7189-7202)
    const QFontMetrics fm = m_list->fontMetrics();
    int maxW = 200;
    const int extra = 28 + 12;  // 左侧圆点留白 + 右侧留白
    for (int i = 0; i < m_list->count(); ++i)
        maxW = qMax(maxW, fm.horizontalAdvance(m_list->item(i)->text()) + extra + 16);
    const int visibleRows = qMin(8, qMax(1, m_list->count()));
    const int rowH = 28;  // 与 delegate sizeHint 保持一致
    const int listH = visibleRows * rowH + 4;
    m_list->setFixedHeight(listH);
    const int frameW = qMin(420, maxW);
    // 高度 = 列表高度 + 上下边距(4+4) + 边框(1+1)
    const int frameH = listH + 10;
    setFixedSize(frameW, frameH);
}

// 对应Python: hasUserSelection (L7204-7211)
bool SuggestionPopup::hasUserSelection() const
{
    return m_hasUserSelection && m_list->currentRow() >= 0;
}

// 弹窗可见时由终端按键处理触发，用于向下选择候选。
// 对应Python: selectNext (L7213-7223)
void SuggestionPopup::selectNext()
{
    if (m_list->count() <= 0)
        return;
    int row = m_list->currentRow();
    row = (row < 0) ? 0 : qMin(m_list->count() - 1, row + 1);
    m_hasUserSelection = true;
    m_list->setCurrentRow(row);
}

// 弹窗可见时由终端按键处理触发，用于向上选择候选。
// 对应Python: selectPrev (L7225-7235)
void SuggestionPopup::selectPrev()
{
    if (m_list->count() <= 0)
        return;
    int row = m_list->currentRow();
    row = (row < 0) ? m_list->count() - 1 : qMax(0, row - 1);
    m_hasUserSelection = true;
    m_list->setCurrentRow(row);
}

// 对应Python: eventFilter 中 scrollToItem 调用 (L7426-7434)
void SuggestionPopup::scrollToCurrentItem()
{
    if (QListWidgetItem *item = m_list->currentItem())
        m_list->scrollToItem(item);
}

// 只有用户显式选中过候选（鼠标点击或上下键导航）才应用，避免回车误触发补全。
// 对应Python: applyCurrentIfSelected (L7237-7254)
bool SuggestionPopup::applyCurrentIfSelected()
{
    if (!hasUserSelection())
        return false;
    QListWidgetItem *item = m_list->currentItem();
    if (!item)
        return false;
    const QVariantMap payload = item->data(Qt::UserRole).toMap();
    emit suggestionApplied(payload.value(QStringLiteral("kind")).toString(),
                           payload.value(QStringLiteral("text")).toString());
    hide();
    return true;
}

// 确保弹窗是主窗口的子控件（懒初始化）。
// 将弹窗从独立顶层窗口转为主窗口子控件，使得所有坐标计算基于主窗口
// 内部相对坐标，彻底绕开 Linux/Wayland 下 mapToGlobal() 坐标失效的问题。
// 对应Python: _ensure_parent (L7256-7274)
void SuggestionPopup::ensureParent()
{
    if (m_reparented)
        return;
    QWidget *mainWindow = m_owner ? m_owner->window() : nullptr;
    if (mainWindow && mainWindow != m_owner) {
        m_reparented = true;
        setParent(mainWindow);
        // 作为子控件不需要窗口级别标志，setParent 已自动重置；
        // 重新设置关键属性（setParent 会重置部分状态）
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setFocusPolicy(Qt::NoFocus);
        setFrameShape(QFrame::Box);
        setLineWidth(1);
    }
}

// 在指定坐标弹出候选窗口（坐标相对于父控件/主窗口）。
// 对应Python: popupAt (L7276-7281)
void SuggestionPopup::popupAt(const QPoint &pos)
{
    ensureParent();
    move(pos);
    show();
    raise();
}

} // namespace cubeshell
