#pragma once

// HistorySearch.h — C++ port of qtermwidget/history_search.py
//
// Searches the terminal history for a regular-expression pattern, forwards or
// backwards, reading in blocks to bound memory use. Emits a signal with the
// match location (or noMatchFound) then deletes itself.
//
// Copyright 2013 Christian Surlykke

#include <QObject>
#include <QRegularExpression>

namespace Konsole {

class Emulation;

// 对应C++: class HistorySearch : public QObject
class HistorySearch : public QObject {
    Q_OBJECT
public:
    // 对应C++: HistorySearch(EmulationPtr emulation, const QRegularExpression& regExp,
    //                        bool forwards, int startColumn, int startLine, QObject* parent)
    explicit HistorySearch(Emulation *emulation,
                           const QRegularExpression &regExp,
                           bool forwards,
                           int startColumn,
                           int startLine,
                           QObject *parent = nullptr);
    ~HistorySearch() override;

    void search();

signals:
    // 对应C++: void matchFound(int startColumn, int startLine, int endColumn, int endLine)
    void matchFound(int startColumn, int startLine, int endColumn, int endLine);
    void noMatchFound();

private:
    bool searchRange(int startColumn, int startLine, int endColumn, int endLine);
    int findLineNumberInString(const QList<int> &linePositions, int position) const;

    Emulation *m_emulation;
    QRegularExpression m_regExp;
    bool m_forwards;
    int m_startColumn;
    int m_startLine;

    int m_foundStartColumn = 0;
    int m_foundStartLine = 0;
    int m_foundEndColumn = 0;
    int m_foundEndLine = 0;
};

} // namespace Konsole
