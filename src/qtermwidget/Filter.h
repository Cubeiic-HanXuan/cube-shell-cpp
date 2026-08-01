#pragma once

// Filter.h — C++ port of qtermwidget/filter.py
//
// Text filter system: scans the terminal screen image for patterns (URLs,
// e-mail addresses, custom highlight regexes, permission strings) and marks
// matching regions as interactive "hot spots".
//
// Ported from the Python PySide6 version (converted from Konsole / QTermWidget).
//
// Original copyright:
//   Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>

#include <QList>
#include <QMultiHash>
#include <QObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <QColor>

#include "Character.h"          // Character, LineProperty, LINE_DEFAULT, LINE_WRAPPED, RE_EXTENDED_CHAR
#include "konsole_wcwidth.h"    // string_width

class QAction;

namespace Konsole {

// 对应C++: class Filter : public QObject
//
// A filter processes blocks of text looking for certain patterns (such as URLs
// or keywords from a list) and marks the areas which match the filter's pattern
// as 'hotspots'.
class Filter : public QObject
{
    Q_OBJECT

public:
    // 对应C++: class Filter::HotSpot
    //
    // Represents an area of text which matched the pattern a particular filter
    // has been looking for. Each hotspot has a type identifier and an action.
    class HotSpot
    {
    public:
        // 对应C++: enum Type { NotSpecified, Link, Marker, Highlight }
        enum Type {
            NotSpecified = 0,
            Link         = 1,
            Marker       = 2,
            Highlight    = 3   // 自定义高亮 (cube-shell 新增)
        };

        // 对应C++: HotSpot(int startLine, int startColumn, int endLine, int endColumn)
        HotSpot(int startLine, int startColumn, int endLine, int endColumn);
        virtual ~HotSpot() = default;

        // 对应C++: int startLine() const 等
        int startLine()  const { return _startLine; }
        int endLine()    const { return _endLine; }
        int startColumn() const { return _startColumn; }
        int endColumn()  const { return _endColumn; }

        // 对应C++: Type type() const / void setType(Type)
        Type type() const { return _type; }
        void setType(Type type) { _type = type; }

        // cube-shell 高亮扩展:前景/背景色 (Highlight 类型使用)。
        // 无效 QColor 表示 "无该颜色"。
        QColor foregroundColor() const { return _foregroundColor; }
        QColor backgroundColor() const { return _backgroundColor; }
        // 对应Python: setColors(fg, bg) — 设置颜色并把类型切到 Highlight
        void setColors(const QColor &fg, const QColor &bg);

        // 触发与热点关联的动作。action 为空执行默认动作,否则为 actions()
        // 列表中某个动作的 objectName。
        // 对应C++: virtual void activate(const QString& action = QString()) = 0
        virtual void activate(const QString &action = QString()) = 0;

        // 返回与热点关联的动作列表 (用于菜单/工具栏)。
        // 对应C++: virtual QList<QAction*> actions()
        virtual QList<QAction *> actions();

        // cube-shell terminal_display 用类名识别权限热点;C++ 用虚函数替代。
        virtual bool isPermissionHotSpot() const { return false; }

    private:
        int _startLine;
        int _startColumn;
        int _endLine;
        int _endColumn;
        Type _type;
        QColor _foregroundColor;
        QColor _backgroundColor;
    };

    // 对应C++: Filter()
    Filter();
    ~Filter() override;

    // 使过滤器处理当前缓冲区中的文本块。
    // 对应C++: virtual void process() = 0
    virtual void process() = 0;

    // 清空缓冲区、行计数并删除所有热点。
    // 对应C++: void reset()
    void reset();

    // 返回覆盖给定行/列的热点,没有则返回 nullptr。
    // 对应C++: HotSpot* hotSpotAt(int line, int column) const
    HotSpot *hotSpotAt(int line, int column) const;

    // 返回过滤器识别的全部热点。
    // 对应C++: QList<HotSpot*> hotSpots() const
    QList<HotSpot *> hotSpots() const { return _hotspotList; }

    // 返回过滤器在给定行识别的热点。
    // 对应C++: QList<HotSpot*> hotSpotsAtLine(int line) const
    QList<HotSpot *> hotSpotsAtLine(int line) const;

    // 设置缓冲区和行位置 (不拥有所有权,指向共享缓冲区)。
    // 对应C++: void setBuffer(const QString* buffer, const QList<int>* linePositions)
    void setBuffer(const QString *buffer, const QList<int> *linePositions);

    // 返回内部缓冲区。
    // 对应C++: const QString* buffer()
    const QString *buffer() const { return _buffer; }

protected:
    // 向列表添加新热点 (过滤器拥有所有权)。
    // 对应C++: void addHotSpot(HotSpot*)
    void addHotSpot(HotSpot *spot);

    // 把 buffer() 中的字符位置转换为行/列。
    // 对应C++: void getLineColumn(int position, int& startLine, int& startColumn)
    void getLineColumn(int position, int &startLine, int &startColumn);

private:
    QMultiHash<int, HotSpot *> _hotspots; // line -> hotspots
    QList<HotSpot *>           _hotspotList;
    const QList<int>          *_linePositions = nullptr;
    const QString             *_buffer = nullptr;
};

// 对应C++: class RegExpFilter : public Filter
//
// A filter which searches for sections of text matching a regular expression
// and creates a new RegExpFilter::HotSpot instance for them.
class RegExpFilter : public Filter
{
    Q_OBJECT

public:
    // 对应C++: class RegExpFilter::HotSpot : public Filter::HotSpot
    class HotSpot : public Filter::HotSpot
    {
    public:
        // 对应C++: HotSpot(int startLine, int startColumn, int endLine, int endColumn)
        HotSpot(int startLine, int startColumn, int endLine, int endColumn);

        // 对应C++: void activate(const QString& action = QString()) override
        void activate(const QString &action = QString()) override;

        // 对应C++: void setCapturedTexts(const QStringList& texts)
        void setCapturedTexts(const QStringList &texts) { _capturedTexts = texts; }
        // 对应C++: QStringList capturedTexts() const
        QStringList capturedTexts() const { return _capturedTexts; }

    private:
        QStringList _capturedTexts;
    };

    // 对应C++: RegExpFilter()
    RegExpFilter();

    // 对应C++: void setRegExp(const QRegularExpression& text)
    void setRegExp(const QRegularExpression &regExp);
    // 对应C++: QRegularExpression regExp() const
    QRegularExpression regExp() const { return _searchText; }

    // 对应C++: void process() override
    void process() override;

protected:
    // 遇到正则匹配时调用。子类可重写以返回自定义热点类型。
    // 对应C++: virtual RegExpFilter::HotSpot* newHotSpot(int,int,int,int)
    virtual RegExpFilter::HotSpot *newHotSpot(int startLine, int startColumn,
                                              int endLine, int endColumn);

private:
    QRegularExpression _searchText;
};

// cube-shell 自定义高亮过滤器 (对应 Python HighlightFilter)。
// 根据正则表达式高亮显示文本,可设置前景/背景色。
class HighlightFilter : public RegExpFilter
{
    Q_OBJECT

public:
    // 对应Python: HighlightFilter(regex_pattern, fg_color=None, bg_color=None)
    explicit HighlightFilter(const QString &regexPattern,
                             const QColor &fgColor = QColor(),
                             const QColor &bgColor = QColor());

protected:
    RegExpFilter::HotSpot *newHotSpot(int startLine, int startColumn,
                                      int endLine, int endColumn) override;

private:
    QColor _fgColor;
    QColor _bgColor;
};

// 权限字符串专用热点 (对应 Python PermissionHotSpot)。
class PermissionHotSpot : public RegExpFilter::HotSpot
{
public:
    PermissionHotSpot(int startLine, int startColumn, int endLine, int endColumn);
    bool isPermissionHotSpot() const override { return true; }
};

// cube-shell 权限字符串高亮过滤器 (对应 Python PermissionHighlightFilter)。
// 匹配 drwxr-xr-x 之类的权限字符串,d/r/w/x 在绘制时分别着色。
class PermissionHighlightFilter : public RegExpFilter
{
    Q_OBJECT

public:
    PermissionHighlightFilter();

protected:
    RegExpFilter::HotSpot *newHotSpot(int startLine, int startColumn,
                                      int endLine, int endColumn) override;
};

// 对应C++: class FilterObject : public QObject
// 辅助类,转发热点激活信号 (QAction.triggered -> HotSpot.activate)。
class FilterObject : public QObject
{
    Q_OBJECT

public:
    explicit FilterObject(Filter::HotSpot *filter);

    // 对应C++: void emitActivated(const QUrl& url, bool fromContextMenu)
    void emitActivated(const QUrl &url, bool fromContextMenu);

signals:
    // 对应C++: void activated(const QUrl& url, bool fromContextMenu)
    void activated(const QUrl &url, bool fromContextMenu);

public slots:
    // 对应C++: void activate()
    void activate();

private:
    Filter::HotSpot *_filter;
};

// 对应C++: class UrlFilter : public RegExpFilter
// 匹配文本块中 URL / 邮箱地址的过滤器。
class UrlFilter : public RegExpFilter
{
    Q_OBJECT

public:
    // 对应C++: UrlFilter()
    UrlFilter();

    // 对应C++: class UrlFilter::HotSpot : public RegExpFilter::HotSpot
    class HotSpot : public RegExpFilter::HotSpot
    {
    public:
        // 对应C++: enum UrlType { StandardUrl, Email, Unknown }
        enum UrlType {
            StandardUrl = 0,
            Email       = 1,
            Unknown     = 2
        };

        HotSpot(int startLine, int startColumn, int endLine, int endColumn);
        ~HotSpot() override;

        // 对应C++: void activate(const QString& action = QString()) override
        void activate(const QString &actionName = QString()) override;
        // 对应C++: QList<QAction*> actions() override
        QList<QAction *> actions() override;

        // 对应C++: FilterObject* getUrlObject() const
        FilterObject *getUrlObject() const { return _urlObject; }

        // 对应C++: UrlType urlType() const
        UrlType urlType() const;

    private:
        FilterObject *_urlObject;
    };

    // 正则表达式常量 (与上游 C++ 一致)。
    static const QRegularExpression FullUrlRegExp;
    static const QRegularExpression EmailAddressRegExp;
    static const QRegularExpression CompleteUrlRegExp;

signals:
    // 对应C++: void activated(const QUrl& url, bool fromContextMenu)
    void activated(const QUrl &url, bool fromContextMenu);

protected:
    // 对应C++: RegExpFilter::HotSpot* newHotSpot(int,int,int,int) override
    RegExpFilter::HotSpot *newHotSpot(int startLine, int startColumn,
                                      int endLine, int endColumn) override;
};

// 对应C++: class FilterChain : protected QList<Filter*>
// 允许把一组过滤器作为一个整体处理的链。链拥有其中的过滤器并在销毁时删除它们。
class FilterChain : protected QList<Filter *>
{
public:
    // 对应C++: FilterChain() / ~FilterChain()
    FilterChain();
    virtual ~FilterChain();

    // 对应C++: void addFilter(Filter* filter)
    void addFilter(Filter *filter);
    // 对应C++: void removeFilter(Filter* filter)
    void removeFilter(Filter *filter);
    // 对应C++: bool containsFilter(Filter* filter)
    bool containsFilter(Filter *filter) const;
    // 对应C++: void clear()
    void clear();

    // 对应C++: void reset()
    void reset();
    // 对应C++: void process()
    void process();
    // 对应C++: void setBuffer(const QString* buffer, const QList<int>* linePositions)
    void setBuffer(const QString *buffer, const QList<int> *linePositions);

    // 对应C++: Filter::HotSpot* hotSpotAt(int line, int column) const
    Filter::HotSpot *hotSpotAt(int line, int column) const;
    // 对应C++: QList<Filter::HotSpot*> hotSpots() const
    QList<Filter::HotSpot *> hotSpots() const;
    // 对应C++: QList<Filter::HotSpot> hotSpotsAtLine(int line) const
    QList<Filter::HotSpot *> hotSpotsAtLine(int line) const;

    // 返回链是否为空。
    bool empty() const { return QList<Filter *>::isEmpty(); }

protected:
    using QList<Filter *>::isEmpty;
    using QList<Filter *>::begin;
    using QList<Filter *>::end;
    using QList<Filter *>::constBegin;
    using QList<Filter *>::constEnd;
};

// 对应C++: class TerminalImageFilterChain : public FilterChain
// 处理来自终端显示的字符图像的过滤器链。
class TerminalImageFilterChain : public FilterChain
{
public:
    TerminalImageFilterChain();
    ~TerminalImageFilterChain() override;

    // 设置当前终端图像并重新跑链上所有过滤器。
    // 对应C++: void setImage(const Character* const image, int lines, int columns,
    //                         const QVector<LineProperty>& lineProperties)
    void setImage(const Character *const image, int lines, int columns,
                  const QVector<LineProperty> &lineProperties);

    // 兼容 Python 的 snake_case 调用。
    void set_image(const Character *const image, int lines, int columns,
                   const QVector<LineProperty> &lineProperties)
    {
        setImage(image, lines, columns, lineProperties);
    }

private:
    // 把一行字符解码为字符串 (对应 Python _decodeLineToString)。
    QString decodeLineToString(const Character *characters, int count,
                               bool includeTrailingWhitespace) const;

    QString    _buffer;
    QList<int> _linePositions;
};

} // namespace Konsole
