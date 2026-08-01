// Filter.cpp — C++ port of qtermwidget/filter.py
//
// Text filter system: scans the terminal screen image for patterns (URLs,
// e-mail addresses, custom highlight regexes, permission strings) and marks
// matching regions as interactive "hot spots".
//
// Ported from the Python PySide6 version (converted from Konsole / QTermWidget).
//
// Original copyright:
//   Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>

#include "Filter.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QRegularExpressionMatch>
#include <QRegularExpressionMatchIterator>

namespace Konsole {

// ---------------------------------------------------------------------------
// Filter::HotSpot
// ---------------------------------------------------------------------------

// 对应C++: Filter::HotSpot::HotSpot(int startLine, int startColumn, int endLine, int endColumn)
Filter::HotSpot::HotSpot(int startLine, int startColumn, int endLine, int endColumn)
    : _startLine(startLine)
    , _startColumn(startColumn)
    , _endLine(endLine)
    , _endColumn(endColumn)
    , _type(NotSpecified)
{
}

// 对应Python: setColors(fg, bg) — 设置颜色并把类型切到 Highlight
void Filter::HotSpot::setColors(const QColor &fg, const QColor &bg)
{
    _foregroundColor = fg;
    _backgroundColor = bg;
    setType(Highlight);
}

// 对应C++: QList<QAction*> Filter::HotSpot::actions()
QList<QAction *> Filter::HotSpot::actions()
{
    return {};
}

// ---------------------------------------------------------------------------
// Filter
// ---------------------------------------------------------------------------

// 对应C++: Filter::Filter()
Filter::Filter() = default;

// 对应C++: Filter::~Filter() — 删除所有热点
Filter::~Filter()
{
    qDeleteAll(_hotspotList);
}

// 对应C++: void Filter::reset()
void Filter::reset()
{
    qDeleteAll(_hotspotList);
    _hotspots.clear();
    _hotspotList.clear();
}

// 对应C++: void Filter::addHotSpot(HotSpot*)
void Filter::addHotSpot(HotSpot *spot)
{
    _hotspotList.append(spot);

    for (int line = spot->startLine(); line <= spot->endLine(); ++line)
        _hotspots.insert(line, spot);
}

// 对应C++: Filter::HotSpot* Filter::hotSpotAt(int line, int column) const
Filter::HotSpot *Filter::hotSpotAt(int line, int column) const
{
    const auto spots = _hotspots.values(line);
    for (HotSpot *spot : spots) {
        if (spot->startLine() == line && spot->startColumn() > column)
            continue;
        if (spot->endLine() == line && spot->endColumn() < column)
            continue;
        return spot;
    }
    return nullptr;
}

// 对应C++: QList<Filter::HotSpot*> Filter::hotSpotsAtLine(int line) const
QList<Filter::HotSpot *> Filter::hotSpotsAtLine(int line) const
{
    return _hotspots.values(line);
}

// 对应C++: void Filter::setBuffer(const QString* buffer, const QList<int>* linePositions)
void Filter::setBuffer(const QString *buffer, const QList<int> *linePositions)
{
    _buffer = buffer;
    _linePositions = linePositions;
}

// 对应C++: void Filter::getLineColumn(int position, int& startLine, int& startColumn)
void Filter::getLineColumn(int position, int &startLine, int &startColumn)
{
    startLine = 0;
    startColumn = 0;

    if (!_linePositions || !_buffer)
        return;

    for (int i = 0; i < _linePositions->size(); ++i) {
        int nextLine;
        if (i == _linePositions->size() - 1)
            nextLine = _buffer->size() + 1;
        else
            nextLine = _linePositions->value(i + 1);

        if (_linePositions->value(i) <= position && position < nextLine) {
            startLine = i;
            // 列号 = 该行起点到 position 之间文本的显示宽度
            const QString textSegment = _buffer->mid(_linePositions->value(i),
                                                     position - _linePositions->value(i));
            startColumn = string_width(textSegment);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// RegExpFilter::HotSpot
// ---------------------------------------------------------------------------

// 对应C++: RegExpFilter::HotSpot::HotSpot(...)
RegExpFilter::HotSpot::HotSpot(int startLine, int startColumn, int endLine, int endColumn)
    : Filter::HotSpot(startLine, startColumn, endLine, endColumn)
{
    setType(Marker);
}

// 对应C++: void RegExpFilter::HotSpot::activate(const QString&) — 默认空实现
void RegExpFilter::HotSpot::activate(const QString &)
{
}

// ---------------------------------------------------------------------------
// RegExpFilter
// ---------------------------------------------------------------------------

// 对应C++: RegExpFilter::RegExpFilter()
RegExpFilter::RegExpFilter() = default;

// 对应C++: void RegExpFilter::setRegExp(const QRegularExpression& text)
void RegExpFilter::setRegExp(const QRegularExpression &regExp)
{
    _searchText = regExp;
}

// 对应C++: RegExpFilter::HotSpot* RegExpFilter::newHotSpot(int,int,int,int)
RegExpFilter::HotSpot *RegExpFilter::newHotSpot(int startLine, int startColumn,
                                                int endLine, int endColumn)
{
    return new RegExpFilter::HotSpot(startLine, startColumn, endLine, endColumn);
}

// 对应C++: void RegExpFilter::process()
void RegExpFilter::process()
{
    if (!_searchText.isValid() || _searchText.pattern().isEmpty() || !buffer())
        return;

    const QString *text = buffer();

    QRegularExpressionMatchIterator it = _searchText.globalMatch(*text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();

        const int start = static_cast<int>(match.capturedStart());
        const int end   = static_cast<int>(match.capturedEnd());

        int startLine = 0, startColumn = 0, endLine = 0, endColumn = 0;
        getLineColumn(start, startLine, startColumn);
        getLineColumn(end, endLine, endColumn);

        RegExpFilter::HotSpot *spot = newHotSpot(startLine, startColumn, endLine, endColumn);

        // 修复 Python bug:原实现用 match.lastindex + 1,在没有捕获组时
        // 只设置 group(0),在有捕获组时漏掉后续的命名/可选组。这里直接用
        // capturedTexts() (group(0) + 所有捕获组)。
        spot->setCapturedTexts(match.capturedTexts());

        addHotSpot(spot);
    }
}

// ---------------------------------------------------------------------------
// HighlightFilter (cube-shell 自定义高亮)
// ---------------------------------------------------------------------------

// 对应Python: HighlightFilter(regex_pattern, fg_color=None, bg_color=None)
HighlightFilter::HighlightFilter(const QString &regexPattern,
                                 const QColor &fgColor,
                                 const QColor &bgColor)
    : _fgColor(fgColor)
    , _bgColor(bgColor)
{
    setRegExp(QRegularExpression(regexPattern));
}

RegExpFilter::HotSpot *HighlightFilter::newHotSpot(int startLine, int startColumn,
                                                   int endLine, int endColumn)
{
    RegExpFilter::HotSpot *spot = RegExpFilter::newHotSpot(startLine, startColumn,
                                                           endLine, endColumn);
    spot->setColors(_fgColor, _bgColor);
    return spot;
}

// ---------------------------------------------------------------------------
// PermissionHotSpot / PermissionHighlightFilter (cube-shell)
// ---------------------------------------------------------------------------

PermissionHotSpot::PermissionHotSpot(int startLine, int startColumn, int endLine, int endColumn)
    : RegExpFilter::HotSpot(startLine, startColumn, endLine, endColumn)
{
    setType(Filter::HotSpot::Highlight);
}

PermissionHighlightFilter::PermissionHighlightFilter()
{
    // 匹配像 drwxr-xr-x. 这样的权限字符串
    setRegExp(QRegularExpression(QStringLiteral("[-d](?:[-r][-w][-x]){3}[\\.+]?")));
}

RegExpFilter::HotSpot *PermissionHighlightFilter::newHotSpot(int startLine, int startColumn,
                                                             int endLine, int endColumn)
{
    // 创建 PermissionHotSpot:不设置统一颜色,绘制时按 d/r/w/x 细分。
    auto *spot = new PermissionHotSpot(startLine, startColumn, endLine, endColumn);
    spot->setColors(QColor(), QColor());
    spot->setType(Filter::HotSpot::Highlight);
    return spot;
}

// ---------------------------------------------------------------------------
// FilterObject
// ---------------------------------------------------------------------------

// 对应C++: FilterObject::FilterObject(Filter::HotSpot*)
FilterObject::FilterObject(Filter::HotSpot *filter)
    : _filter(filter)
{
}

// 对应C++: void FilterObject::emitActivated(const QUrl& url, bool fromContextMenu)
void FilterObject::emitActivated(const QUrl &url, bool fromContextMenu)
{
    emit activated(url, fromContextMenu);
}

// 对应C++: void FilterObject::activate()
void FilterObject::activate()
{
    auto *action = qobject_cast<QAction *>(sender());
    const QString actionName = action ? action->objectName() : QString();
    _filter->activate(actionName);
}

// ---------------------------------------------------------------------------
// UrlFilter
// ---------------------------------------------------------------------------

// 对应C++: 静态正则常量
const QRegularExpression UrlFilter::FullUrlRegExp(
    QStringLiteral("(www\\.(?!\\.)|[a-z][a-z0-9+.-]*://)[^\\s<>'\"]+[^!,\\.\\s<>'\"\\]]"));
const QRegularExpression UrlFilter::EmailAddressRegExp(
    QStringLiteral("\\b(\\w|\\.|-)+@(\\w|\\.|-)+\\.\\w+\\b"));
const QRegularExpression UrlFilter::CompleteUrlRegExp(
    QStringLiteral("((www\\.(?!\\.)|[a-z][a-z0-9+.-]*://)[^\\s<>'\"]+[^!,\\.\\s<>'\"\\]]"
                   "|\\b(\\w|\\.|-)+@(\\w|\\.|-)+\\.\\w+\\b)"));

// 对应C++: UrlFilter::UrlFilter()
UrlFilter::UrlFilter()
{
    setRegExp(CompleteUrlRegExp);
}

// 对应C++: RegExpFilter::HotSpot* UrlFilter::newHotSpot(int,int,int,int)
RegExpFilter::HotSpot *UrlFilter::newHotSpot(int startLine, int startColumn,
                                             int endLine, int endColumn)
{
    auto *spot = new UrlFilter::HotSpot(startLine, startColumn, endLine, endColumn);
    connect(spot->getUrlObject(), &FilterObject::activated,
            this, &UrlFilter::activated);
    return spot;
}

// ---------------------------------------------------------------------------
// UrlFilter::HotSpot
// ---------------------------------------------------------------------------

// 对应C++: UrlFilter::HotSpot::HotSpot(...)
UrlFilter::HotSpot::HotSpot(int startLine, int startColumn, int endLine, int endColumn)
    : RegExpFilter::HotSpot(startLine, startColumn, endLine, endColumn)
{
    setType(Link);
    _urlObject = new FilterObject(this);
}

// 对应C++: UrlFilter::HotSpot::~HotSpot()
UrlFilter::HotSpot::~HotSpot()
{
    delete _urlObject;
}

// 对应C++: UrlFilter::HotSpot::UrlType UrlFilter::HotSpot::urlType() const
UrlFilter::HotSpot::UrlType UrlFilter::HotSpot::urlType() const
{
    if (capturedTexts().isEmpty())
        return Unknown;

    const QString url = capturedTexts().first();

    if (FullUrlRegExp.match(url).hasMatch())
        return StandardUrl;
    if (EmailAddressRegExp.match(url).hasMatch())
        return Email;
    return Unknown;
}

// 对应C++: void UrlFilter::HotSpot::activate(const QString& actionName)
void UrlFilter::HotSpot::activate(const QString &actionName)
{
    if (capturedTexts().isEmpty())
        return;

    QString url = capturedTexts().first();
    const UrlType kind = urlType();

    if (actionName == QLatin1String("copy-action")) {
        QApplication::clipboard()->setText(url);
        return;
    }

    if (actionName.isEmpty() ||
        actionName == QLatin1String("open-action") ||
        actionName == QLatin1String("click-action")) {
        if (kind == StandardUrl) {
            if (!url.contains(QLatin1String("://")))
                url.prepend(QLatin1String("http://"));
        } else if (kind == Email) {
            url.prepend(QLatin1String("mailto:"));
        }

        QUrl qurl;
        qurl.setUrl(url, QUrl::StrictMode);
        _urlObject->emitActivated(qurl, actionName != QLatin1String("click-action"));
    }
}

// 对应C++: QList<QAction*> UrlFilter::HotSpot::actions()
QList<QAction *> UrlFilter::HotSpot::actions()
{
    QList<QAction *> actionsList;
    const UrlType kind = urlType();

    if (kind == StandardUrl || kind == Email) {
        auto *openAction = new QAction(_urlObject);
        auto *copyAction = new QAction(_urlObject);

        if (kind == StandardUrl) {
            openAction->setText(QStringLiteral("打开链接"));
            copyAction->setText(QStringLiteral("复制链接地址"));
        } else {
            openAction->setText(QStringLiteral("发送邮件到..."));
            copyAction->setText(QStringLiteral("复制邮件地址"));
        }

        openAction->setObjectName(QStringLiteral("open-action"));
        copyAction->setObjectName(QStringLiteral("copy-action"));

        QObject::connect(openAction, &QAction::triggered, _urlObject, &FilterObject::activate);
        QObject::connect(copyAction, &QAction::triggered, _urlObject, &FilterObject::activate);

        actionsList << openAction << copyAction;
    }

    return actionsList;
}

// ---------------------------------------------------------------------------
// FilterChain
// ---------------------------------------------------------------------------

// 对应C++: FilterChain::FilterChain()
FilterChain::FilterChain() = default;

// 对应C++: FilterChain::~FilterChain() — 链拥有过滤器,销毁时删除
FilterChain::~FilterChain()
{
    for (Filter *filter : *this)
        delete filter;
}

// 对应C++: void FilterChain::addFilter(Filter* filter)
void FilterChain::addFilter(Filter *filter)
{
    append(filter);
}

// 对应C++: void FilterChain::removeFilter(Filter* filter)
void FilterChain::removeFilter(Filter *filter)
{
    removeAll(filter);
}

// 对应C++: bool FilterChain::containsFilter(Filter* filter)
bool FilterChain::containsFilter(Filter *filter) const
{
    return contains(filter);
}

// 对应C++: void FilterChain::clear()
void FilterChain::clear()
{
    for (Filter *filter : *this)
        delete filter;
    QList<Filter *>::clear();
}

// 对应C++: void FilterChain::reset()
void FilterChain::reset()
{
    for (Filter *filter : *this)
        filter->reset();
}

// 对应C++: void FilterChain::process()
void FilterChain::process()
{
    for (Filter *filter : *this)
        filter->process();
}

// 对应C++: void FilterChain::setBuffer(const QString* buffer, const QList<int>* linePositions)
void FilterChain::setBuffer(const QString *buffer, const QList<int> *linePositions)
{
    for (Filter *filter : *this)
        filter->setBuffer(buffer, linePositions);
}

// 对应C++: Filter::HotSpot* FilterChain::hotSpotAt(int line, int column) const
Filter::HotSpot *FilterChain::hotSpotAt(int line, int column) const
{
    for (Filter *filter : *this) {
        Filter::HotSpot *spot = filter->hotSpotAt(line, column);
        if (spot)
            return spot;
    }
    return nullptr;
}

// 对应C++: QList<Filter::HotSpot*> FilterChain::hotSpots() const
QList<Filter::HotSpot *> FilterChain::hotSpots() const
{
    QList<Filter::HotSpot *> allSpots;
    for (Filter *filter : *this)
        allSpots << filter->hotSpots();
    return allSpots;
}

// 对应C++: QList<Filter::HotSpot> FilterChain::hotSpotsAtLine(int line) const
QList<Filter::HotSpot *> FilterChain::hotSpotsAtLine(int line) const
{
    QList<Filter::HotSpot *> allSpots;
    for (Filter *filter : *this)
        allSpots << filter->hotSpotsAtLine(line);
    return allSpots;
}

// ---------------------------------------------------------------------------
// TerminalImageFilterChain
// ---------------------------------------------------------------------------

// 对应C++: TerminalImageFilterChain::TerminalImageFilterChain()
TerminalImageFilterChain::TerminalImageFilterChain() = default;

// 对应C++: TerminalImageFilterChain::~TerminalImageFilterChain()
TerminalImageFilterChain::~TerminalImageFilterChain() = default;

// 对应C++: void TerminalImageFilterChain::setImage(const Character* const image,
//                                                  int lines, int columns,
//                                                  const QVector<LineProperty>& lineProperties)
void TerminalImageFilterChain::setImage(const Character *const image, int lines, int columns,
                                        const QVector<LineProperty> &lineProperties)
{
    if (empty())
        return;

    // 重置所有过滤器和热点
    reset();

    _buffer.clear();
    _linePositions.clear();

    for (int i = 0; i < lines; ++i) {
        _linePositions.append(_buffer.size());

        const Character *lineChars = image + i * columns;

        // 简化的字符解码,包含尾随空白
        _buffer += decodeLineToString(lineChars, columns, true);

        // 假装每行都以换行符结尾,防止一行末尾的链接被当成下一行开头链接的一部分。
        // 缺点是跨多行的链接不会被高亮。TODO: 利用 line-wrapped 属性避免此虚构字符。
        const LineProperty lineProp = (i < lineProperties.size()) ? lineProperties[i]
                                                                  : LINE_DEFAULT;
        if (!(lineProp & LINE_WRAPPED))
            _buffer += QLatin1Char('\n');
    }

    // 设置缓冲区给链上所有过滤器
    setBuffer(&_buffer, &_linePositions);
}

// 对应 Python _decodeLineToString — 把一行字符解码为字符串
QString TerminalImageFilterChain::decodeLineToString(const Character *characters, int count,
                                                     bool includeTrailingWhitespace) const
{
    if (!characters || count <= 0)
        return QString();

    QString plainText;
    plainText.reserve(count);

    int outputCount = count;

    // 如果禁用尾随空白,找到行尾
    if (!includeTrailingWhitespace) {
        for (int i = count - 1; i >= 0; --i) {
            if (!characters[i].isSpace())
                break;
            --outputCount;
        }
    }

    int i = 0;
    while (i < outputCount) {
        const Character &ch = characters[i];

        if (ch.rendition & RE_EXTENDED_CHAR) {
            // 扩展字符序列
            ushort length = 0;
            const uint *chars = ExtendedCharTable::instance.lookupExtendedChar(ch.extendedCharHash,
                                                                               length);
            if (chars && length > 0) {
                QString charStr;
                for (int n = 0; n < length; ++n)
                    charStr += QChar(chars[n]);
                plainText += charStr;
                i += qMax(1, string_width(charStr));
            } else {
                ++i;
            }
        } else {
            const quint32 code = ch.character;
            if (code <= 0x10FFFF) {
                QString charStr;
                if (code <= 0xFFFF)
                    charStr = QString(1, QChar(static_cast<char16_t>(code)));
                else {
                    const char32_t c32 = static_cast<char32_t>(code);
                    charStr = QString::fromUcs4(&c32, 1);
                }
                plainText += charStr;
                i += qMax(1, string_width(charStr));
            } else {
                plainText += QLatin1Char(' ');
                ++i;
            }
        }
    }

    return plainText;
}

} // namespace Konsole
