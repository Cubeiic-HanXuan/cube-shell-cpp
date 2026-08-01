// HistorySearch.cpp — C++ port of qtermwidget/history_search.py

#include "HistorySearch.h"

#include <QString>
#include <QTextStream>

#include "Emulation.h"
#include "TerminalCharacterDecoder.h"
#include "tools.h"

namespace Konsole {

HistorySearch::HistorySearch(Emulation *emulation,
                             const QRegularExpression &regExp,
                             bool forwards,
                             int startColumn,
                             int startLine,
                             QObject *parent)
    : QObject(parent)
    , m_emulation(emulation)
    , m_regExp(regExp)
    , m_forwards(forwards)
    , m_startColumn(startColumn)
    , m_startLine(startLine)
{
}

HistorySearch::~HistorySearch() = default;

void HistorySearch::search()
{
    bool found = false;

    if (m_regExp.pattern().isEmpty()) {
        emit noMatchFound();
        deleteLater();
        return;
    }

    if (m_forwards) {
        found = searchRange(m_startColumn, m_startLine, -1, m_emulation->lineCount())
                || searchRange(0, 0, m_startColumn, m_startLine);
    } else {
        found = searchRange(0, 0, m_startColumn, m_startLine)
                || searchRange(m_startColumn, m_startLine, -1, m_emulation->lineCount());
    }

    if (found) {
        emit matchFound(m_foundStartColumn, m_foundStartLine,
                        m_foundEndColumn, m_foundEndLine);
    } else {
        emit noMatchFound();
    }

    deleteLater();
}

bool HistorySearch::searchRange(int startColumn, int startLine, int endColumn, int endLine)
{
    qCDebug(qtermwidgetLogger) << "search from" << startColumn << "," << startLine
                               << "to" << endColumn << "," << endLine;

    int linesRead = 0;
    const int linesToRead = endLine - startLine + 1;

    // Read in blocks of at most 10K lines to bound memory use.
    while (true) {
        const int blockSize = qMin(10000, linesToRead - linesRead);
        if (blockSize <= 0)
            break;

        QString string;
        QTextStream searchStream(&string);

        PlainTextDecoder decoder;
        decoder.begin(&searchStream);
        decoder.setRecordLinePositions(true);

        const int blockStartLine = m_forwards
                ? startLine + linesRead
                : endLine - linesRead - blockSize + 1;
        const int chunkEndLine = blockStartLine + blockSize - 1;

        m_emulation->writeToStream(&decoder, blockStartLine, chunkEndLine);

        searchStream.flush();
        const QList<int> linePositions = decoder.linePositions();
        const int numberOfLinesInString = linePositions.count() - 1; // ignore trailing empty line

        int endPosition;
        if (numberOfLinesInString > 0 && endColumn > -1)
            endPosition = linePositions[numberOfLinesInString - 1] + endColumn;
        else
            endPosition = string.length();

        int matchStart = -1;
        QRegularExpressionMatch match;

        if (m_forwards) {
            match = m_regExp.match(string, startColumn);
            if (match.hasMatch()) {
                matchStart = match.capturedStart();
                if (matchStart >= endPosition)
                    matchStart = -1;
            }
        } else {
            // Find the last match within [startColumn, endPosition).
            QRegularExpressionMatchIterator it = m_regExp.globalMatch(string);
            QRegularExpressionMatch lastMatch;
            bool haveLast = false;
            while (it.hasNext()) {
                const QRegularExpressionMatch current = it.next();
                const int currentStart = current.capturedStart();
                if (currentStart >= startColumn && currentStart < endPosition) {
                    lastMatch = current;
                    haveLast = true;
                }
            }
            if (haveLast) {
                match = lastMatch;
                matchStart = match.capturedStart();
                if (matchStart < startColumn)
                    matchStart = -1;
            }
        }

        if (matchStart > -1) {
            const int matchEnd = matchStart + match.capturedLength() - 1;

            // Translate string positions back to history line/column coordinates.
            const int startLineNumberInString = findLineNumberInString(linePositions, matchStart);
            m_foundStartColumn = matchStart - linePositions[startLineNumberInString];
            m_foundStartLine = startLineNumberInString + startLine + linesRead;

            const int endLineNumberInString = findLineNumberInString(linePositions, matchEnd);
            m_foundEndColumn = matchEnd - linePositions[endLineNumberInString];
            m_foundEndLine = endLineNumberInString + startLine + linesRead;

            return true;
        }

        linesRead += blockSize;
    }

    return false;
}

int HistorySearch::findLineNumberInString(const QList<int> &linePositions, int position) const
{
    int lineNum = 0;
    while (lineNum + 1 < linePositions.count() && linePositions[lineNum + 1] <= position)
        ++lineNum;
    return lineNum;
}

} // namespace Konsole
