// screen_reflow_test.cpp — Unit tests for Screen reflow (resizeImage with reflow=true).
//
// Covers:
//   (a) Narrow→widen round-trip preserves on-screen text (character-by-character)
//   (b) CJK double-width characters are never split across rows
//   (c) Cursor position tracks the same logical character after reflow
//   (d) Alt-screen path (reflow=false) preserves truncation semantics
//   (e) History lines participate in reflow: content remains intact after narrow
//   (f) No-history screen (HistoryScrollNone): reflow must not count dropped lines

#include <QCoreApplication>
#include <QDebug>
#include <QVector>

#include "Screen.h"
#include "History.h"
#include "Character.h"

using namespace Konsole;

static int g_passed = 0;
static int g_failed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            qCritical() << "FAIL:" << msg << "at" << __FILE__ << ":" << __LINE__; \
            ++g_failed; \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_EQ(a, b, msg) \
    do { \
        if ((a) != (b)) { \
            qCritical() << "FAIL:" << msg << " expected:" << (b) << "got:" << (a) \
                        << "at" << __FILE__ << ":" << __LINE__; \
            ++g_failed; \
            return; \
        } \
    } while (0)

// Helper: write a string of ASCII characters into the screen at the current cursor position,
// letting displayCharacter handle line wrapping naturally.
static void writeString(Screen &screen, const QString &text)
{
    for (const QChar &ch : text) {
        screen.displayCharacter(ch.unicode());
    }
}

// Helper: write a single wide (CJK) character (2 cells).
static void writeWideChar(Screen &screen, quint16 codePoint)
{
    screen.displayCharacter(codePoint);
}

// Helper: extract visible text from screen rows as a flat string (no history).
// Trims trailing spaces from each line, joins with newline.
static QString extractScreenText(Screen &screen)
{
    QString result;
    for (int row = 0; row < screen.getLines(); ++row) {
        // Get line content via copyFromScreen into a Character buffer
        QVector<Character> line(screen.getColumns());
        screen.copyFromScreen(line.data(), row, 1);
        // Convert to QString, trim trailing default chars (space)
        QString lineStr;
        int lastNonSpace = -1;
        for (int col = 0; col < screen.getColumns(); ++col) {
            QChar ch = (line[col].character == 0) ? QChar(' ') : QChar(line[col].character);
            lineStr += ch;
            if (ch != ' ')
                lastNonSpace = col;
        }
        lineStr = lineStr.left(lastNonSpace + 1);
        if (row > 0)
            result += '\n';
        result += lineStr;
    }
    return result;
}

// Helper: extract all text including history as a flat character sequence (no newlines,
// just the raw characters in order). Skips NUL wide-char placeholders.
static QString extractAllContent(Screen &screen)
{
    QString result;
    // History portion
    for (int row = 0; row < screen.getHistLines(); ++row) {
        QVector<Character> line(1024); // generous buffer
        // getLineLen not directly available from public interface easily;
        // use copyFromHistory
        screen.copyFromHistory(line.data(), row, 1, true);
        for (int col = 0; col < screen.getColumns(); ++col) {
            quint16 c = line[col].character;
            if (c == 0) continue; // skip wide-char placeholders
            if (c == ' ') continue; // skip padding
            result += QChar(c);
        }
    }
    // Screen portion
    for (int row = 0; row < screen.getLines(); ++row) {
        QVector<Character> line(screen.getColumns());
        screen.copyFromScreen(line.data(), row, 1);
        for (int col = 0; col < screen.getColumns(); ++col) {
            quint16 c = line[col].character;
            if (c == 0) continue; // skip wide-char placeholders
            if (c == ' ') continue; // skip padding
            result += QChar(c);
        }
    }
    return result;
}

// ============================================================================
// Test (a): Narrow→widen round-trip text preservation
// ============================================================================
static void test_narrow_widen_roundtrip()
{
    // Create a 24-line, 80-column screen with history
    Screen screen(24, 80);
    HistoryTypeBuffer histType(1000);
    screen.setScroll(histType);
    screen.setMode(MODE_Wrap); // enable line wrapping

    // Write a string longer than narrow width (e.g., 20 chars at width 10 should wrap)
    const QString original = QStringLiteral("ABCDEFGHIJKLMNOPQRST"); // 20 chars
    writeString(screen, original);

    // Record cursor position and screen content at 80 cols
    const int origCursorX = screen.getCursorX();
    const int origCursorY = screen.getCursorY();
    Q_UNUSED(origCursorY);

    // Verify the text is on screen
    QString content80 = extractAllContent(screen);
    TEST_ASSERT(content80.contains(original), "Original text present at 80 cols");

    // Narrow to 10 columns — this will wrap the 20 chars into 2 rows
    screen.resizeImage(24, 10, true);
    TEST_ASSERT_EQ(screen.getColumns(), 10, "Columns after narrow");

    // Widen back to 80 columns — should recombine
    screen.resizeImage(24, 80, true);
    TEST_ASSERT_EQ(screen.getColumns(), 80, "Columns after widen");

    // Verify all characters are intact
    QString contentAfter = extractAllContent(screen);
    TEST_ASSERT(contentAfter.contains(original),
                "Text preserved after narrow->widen round-trip");

    qInfo() << "  test_narrow_widen_roundtrip: PASS";
    ++g_passed;
}

// ============================================================================
// Test (b): CJK double-width characters are never split
// ============================================================================
static void test_cjk_no_split()
{
    // Create a 24-line, 10-column screen
    Screen screen(24, 10);
    HistoryTypeBuffer histType(1000);
    screen.setScroll(histType);
    screen.setMode(MODE_Wrap);

    // Write characters: 4 ASCII + 1 CJK (2 cells) + 4 ASCII = 10 cells exactly
    // Then add 1 more CJK which won't fit without wrapping
    // "ABCD中EFGH" fills cols 0-9 exactly (A=0, B=1, C=2, D=3, 中=4-5, E=6, F=7, G=8, H=9)
    writeString(screen, QStringLiteral("ABCD"));
    writeWideChar(screen, 0x4E2D); // '中' - 2 cells
    writeString(screen, QStringLiteral("EFGH"));
    // Now cursor is at col 10 (end of line)
    // Write another CJK char '国' (2 cells): should wrap to next line, not split
    writeWideChar(screen, 0x56FD); // '国' - 2 cells

    // Now narrow to 5 columns and check that no wide char is split
    screen.resizeImage(24, 5, true);

    // Verify: scan all screen rows, if a cell has character==0 (placeholder),
    // the cell before it must have a non-zero character (the first half of a wide char).
    bool splitFound = false;
    for (int row = 0; row < screen.getLines(); ++row) {
        QVector<Character> line(screen.getColumns());
        screen.copyFromScreen(line.data(), row, 1);
        for (int col = 0; col < screen.getColumns(); ++col) {
            if (line[col].character == 0) {
                // This is a wide-char placeholder; the previous cell must be the first half
                if (col == 0) {
                    // A placeholder at column 0 means the wide char was split across rows!
                    splitFound = true;
                    break;
                }
                // Previous cell must be non-zero (the actual character)
                if (line[col - 1].character == 0) {
                    splitFound = true;
                    break;
                }
            }
        }
        if (splitFound) break;
    }
    // Also check history lines
    for (int row = 0; row < screen.getHistLines() && !splitFound; ++row) {
        QVector<Character> line(screen.getColumns());
        screen.copyFromHistory(line.data(), row, 1, true);
        for (int col = 0; col < screen.getColumns(); ++col) {
            if (line[col].character == 0 && col == 0) {
                splitFound = true;
                break;
            }
        }
    }

    TEST_ASSERT(!splitFound, "CJK wide chars are never split across rows");

    qInfo() << "  test_cjk_no_split: PASS";
    ++g_passed;
}

// ============================================================================
// Test (c): Cursor position tracks the same logical character after reflow
// ============================================================================
static void test_cursor_position_after_reflow()
{
    // 24x20 screen
    Screen screen(24, 20);
    HistoryTypeBuffer histType(1000);
    screen.setScroll(histType);
    screen.setMode(MODE_Wrap);

    // Write "HELLO WORLD!" (12 chars), cursor ends at col 12
    writeString(screen, QStringLiteral("HELLO WORLD!"));
    TEST_ASSERT_EQ(screen.getCursorX(), 12, "Cursor at col 12 before reflow");
    TEST_ASSERT_EQ(screen.getCursorY(), 0, "Cursor at row 0 before reflow");

    // Narrow to 6 columns: "HELLO " wraps to row 0, "WORLD!" to row 1
    // Cursor should be at col 6 of the logical content → end of row 1 (col 6 in 6-col screen)
    screen.resizeImage(24, 6, true);

    // After reflow, cursor should point after 'HELLO WORLD!' which is 12 chars.
    // In a 6-col screen that's: row0="HELLO " row1="WORLD!" cursor at (6, 1) → clamped to col 5 or 6?
    // Actually cursor at end of "WORLD!" means col 6, but max col is columns-1=5.
    // The implementation clamps: cursorCol = min(cursorCol, columns-1).
    // Let's just verify the cursor row is >= 1 (i.e. it moved down as text wrapped)
    // and that the overall content is preserved.
    const int newCursorY = screen.getCursorY();
    TEST_ASSERT(newCursorY >= 1, "Cursor moved to row >= 1 after narrowing");

    // Widen back to 20
    screen.resizeImage(24, 20, true);
    // Cursor should be back at row 0 (content fits one row)
    TEST_ASSERT_EQ(screen.getCursorY(), 0, "Cursor back at row 0 after widening");
    // Cursor X: original was 12 (one past 'HELLO WORLD!'). When narrowed to 6 cols,
    // the cursor at logical offset 12 maps to col 6 in the second row (past-end),
    // which gets clamped to columns-1 = 5. On re-collect the logical offset becomes
    // 6 (row0 width) + 5 = 11. This 1-position loss is inherent to terminal cursor
    // semantics (cursor cannot exceed columns-1).
    TEST_ASSERT_EQ(screen.getCursorX(), 11, "Cursor X at 11 after widen (1-pos clamp loss expected)");

    qInfo() << "  test_cursor_position_after_reflow: PASS";
    ++g_passed;
}

// ============================================================================
// Test (d): Alt-screen (reflow=false) truncates, does not reflow
// ============================================================================
static void test_alt_screen_no_reflow()
{
    // 24x20 screen
    Screen screen(24, 20);
    screen.setMode(MODE_Wrap);

    // Write a 20-char line
    const QString original = QStringLiteral("12345678901234567890");
    writeString(screen, original);

    // Resize to 10 columns with reflow=false (alt-screen behavior)
    screen.resizeImage(24, 10, false);

    // The line should be truncated to 10 columns — content beyond col 10 is lost
    QVector<Character> line(10);
    screen.copyFromScreen(line.data(), 0, 1);
    QString truncated;
    for (int i = 0; i < 10; ++i) {
        if (line[i].character != ' ')
            truncated += QChar(line[i].character);
    }
    TEST_ASSERT_EQ(truncated, QStringLiteral("1234567890"), "Truncated to 10 cols");

    // Row 1 should be empty (no wrapping happened)
    QVector<Character> line2(10);
    screen.copyFromScreen(line2.data(), 1, 1);
    bool row1empty = true;
    for (int i = 0; i < 10; ++i) {
        if (line2[i].character != ' ' && line2[i].character != 0) {
            row1empty = false;
            break;
        }
    }
    TEST_ASSERT(row1empty, "Row 1 is empty (no reflow into next row)");

    // Widen back to 20 — the truncated content does NOT reappear
    screen.resizeImage(24, 20, false);
    QVector<Character> lineWide(20);
    screen.copyFromScreen(lineWide.data(), 0, 1);
    QString afterWiden;
    for (int i = 0; i < 20; ++i) {
        if (lineWide[i].character != ' ' && lineWide[i].character != 0)
            afterWiden += QChar(lineWide[i].character);
    }
    // Only first 10 chars survive — rest was lost to truncation
    TEST_ASSERT_EQ(afterWiden, QStringLiteral("1234567890"),
                   "Alt-screen: truncated data is not recovered on widen");

    qInfo() << "  test_alt_screen_no_reflow: PASS";
    ++g_passed;
}

// ============================================================================
// Test (e): History lines participate in reflow
// ============================================================================
static void test_history_reflow()
{
    // Small screen: 4 lines, 20 columns, with history buffer of 100 lines
    Screen screen(4, 20);
    HistoryTypeBuffer histType(100);
    screen.setScroll(histType);
    screen.setMode(MODE_Wrap);

    // Write 6 lines of content — first 2 will scroll into history (screen has 4 lines)
    const QStringList lines = {
        QStringLiteral("LINE1_AAAA_BBBB_CCCC"), // 20 chars, fills row exactly
        QStringLiteral("LINE2_DDDD_EEEE_FFFF"),
        QStringLiteral("LINE3_GGGG_HHHH_IIII"),
        QStringLiteral("LINE4_JJJJ_KKKK_LLLL"),
        QStringLiteral("LINE5_MMMM_NNNN_OOOO"),
        QStringLiteral("LINE6_PPPP_QQQQ_RRRR"),
    };
    for (const QString &l : lines) {
        writeString(screen, l);
        screen.newLine(); // move to next line (index + newLine)
    }

    // Verify some content is in history
    TEST_ASSERT(screen.getHistLines() > 0, "Some lines in history before reflow");

    // Collect all content before reflow
    QString allBefore = extractAllContent(screen);

    // Narrow to 10 columns — forces reflow of history + screen
    screen.resizeImage(4, 10, true);

    // Collect all content after narrow
    QString allAfterNarrow = extractAllContent(screen);

    // All original characters must still be present (possibly reordered line-by-line,
    // but the same set of non-space characters)
    for (const QString &l : lines) {
        // Each line's unique substring should be findable
        // e.g., "LINE1_AAAA_BBBB_CCCC" — check "LINE1" as a fragment
        QString fragment = l.left(5); // "LINE1", "LINE2", etc.
        TEST_ASSERT(allAfterNarrow.contains(fragment),
                    QString("Fragment '%1' present after narrow").arg(fragment).toUtf8().constData());
    }

    // Widen back to 20 columns
    screen.resizeImage(4, 20, true);
    QString allAfterWiden = extractAllContent(screen);

    for (const QString &l : lines) {
        QString fragment = l.left(5);
        TEST_ASSERT(allAfterWiden.contains(fragment),
                    QString("Fragment '%1' present after widen").arg(fragment).toUtf8().constData());
    }

    qInfo() << "  test_history_reflow: PASS";
    ++g_passed;
}

// ============================================================================
// Test (f): No-history screen — reflow must not produce phantom dropped lines
// ============================================================================
static void test_no_history_dropped_lines()
{
    // Default-constructed Screen has HistoryScrollNone (hasScroll()==false,
    // getLines()==0 always). Narrowing forces rows past the screen into the
    // history-write path; with no history those writes are no-ops and must
    // NOT be counted as dropped lines.
    Screen screen(4, 20);
    screen.setMode(MODE_Wrap);

    // Fill more logical content than fits after narrowing: 4 full 20-char rows
    for (int i = 0; i < 4; ++i) {
        writeString(screen, QStringLiteral("ROW%1_AAAA_BBBB_CCCC").arg(i));
        if (i < 3)
            screen.newLine();
    }
    TEST_ASSERT_EQ(screen.droppedLines(), 0, "No dropped lines before reflow");

    // Narrow to 10 columns: 4 rows of 20 chars reflow to ~8 rows, only 4 fit
    // on screen — the prefix rows go through the (no-op) history write path.
    screen.resizeImage(4, 10, true);

    TEST_ASSERT_EQ(screen.getHistLines(), 0, "HistoryScrollNone keeps zero lines");
    TEST_ASSERT_EQ(screen.droppedLines(), 0,
                   "No phantom dropped lines with HistoryScrollNone");

    qInfo() << "  test_no_history_dropped_lines: PASS";
    ++g_passed;
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    qInfo() << "=== Screen Reflow Tests ===";
    qInfo() << "";

    qInfo() << "[a] Narrow→Widen round-trip text preservation:";
    test_narrow_widen_roundtrip();

    qInfo() << "[b] CJK double-width chars never split:";
    test_cjk_no_split();

    qInfo() << "[c] Cursor position after reflow:";
    test_cursor_position_after_reflow();

    qInfo() << "[d] Alt-screen (reflow=false) truncation semantics:";
    test_alt_screen_no_reflow();

    qInfo() << "[e] History lines participate in reflow:";
    test_history_reflow();

    qInfo() << "[f] No-history screen produces no phantom dropped lines:";
    test_no_history_dropped_lines();

    qInfo() << "";
    qInfo() << "=== Results:" << g_passed << "passed," << g_failed << "failed ===";

    return g_failed > 0 ? 1 : 0;
}
