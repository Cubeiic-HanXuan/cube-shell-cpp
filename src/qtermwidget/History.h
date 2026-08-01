#pragma once

// History.h — C++ port of qtermwidget/history.py
//
// Terminal scrollback buffer. Ported from the Python PySide6 version, which
// was itself converted from Konsole / QTermWidget (upstream History.h).
//
// Original copyright:
//   Copyright 1997,1998 by Lars Doelle <lars.doelle@on-line.de>

#include <QBitArray>
#include <QList>
#include <QString>
#include <QTemporaryFile>
#include <QVector>

#include "Character.h"

namespace Konsole {

// Forward declaration of the (separately ported) BlockArray storage.
// 对应C++: #include "BlockArray.h"
class BlockArray;
class HistoryScroll;

// 对应C++: #define LINE_SIZE 1024
inline constexpr int LINE_SIZE = 1024;
// 对应C++: #define MAP_THRESHOLD -1000
// 当读写平衡低于此阈值时自动 mmap 文件
inline constexpr int MAP_THRESHOLD = -1000;

// A single line of text in the scrollback.
// 对应C++: typedef QVector<Character> TextLine
using TextLine = QVector<Character>;

// An extendable buffer based on a temporary file, optionally memory-mapped
// for fast reads. 对应C++: class HistoryFile
class HistoryFile {
public:
    // 对应C++: HistoryFile::HistoryFile()
    HistoryFile();
    // 对应C++: virtual ~HistoryFile()
    virtual ~HistoryFile();

    HistoryFile(const HistoryFile &) = delete;
    HistoryFile &operator=(const HistoryFile &) = delete;

    // 对应C++: virtual void add(const unsigned char* bytes, int len)
    virtual void add(const unsigned char *bytes, int len);
    // 对应C++: virtual void get(unsigned char* bytes, int len, int loc)
    virtual void get(unsigned char *bytes, int len, int loc);
    // 对应C++: virtual int len() const
    virtual int len() const;

    // 对应C++: void map()
    void map();
    // 对应C++: void unmap()
    void unmap();
    // 对应C++: bool isMapped() const
    bool isMapped() const;

private:
    int ion = -1;
    int length = 0;
    // Memory-mapped view of the file (read only). nullptr when not mapped.
    // 对应C++: char* fileMap (Python: mmap.mmap)
    uchar *fileMap = nullptr;
    // Balance of add() vs get() calls; when it drops below MAP_THRESHOLD the
    // file is memory mapped to speed up repeated reads.
    int readWriteBalance = 0;

    QTemporaryFile tmpFile;
};

// Abstract base for the different scrollback retention policies.
// 对应C++: class HistoryType
class HistoryType {
public:
    // 对应C++: HistoryType::HistoryType()
    HistoryType() = default;
    // 对应C++: virtual ~HistoryType()
    virtual ~HistoryType() = default;

    // 对应C++: virtual bool isEnabled() const = 0
    virtual bool isEnabled() const = 0;
    // Returns true if there is no limit on the number of lines stored.
    // 对应C++: bool isUnlimited() const
    bool isUnlimited() const { return maximumLineCount() == 0; }
    // 对应C++: virtual int maximumLineCount() const = 0
    virtual int maximumLineCount() const = 0;

    // 对应C++: virtual HistoryScroll* scroll(HistoryScroll *) const = 0
    virtual HistoryScroll *scroll(HistoryScroll *old) const = 0;
};

// Abstract base for a concrete scrollback storage.
// 对应C++: class HistoryScroll
class HistoryScroll {
public:
    // 对应C++: HistoryScroll::HistoryScroll(HistoryType*)
    explicit HistoryScroll(HistoryType *type);
    // 对应C++: virtual ~HistoryScroll()
    virtual ~HistoryScroll();

    HistoryScroll(const HistoryScroll &) = delete;
    HistoryScroll &operator=(const HistoryScroll &) = delete;

    // 对应C++: virtual bool hasScroll() const
    virtual bool hasScroll() const { return true; }

    // 对应C++: virtual int getLines() const = 0
    virtual int getLines() const = 0;
    // 对应C++: virtual int getLineLen(int lineno) const = 0
    virtual int getLineLen(int lineno) const = 0;
    // 对应C++: virtual void getCells(int lineno, int colno, int count, Character res[]) const = 0
    virtual void getCells(int lineno, int colno, int count, Character res[]) const = 0;
    // 对应C++: virtual bool isWrappedLine(int lineno) const = 0
    virtual bool isWrappedLine(int lineno) const = 0;

    // Convenience single-cell access (kept for backward compatibility).
    // 对应C++: Character getCell(int lineno, int colno) const
    Character getCell(int lineno, int colno) const;

    // 对应C++: virtual void addCells(const Character a[], int count) = 0
    virtual void addCells(const Character a[], int count) = 0;
    // 对应C++: virtual void addCellsVector(const TextLine& cells)
    virtual void addCellsVector(const TextLine &cells);
    // 对应C++: virtual void addLine(bool previousWrapped = false) = 0
    virtual void addLine(bool previousWrapped = false) = 0;

    // The retention policy owning this scroll. Owned by this object.
    // 对应C++: const HistoryType& getType() const
    const HistoryType &getType() const { return *m_histType; }

private:
    HistoryType *m_histType;
};

// File-backed scrollback (unlimited length), e.g. a log file.
// 对应C++: class HistoryScrollFile : public HistoryScroll
class HistoryScrollFile : public HistoryScroll {
public:
    // 对应C++: HistoryScrollFile::HistoryScrollFile(const QString &logFileName)
    explicit HistoryScrollFile(const QString &logFileName);
    ~HistoryScrollFile() override = default;

    // 对应C++: int getLines() const
    int getLines() const override;
    // 对应C++: int getLineLen(int lineno) const
    int getLineLen(int lineno) const override;
    // 对应C++: bool isWrappedLine(int lineno) const
    bool isWrappedLine(int lineno) const override;
    // 对应C++: void getCells(int lineno, int colno, int count, Character res[]) const
    void getCells(int lineno, int colno, int count, Character res[]) const override;
    // 对应C++: void addCells(const Character text[], int count)
    void addCells(const Character text[], int count) override;
    // 对应C++: void addLine(bool previousWrapped = false)
    void addLine(bool previousWrapped = false) override;

private:
    // 对应C++: int startOfLine(int lineno) const
    int startOfLine(int lineno) const;

    QString m_logFileName;
    // Accessors are const in the public API but lazily map / page-in the file,
    // so the backing stores are mutable (mirrors upstream Konsole).
    mutable HistoryFile index;    // lines Row index
    mutable HistoryFile cells;    // text
    mutable HistoryFile lineflags; // flags
};

// In-memory fixed-size circular buffer scrollback.
// 对应C++: class HistoryScrollBuffer : public HistoryScroll
class HistoryScrollBuffer : public HistoryScroll {
public:
    // 对应C++: HistoryScrollBuffer::HistoryScrollBuffer(unsigned int maxLineCount = 1000)
    explicit HistoryScrollBuffer(unsigned int maxLineCount = 1000);
    ~HistoryScrollBuffer() override = default;

    // 对应C++: void addCellsVector(const QVector<Character>& cells)
    void addCellsVector(const QVector<Character> &cells) override;
    // 对应C++: void addCells(const Character a[], int count)
    void addCells(const Character a[], int count) override;
    // 对应C++: void addLine(bool previousWrapped = false)
    void addLine(bool previousWrapped = false) override;

    // 对应C++: int getLines() const
    int getLines() const override;
    // 对应C++: int getLineLen(int lineNumber) const
    int getLineLen(int lineNumber) const override;
    // 对应C++: bool isWrappedLine(int lineNumber) const
    bool isWrappedLine(int lineNumber) const override;
    // 对应C++: void getCells(int lineNumber, int startColumn, int count, Character buffer[]) const
    void getCells(int lineNumber, int startColumn, int count, Character buffer[]) const override;

    // 对应C++: void setMaxNbLines(unsigned int lineCount)
    void setMaxNbLines(unsigned int lineCount);
    // 对应C++: unsigned int maxNbLines() const
    unsigned int maxNbLines() const { return _maxLineCount; }

private:
    // 对应C++: int bufferIndex(int lineNumber) const
    int bufferIndex(int lineNumber) const;

    QVector<QVector<Character>> _historyBuffer;
    QBitArray _wrappedLine;
    int _maxLineCount = 0;
    int _usedLines = 0;
    int _head = 0;
};

// Scrollback that keeps nothing.
// 对应C++: class HistoryScrollNone : public HistoryScroll
class HistoryScrollNone : public HistoryScroll {
public:
    // 对应C++: HistoryScrollNone::HistoryScrollNone()
    HistoryScrollNone();
    ~HistoryScrollNone() override = default;

    // 对应C++: bool hasScroll() const
    bool hasScroll() const override { return false; }

    // 对应C++: int getLines() const
    int getLines() const override { return 0; }
    // 对应C++: int getLineLen(int) const
    int getLineLen(int lineno) const override;
    // 对应C++: bool isWrappedLine(int) const
    bool isWrappedLine(int lineno) const override;
    // 对应C++: void getCells(int, int, int, Character []) const
    void getCells(int lineno, int colno, int count, Character res[]) const override;
    // 对应C++: void addCells(const Character [], int)
    void addCells(const Character a[], int count) override;
    // 对应C++: void addLine(bool)
    void addLine(bool previousWrapped = false) override;
};

// Scrollback backed by a BlockArray of fixed-size blocks.
// 对应C++: class HistoryScrollBlockArray : public HistoryScroll
class HistoryScrollBlockArray : public HistoryScroll {
public:
    // 对应C++: HistoryScrollBlockArray::HistoryScrollBlockArray(size_t size)
    explicit HistoryScrollBlockArray(size_t size);
    ~HistoryScrollBlockArray() override;

    // 对应C++: int getLines() const
    int getLines() const override;
    // 对应C++: int getLineLen(int lineno) const
    int getLineLen(int lineno) const override;
    // 对应C++: bool isWrappedLine(int) const
    bool isWrappedLine(int lineno) const override;
    // 对应C++: void getCells(int lineno, int colno, int count, Character res[]) const
    void getCells(int lineno, int colno, int count, Character res[]) const override;
    // 对应C++: void addCells(const Character a[], int count)
    void addCells(const Character a[], int count) override;
    // 对应C++: void addLine(bool)
    void addLine(bool previousWrapped = false) override;

private:
    BlockArray *m_blockArray;
    // Per-line lengths keyed by the BlockArray "current" index at the time the
    // line was committed. Mirrors Python's dict; a QHash keeps ordering-free
    // lookup semantics. 对应C++: (no direct member; Python uses m_lineLengths)
    QHash<qint64, int> m_lineLengths;
};

// ---------------------------------------------------------------------------
// Compact history storage (format-run compressed).
// ---------------------------------------------------------------------------

// A single run of identical (colors+rendition) formatting within a line.
// 对应C++: class CharacterFormat
class CharacterFormat {
public:
    // 对应C++: bool equalsFormat(const CharacterFormat &other) const
    bool equalsFormat(const CharacterFormat &other) const;
    // 对应C++: bool equalsFormat(const Character &other) const
    bool equalsFormat(const Character &other) const;
    // 对应C++: void setFormat(const Character& c)
    void setFormat(const Character &c);

    CharacterColor fgColor;
    CharacterColor bgColor;
    quint16 startPos = 0;
    // 对应C++: quint16 rendition (Python 注释里写 8 位,实际 Character.rendition 是 16 位)
    quint16 rendition = 0;
};

// A fixed-size (256KB) allocation arena for compact history lines.
// 对应C++: class CompactHistoryBlock
class CompactHistoryBlock {
public:
    // 对应C++: CompactHistoryBlock::CompactHistoryBlock()
    CompactHistoryBlock();
    // 对应C++: virtual ~CompactHistoryBlock()
    virtual ~CompactHistoryBlock();

    // 对应C++: virtual unsigned int remaining()
    unsigned int remaining() const { return blockLength - static_cast<unsigned int>(tail - blockStart); }
    // 对应C++: virtual unsigned length()
    unsigned int length() const { return blockLength; }
    // 对应C++: virtual void* allocate(size_t length)
    void *allocate(size_t length);
    // 对应C++: virtual bool contains(void *addr)
    bool contains(void *addr) const;
    // 对应C++: virtual void deallocate()
    void deallocate();
    // 对应C++: virtual bool isInUse()
    bool isInUse() const { return allocCount != 0; }

private:
    unsigned int blockLength;
    quint8 *blockStart;
    quint8 *head;
    quint8 *tail;
    int allocCount;
};

// A list of CompactHistoryBlock arenas; allocates from the last block.
// 对应C++: class CompactHistoryBlockList
class CompactHistoryBlockList {
public:
    // 对应C++: CompactHistoryBlockList::CompactHistoryBlockList()
    CompactHistoryBlockList() = default;
    // 对应C++: CompactHistoryBlockList::~CompactHistoryBlockList()
    ~CompactHistoryBlockList();

    // 对应C++: void* allocate(size_t size)
    void *allocate(size_t size);
    // 对应C++: void deallocate(void *)
    void deallocate(void *ptr);
    // 对应C++: int length()
    int length() const { return list.size(); }

private:
    QList<CompactHistoryBlock *> list;
};

// One compressed scrollback line: text plus an array of format runs.
// 对应C++: class CompactHistoryLine
class CompactHistoryLine {
public:
    // 对应C++: CompactHistoryLine(const TextLine&, CompactHistoryBlockList& blockList)
    CompactHistoryLine(const TextLine &line, CompactHistoryBlockList &blockList);
    // 对应C++: virtual ~CompactHistoryLine()
    virtual ~CompactHistoryLine();

    // 对应C++: void getCharacter(int index, Character &r)
    void getCharacter(int index, Character &r) const;
    // 对应C++: void getCharacters(Character* array, int length, int startColumn)
    void getCharacters(Character *array, int length, int startColumn) const;
    // 对应C++: virtual bool isWrapped() const
    bool isWrapped() const { return wrapped; }
    // 对应C++: virtual void setWrapped(bool isWrapped)
    void setWrapped(bool isWrapped) { wrapped = isWrapped; }
    // 对应C++: virtual unsigned int getLength() const
    unsigned int getLength() const { return length; }

private:
    CompactHistoryBlockList &blockList;
    CharacterFormat *formatArray = nullptr;
    quint16 *text = nullptr;
    quint16 formatLength = 0;
    quint16 length = 0;
    bool wrapped = false;
};

// Format-compressed scrollback.
// 对应C++: class CompactHistoryScroll : public HistoryScroll
class CompactHistoryScroll : public HistoryScroll {
public:
    // 对应C++: CompactHistoryScroll(unsigned int maxLineCount = 1000)
    explicit CompactHistoryScroll(unsigned int maxLineCount = 1000);
    ~CompactHistoryScroll() override = default;

    // 对应C++: void addCellsVector(const TextLine& cells)
    void addCellsVector(const TextLine &cells) override;
    // 对应C++: void addCells(const Character a[], int count)
    void addCells(const Character a[], int count) override;
    // 对应C++: void addLine(bool previousWrapped)
    void addLine(bool previousWrapped = false) override;

    // 对应C++: int getLines() const
    int getLines() const override;
    // 对应C++: int getLineLen(int lineNumber) const
    int getLineLen(int lineNumber) const override;
    // 对应C++: void getCells(int lineNumber, int startColumn, int count, Character buffer[]) const
    void getCells(int lineNumber, int startColumn, int count, Character buffer[]) const override;
    // 对应C++: bool isWrappedLine(int lineNumber) const
    bool isWrappedLine(int lineNumber) const override;

    // 对应C++: void setMaxNbLines(unsigned int lineCount)
    void setMaxNbLines(unsigned int lineCount);
    // 对应C++: unsigned int maxNbLines() const
    unsigned int maxNbLines() const { return _maxLineCount; }

private:
    bool hasDifferentColors(const TextLine &line) const;

    QList<CompactHistoryLine *> lines;
    CompactHistoryBlockList blockList;
    unsigned int _maxLineCount = 0;
};

// ---------------------------------------------------------------------------
// Concrete retention policies.
// ---------------------------------------------------------------------------

// 对应C++: class HistoryTypeNone : public HistoryType
class HistoryTypeNone : public HistoryType {
public:
    // 对应C++: HistoryTypeNone::HistoryTypeNone()
    HistoryTypeNone() = default;

    // 对应C++: bool isEnabled() const
    bool isEnabled() const override { return false; }
    // 对应C++: int maximumLineCount() const
    int maximumLineCount() const override { return 0; }
    // 对应C++: HistoryScroll* scroll(HistoryScroll *old) const
    HistoryScroll *scroll(HistoryScroll *old) const override;
};

// 对应C++: class HistoryTypeBlockArray : public HistoryType
class HistoryTypeBlockArray : public HistoryType {
public:
    // 对应C++: HistoryTypeBlockArray::HistoryTypeBlockArray(size_t size)
    explicit HistoryTypeBlockArray(size_t size);

    // 对应C++: bool isEnabled() const
    bool isEnabled() const override { return true; }
    // 对应C++: int maximumLineCount() const
    int maximumLineCount() const override { return static_cast<int>(m_size); }
    // 对应C++: HistoryScroll* scroll(HistoryScroll *old) const
    HistoryScroll *scroll(HistoryScroll *old) const override;

protected:
    size_t m_size;
};

// 对应C++: class HistoryTypeFile : public HistoryType
class HistoryTypeFile : public HistoryType {
public:
    // 对应C++: HistoryTypeFile::HistoryTypeFile(const QString& fileName = QString())
    explicit HistoryTypeFile(const QString &fileName = QString());

    // 对应C++: bool isEnabled() const
    bool isEnabled() const override { return true; }
    // 对应C++: const QString& getFileName() const
    const QString &getFileName() const { return m_fileName; }
    // 对应C++: int maximumLineCount() const
    int maximumLineCount() const override { return 0; } // unlimited
    // 对应C++: HistoryScroll* scroll(HistoryScroll *old) const
    HistoryScroll *scroll(HistoryScroll *old) const override;

protected:
    QString m_fileName;
};

// 对应C++: class HistoryTypeBuffer : public HistoryType
class HistoryTypeBuffer : public HistoryType {
public:
    // 对应C++: HistoryTypeBuffer::HistoryTypeBuffer(unsigned int nbLines)
    explicit HistoryTypeBuffer(unsigned int nbLines);

    // 对应C++: bool isEnabled() const
    bool isEnabled() const override { return true; }
    // 对应C++: int maximumLineCount() const
    int maximumLineCount() const override { return m_nbLines; }
    // 对应C++: HistoryScroll* scroll(HistoryScroll *old) const
    HistoryScroll *scroll(HistoryScroll *old) const override;

protected:
    int m_nbLines;
};

// 对应C++: class CompactHistoryType : public HistoryType
class CompactHistoryType : public HistoryType {
public:
    // 对应C++: CompactHistoryType::CompactHistoryType(unsigned int nbLines)
    explicit CompactHistoryType(unsigned int nbLines);

    // 对应C++: bool isEnabled() const
    bool isEnabled() const override { return true; }
    // 对应C++: int maximumLineCount() const
    int maximumLineCount() const override { return m_nbLines; }
    // 对应C++: HistoryScroll* scroll(HistoryScroll *old) const
    HistoryScroll *scroll(HistoryScroll *old) const override;

protected:
    int m_nbLines;
};

} // namespace Konsole
