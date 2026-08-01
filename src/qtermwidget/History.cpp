// History.cpp — C++ port of qtermwidget/history.py
//
// Terminal scrollback buffer. Ported from the Python PySide6 version, which
// was itself converted from Konsole / QTermWidget (upstream History.cpp).
//
// Original copyright:
//   Copyright 1997,1998 by Lars Doelle <lars.doelle@on-line.de>

#include "History.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>

#ifdef Q_OS_WIN
// Windows 没有 mmap/unistd.h：文件映射用 Win32 API，低级 IO 用 CRT 的 <io.h>。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <QDebug>

#include "CharacterColor.h"

namespace Konsole {

namespace {

// POSIX 低级 IO 在 MSVC 上只有下划线前缀版本，且未定义 ssize_t，
// 这里统一包装供 HistoryFile 使用。
inline qint64 histSeek(int fd, int offset)
{
#ifdef Q_OS_WIN
    return ::_lseek(fd, offset, SEEK_SET);
#else
    return ::lseek(fd, offset, SEEK_SET);
#endif
}

inline qint64 histWrite(int fd, const void *buf, int len)
{
#ifdef Q_OS_WIN
    return ::_write(fd, buf, static_cast<unsigned int>(len));
#else
    return ::write(fd, buf, static_cast<size_t>(len));
#endif
}

inline qint64 histRead(int fd, void *buf, int len)
{
#ifdef Q_OS_WIN
    return ::_read(fd, buf, static_cast<unsigned int>(len));
#else
    return ::read(fd, buf, static_cast<size_t>(len));
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// Minimal BlockArray interface (module not yet ported — BlockArray.h/.cpp).
// These declarations mirror upstream QTermWidget's BlockArray.h so that the
// real header can replace them later without touching this file. When
// BlockArray.h is ported, delete this block and #include "BlockArray.h".
//
// INTERFACE ASSUMPTION (see report).
// ---------------------------------------------------------------------------
// 对应C++: #define ENTRIES (BlockSize - sizeof(size_t))
inline constexpr size_t HistoryBlockSize = (1 << 12);          // 4096 bytes
inline constexpr size_t ENTRIES = HistoryBlockSize - sizeof(size_t);

// 对应C++: struct Block { unsigned char data[ENTRIES]; size_t size; }
struct Block {
    unsigned char data[ENTRIES];
    size_t size;
};

// 对应C++: class BlockArray
class BlockArray {
public:
    BlockArray();
    ~BlockArray();
    size_t newBlock();
    Block *lastBlock() const;
    const Block *at(size_t i);
    bool has(size_t i) const;
    bool setHistorySize(size_t newsize);
    size_t getCurrent() const;
};
// ---------------------------------------------------------------------------

// HistoryFile
// -----------

// 对应C++: HistoryFile::HistoryFile()
HistoryFile::HistoryFile()
    : ion(-1), length(0), fileMap(nullptr), readWriteBalance(0)
{
    if (tmpFile.open()) {
        tmpFile.setAutoRemove(true);
        ion = tmpFile.handle();
    }
}

// 对应C++: HistoryFile::~HistoryFile() (Python 在 __del__ 里 unmap)
HistoryFile::~HistoryFile()
{
    if (fileMap)
        unmap();
}

// 对应C++: void HistoryFile::map()
void HistoryFile::map()
{
    Q_ASSERT(fileMap == nullptr);

    if (ion < 0 || length <= 0)
        return;

    // 对应C++: fileMap = (char*)mmap(nullptr, length, PROT_READ, MAP_PRIVATE, ion, 0);
#ifdef Q_OS_WIN
    // Win32 等价实现：CreateFileMapping + MapViewOfFile（只读视图）。
    void *mapResult = nullptr;
    const HANDLE file = reinterpret_cast<HANDLE>(::_get_osfhandle(ion));
    if (file != INVALID_HANDLE_VALUE) {
        const HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping) {
            mapResult = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(length));
            // 视图自身持有映射对象的引用，句柄可以立即关闭。
            ::CloseHandle(mapping);
        }
    }
    if (!mapResult) {
        // 映射失败，回退到 read/seek 组合
        readWriteBalance = 0;
        fileMap = nullptr;
        qWarning() << "mmap history file failed: win32 error" << ::GetLastError();
    } else {
        fileMap = static_cast<uchar *>(mapResult);
    }
#else
    void *mapResult = mmap(nullptr, static_cast<size_t>(length), PROT_READ, MAP_PRIVATE, ion, 0);
    if (mapResult == MAP_FAILED) {
        // mmap 失败,回退到 read/seek 组合
        readWriteBalance = 0;
        fileMap = nullptr;
        qWarning() << "mmap history file failed:" << strerror(errno);
    } else {
        fileMap = static_cast<uchar *>(mapResult);
    }
#endif
}

// 对应C++: void HistoryFile::unmap()
void HistoryFile::unmap()
{
    if (fileMap) {
#ifdef Q_OS_WIN
        ::UnmapViewOfFile(fileMap);
#else
        munmap(fileMap, static_cast<size_t>(length));
#endif
        fileMap = nullptr;
    }
}

// 对应C++: bool HistoryFile::isMapped() const
bool HistoryFile::isMapped() const
{
    return fileMap != nullptr;
}

// 对应C++: void HistoryFile::add(const unsigned char* bytes, int len)
void HistoryFile::add(const unsigned char *bytes, int len)
{
    if (fileMap)
        unmap();

    readWriteBalance++;

    if (histSeek(ion, length) == -1) {
        qWarning() << "HistoryFile::add lseek error:" << strerror(errno);
        return;
    }
    const qint64 written = histWrite(ion, bytes, len);
    if (written < 0) {
        qWarning() << "HistoryFile::add write error:" << strerror(errno);
        return;
    }
    length += static_cast<int>(written);
}

// 对应C++: void HistoryFile::get(unsigned char* bytes, int len, int loc)
void HistoryFile::get(unsigned char *bytes, int len, int loc)
{
    // Count get() calls vs add() calls.
    readWriteBalance--;
    if (!fileMap && readWriteBalance < MAP_THRESHOLD)
        map();

    if (loc < 0 || len < 0 || loc + len > length) {
        qWarning() << "getHist(...," << len << "," << loc << "): invalid args.";
        return;
    }

    if (fileMap) {
        // 内存映射读取
        std::memcpy(bytes, fileMap + loc, static_cast<size_t>(len));
    } else {
        // 传统 seek+read 方式
        if (histSeek(ion, loc) == -1) {
            qWarning() << "HistoryFile::get lseek error:" << strerror(errno);
            return;
        }
        const qint64 rc = histRead(ion, bytes, len);
        if (rc < 0)
            qWarning() << "HistoryFile::get read error:" << strerror(errno);
    }
}

// 对应C++: int HistoryFile::len() const
int HistoryFile::len() const
{
    return length;
}

// HistoryScroll
// -------------

// 对应C++: HistoryScroll::HistoryScroll(HistoryType *t)
HistoryScroll::HistoryScroll(HistoryType *t)
    : m_histType(t)
{
}

// 对应C++: HistoryScroll::~HistoryScroll()
HistoryScroll::~HistoryScroll()
{
    delete m_histType;
}

// 对应C++: Character HistoryScroll::getCell(int lineno, int colno) const
Character HistoryScroll::getCell(int lineno, int colno) const
{
    Character result;
    getCells(lineno, colno, 1, &result);
    return result;
}

// 对应C++: void HistoryScroll::addCellsVector(const TextLine& cells)
void HistoryScroll::addCellsVector(const TextLine &cells)
{
    addCells(cells.data(), static_cast<int>(cells.size()));
}

// HistoryScrollFile
// -----------------

// 对应C++: HistoryScrollFile::HistoryScrollFile(const QString &logFileName)
HistoryScrollFile::HistoryScrollFile(const QString &logFileName)
    : HistoryScroll(new HistoryTypeFile(logFileName)), m_logFileName(logFileName)
{
}

// 对应C++: int HistoryScrollFile::getLines() const
int HistoryScrollFile::getLines() const
{
    return index.len() / 4; // sizeof(int) = 4
}

// 对应C++: int HistoryScrollFile::getLineLen(int lineno) const
int HistoryScrollFile::getLineLen(int lineno) const
{
    return (startOfLine(lineno + 1) - startOfLine(lineno)) / static_cast<int>(sizeof(Character));
}

// 对应C++: bool HistoryScrollFile::isWrappedLine(int lineno) const
bool HistoryScrollFile::isWrappedLine(int lineno) const
{
    if (lineno >= 0 && lineno <= getLines()) {
        unsigned char flag = 0;
        lineflags.get(&flag, 1, lineno);
        return flag != 0;
    }
    return false;
}

// 对应C++: int HistoryScrollFile::startOfLine(int lineno) const
int HistoryScrollFile::startOfLine(int lineno) const
{
    if (lineno <= 0)
        return 0;
    if (lineno <= getLines()) {
        if (!index.isMapped())
            index.map();

        int res = 0;
        index.get(reinterpret_cast<unsigned char *>(&res), 4, (lineno - 1) * 4);
        return res;
    }
    return cells.len();
}

// 对应C++: void HistoryScrollFile::getCells(int lineno, int colno, int count, Character res[]) const
void HistoryScrollFile::getCells(int lineno, int colno, int count, Character res[]) const
{
    cells.get(reinterpret_cast<unsigned char *>(res),
              count * static_cast<int>(sizeof(Character)),
              startOfLine(lineno) + colno * static_cast<int>(sizeof(Character)));
}

// 对应C++: void HistoryScrollFile::addCells(const Character text[], int count)
void HistoryScrollFile::addCells(const Character text[], int count)
{
    cells.add(reinterpret_cast<const unsigned char *>(text), count * static_cast<int>(sizeof(Character)));
}

// 对应C++: void HistoryScrollFile::addLine(bool previousWrapped)
void HistoryScrollFile::addLine(bool previousWrapped)
{
    if (index.isMapped())
        index.unmap();

    int locn = cells.len();
    index.add(reinterpret_cast<unsigned char *>(&locn), 4);
    unsigned char flags = previousWrapped ? 0x01 : 0x00;
    lineflags.add(&flags, 1);
}

// HistoryScrollBuffer
// -------------------

// 对应C++: HistoryScrollBuffer::HistoryScrollBuffer(unsigned int maxLineCount)
HistoryScrollBuffer::HistoryScrollBuffer(unsigned int maxLineCount)
    : HistoryScroll(new HistoryTypeBuffer(maxLineCount))
{
    setMaxNbLines(maxLineCount);
}

// 对应C++: void HistoryScrollBuffer::addCellsVector(const QVector<Character>& cells)
void HistoryScrollBuffer::addCellsVector(const QVector<Character> &cells)
{
    _head++;
    if (_usedLines < _maxLineCount)
        _usedLines++;

    if (_head >= _maxLineCount)
        _head = 0;

    _historyBuffer[bufferIndex(_usedLines - 1)] = cells;
    _wrappedLine.setBit(bufferIndex(_usedLines - 1), false);
}

// 对应C++: void HistoryScrollBuffer::addCells(const Character a[], int count)
void HistoryScrollBuffer::addCells(const Character a[], int count)
{
    QVector<Character> newLine;
    newLine.resize(count);
    std::memcpy(newLine.data(), a, sizeof(Character) * static_cast<size_t>(count));

    addCellsVector(newLine);
}

// 对应C++: void HistoryScrollBuffer::addLine(bool previousWrapped)
void HistoryScrollBuffer::addLine(bool previousWrapped)
{
    _wrappedLine.setBit(bufferIndex(_usedLines - 1), previousWrapped);
}

// 对应C++: int HistoryScrollBuffer::getLines() const
int HistoryScrollBuffer::getLines() const
{
    return _usedLines;
}

// 对应C++: int HistoryScrollBuffer::getLineLen(int lineNumber) const
int HistoryScrollBuffer::getLineLen(int lineNumber) const
{
    Q_ASSERT(lineNumber >= 0 && lineNumber < _maxLineCount);

    if (lineNumber < _usedLines)
        return static_cast<int>(_historyBuffer[bufferIndex(lineNumber)].size());
    return 0;
}

// 对应C++: bool HistoryScrollBuffer::isWrappedLine(int lineNumber) const
bool HistoryScrollBuffer::isWrappedLine(int lineNumber) const
{
    Q_ASSERT(lineNumber >= 0 && lineNumber < _maxLineCount);

    if (lineNumber < _usedLines)
        return _wrappedLine.at(bufferIndex(lineNumber));
    return false;
}

// 对应C++: void HistoryScrollBuffer::getCells(int lineNumber, int startColumn, int count, Character buffer[]) const
void HistoryScrollBuffer::getCells(int lineNumber, int startColumn, int count, Character buffer[]) const
{
    if (count == 0)
        return;

    Q_ASSERT(lineNumber < _maxLineCount);

    if (lineNumber >= _usedLines) {
        for (int i = 0; i < count; i++)
            buffer[i] = Character();
        return;
    }

    const QVector<Character> &line = _historyBuffer[bufferIndex(lineNumber)];

    Q_ASSERT(startColumn >= 0);
    Q_ASSERT(static_cast<unsigned int>(startColumn) <= line.size() - static_cast<unsigned int>(count));

    std::memcpy(buffer, line.constData() + startColumn, sizeof(Character) * static_cast<size_t>(count));
}

// 对应C++: void HistoryScrollBuffer::setMaxNbLines(unsigned int lineCount)
void HistoryScrollBuffer::setMaxNbLines(unsigned int lineCount)
{
    // 把旧环形缓冲区里的数据复制到新缓冲区的连续位置 [0..copyLines)
    QVector<QVector<Character>> oldBuffer;
    oldBuffer.swap(_historyBuffer);

    _historyBuffer.resize(static_cast<int>(lineCount));

    int copyLines = qMin(_usedLines, static_cast<int>(lineCount));
    for (int i = 0; i < copyLines; i++) {
        int oldIdx = bufferIndex(i);
        if (oldIdx >= 0 && oldIdx < oldBuffer.size())
            _historyBuffer[i] = oldBuffer[oldIdx];
    }

    _usedLines = copyLines;
    _maxLineCount = static_cast<int>(lineCount);
    _head = (_usedLines == _maxLineCount) ? 0 : _usedLines - 1;

    _wrappedLine.resize(static_cast<int>(lineCount));
}

// 对应C++: int HistoryScrollBuffer::bufferIndex(int lineNumber) const
int HistoryScrollBuffer::bufferIndex(int lineNumber) const
{
    Q_ASSERT(lineNumber >= 0);
    Q_ASSERT(lineNumber < _maxLineCount);
    Q_ASSERT((_usedLines == _maxLineCount) || lineNumber <= _head);

    if (_usedLines == _maxLineCount)
        return (_head + lineNumber + 1) % _maxLineCount;
    return lineNumber;
}

// HistoryScrollNone
// -----------------

// 对应C++: HistoryScrollNone::HistoryScrollNone()
HistoryScrollNone::HistoryScrollNone()
    : HistoryScroll(new HistoryTypeNone())
{
}

// 对应C++: int HistoryScrollNone::getLineLen(int) const
int HistoryScrollNone::getLineLen(int) const
{
    return 0;
}

// 对应C++: bool HistoryScrollNone::isWrappedLine(int) const
bool HistoryScrollNone::isWrappedLine(int) const
{
    return false;
}

// 对应C++: void HistoryScrollNone::getCells(int, int, int, Character []) const
void HistoryScrollNone::getCells(int, int, int, Character[]) const
{
}

// 对应C++: void HistoryScrollNone::addCells(const Character [], int)
void HistoryScrollNone::addCells(const Character[], int)
{
}

// 对应C++: void HistoryScrollNone::addLine(bool)
void HistoryScrollNone::addLine(bool)
{
}

// HistoryScrollBlockArray
// -----------------------

// 对应C++: HistoryScrollBlockArray::HistoryScrollBlockArray(size_t size)
HistoryScrollBlockArray::HistoryScrollBlockArray(size_t size)
    : HistoryScroll(new HistoryTypeBlockArray(size)), m_blockArray(new BlockArray())
{
    m_blockArray->setHistorySize(size);
}

// 对应C++: HistoryScrollBlockArray::~HistoryScrollBlockArray()
HistoryScrollBlockArray::~HistoryScrollBlockArray()
{
    delete m_blockArray;
}

// 对应C++: int HistoryScrollBlockArray::getLines() const
int HistoryScrollBlockArray::getLines() const
{
    return m_lineLengths.count();
}

// 对应C++: int HistoryScrollBlockArray::getLineLen(int lineno) const
int HistoryScrollBlockArray::getLineLen(int lineno) const
{
    return m_lineLengths.value(lineno, 0);
}

// 对应C++: bool HistoryScrollBlockArray::isWrappedLine(int) const
bool HistoryScrollBlockArray::isWrappedLine(int) const
{
    return false;
}

// 对应C++: void HistoryScrollBlockArray::getCells(int lineno, int colno, int count, Character res[]) const
void HistoryScrollBlockArray::getCells(int lineno, int colno, int count, Character res[]) const
{
    if (!count)
        return;

    const Block *b = m_blockArray->at(static_cast<size_t>(lineno));
    if (!b) {
        for (int i = 0; i < count; i++)
            res[i] = Character();
        return;
    }

    Q_ASSERT(((colno + count) * static_cast<int>(sizeof(Character))) < static_cast<int>(ENTRIES));

    const Character *data = reinterpret_cast<const Character *>(b->data);
    for (int i = 0; i < count; i++)
        res[i] = data[colno + i];
}

// 对应C++: void HistoryScrollBlockArray::addCells(const Character a[], int count)
void HistoryScrollBlockArray::addCells(const Character a[], int count)
{
    Block *b = m_blockArray->lastBlock();
    if (!b)
        return;

    // put the bytes into the block
    std::memset(b->data, 0, ENTRIES);

    std::memcpy(b->data, a, sizeof(Character) * static_cast<size_t>(count));
    b->size = sizeof(Character) * static_cast<size_t>(count);

    size_t res = m_blockArray->newBlock();
    Q_ASSERT(res > 0);
    Q_UNUSED(res)

    m_lineLengths.insert(static_cast<qint64>(m_blockArray->getCurrent()), count);
}

// 对应C++: void HistoryScrollBlockArray::addLine(bool)
void HistoryScrollBlockArray::addLine(bool)
{
}

// ---------------------------------------------------------------------------
// Compact history
// ---------------------------------------------------------------------------

// CharacterFormat
// ---------------

// 对应C++: bool CharacterFormat::equalsFormat(const CharacterFormat &other) const
bool CharacterFormat::equalsFormat(const CharacterFormat &other) const
{
    return other.rendition == rendition && other.fgColor == fgColor && other.bgColor == bgColor;
}

// 对应C++: bool CharacterFormat::equalsFormat(const Character &other) const
bool CharacterFormat::equalsFormat(const Character &other) const
{
    return other.rendition == rendition && other.foregroundColor == fgColor && other.backgroundColor == bgColor;
}

// 对应C++: void CharacterFormat::setFormat(const Character& c)
void CharacterFormat::setFormat(const Character &c)
{
    rendition = c.rendition;
    fgColor = c.foregroundColor;
    bgColor = c.backgroundColor;
}

// CompactHistoryBlock
// -------------------

// 对应C++: CompactHistoryBlock::CompactHistoryBlock()
CompactHistoryBlock::CompactHistoryBlock()
    : blockLength(4096 * 64), // 256KB
      head(nullptr), tail(nullptr), allocCount(0)
{
    blockStart = new quint8[blockLength];
    head = blockStart;
    tail = blockStart;
}

// 对应C++: void* CompactHistoryBlock::allocate(size_t length)
void *CompactHistoryBlock::allocate(size_t len)
{
    Q_ASSERT(len > 0);
    if (tail - blockStart + len > blockLength)
        return nullptr;

    void *block = tail;
    tail += len;
    allocCount++;
    return block;
}

// 对应C++: bool CompactHistoryBlock::contains(void *addr)
bool CompactHistoryBlock::contains(void *addr) const
{
    return addr >= blockStart && addr < (blockStart + blockLength);
}

// 对应C++: void CompactHistoryBlock::deallocate()
void CompactHistoryBlock::deallocate()
{
    allocCount--;
    Q_ASSERT(allocCount >= 0);
}

// 对应C++: virtual CompactHistoryBlock::~CompactHistoryBlock()
CompactHistoryBlock::~CompactHistoryBlock()
{
    delete[] blockStart;
}

// CompactHistoryBlockList
// -----------------------

// 对应C++: CompactHistoryBlockList::~CompactHistoryBlockList()
CompactHistoryBlockList::~CompactHistoryBlockList()
{
    qDeleteAll(list);
    list.clear();
}

// 对应C++: void* CompactHistoryBlockList::allocate(size_t size)
void *CompactHistoryBlockList::allocate(size_t size)
{
    if (list.isEmpty() || list.last()->remaining() < size) {
        auto *newBlock = new CompactHistoryBlock();
        list.append(newBlock);
    }
    return list.last()->allocate(size);
}

// 对应C++: void CompactHistoryBlockList::deallocate(void *ptr)
void CompactHistoryBlockList::deallocate(void *ptr)
{
    Q_ASSERT(!list.isEmpty());

    int i = 0;
    CompactHistoryBlock *block = list.at(i);
    while (i < list.size() && !block->contains(ptr)) {
        i++;
        block = list.at(i);
    }

    Q_ASSERT(i < list.size());

    block->deallocate();

    if (!block->isInUse()) {
        list.removeAt(i);
        delete block;
    }
}

// CompactHistoryLine
// ------------------

// 对应C++: CompactHistoryLine::CompactHistoryLine(const TextLine& line, CompactHistoryBlockList& bList)
CompactHistoryLine::CompactHistoryLine(const TextLine &line, CompactHistoryBlockList &bList)
    : blockList(bList), formatArray(nullptr), text(nullptr), formatLength(0),
      length(static_cast<quint16>(line.size())), wrapped(false)
{
    if (!line.isEmpty()) {
        formatLength = 1;
        int k = 1;

        // count number of different formats in this textline
        Character c = line[0];
        while (k < length) {
            if (!line[k].equalsFormat(c)) {
                formatLength++; // format change detected
                c = line[k];
            }
            k++;
        }

        // allocate space for the format array
        formatArray = static_cast<CharacterFormat *>(blockList.allocate(sizeof(CharacterFormat) * formatLength));
        Q_ASSERT(formatArray != nullptr);

        // allocate space for the text
        text = static_cast<quint16 *>(blockList.allocate(sizeof(quint16) * line.size()));
        Q_ASSERT(text != nullptr);

        // record formats and their positions in the format array
        c = line[0];
        formatArray[0].setFormat(c);
        formatArray[0].startPos = 0;

        k = 1;   // position in text line
        int j = 1; // position in format array
        while (k < length && j < formatLength) {
            if (!line[k].equalsFormat(c)) {
                c = line[k];
                formatArray[j].setFormat(c);
                formatArray[j].startPos = static_cast<quint16>(k);
                j++;
            }
            k++;
        }

        // copy character values
        for (int i = 0; i < line.size(); i++)
            text[i] = line[i].character;
    }
}

// 对应C++: CompactHistoryLine::~CompactHistoryLine()
CompactHistoryLine::~CompactHistoryLine()
{
    if (length > 0) {
        blockList.deallocate(text);
        blockList.deallocate(formatArray);
    }
}

// 对应C++: void CompactHistoryLine::getCharacter(int index, Character &r)
void CompactHistoryLine::getCharacter(int index, Character &r) const
{
    Q_ASSERT(index < length);
    int formatPos = 0;
    while ((formatPos + 1) < formatLength && index >= formatArray[formatPos + 1].startPos)
        formatPos++;

    r.character = text[index];
    r.rendition = formatArray[formatPos].rendition;
    r.foregroundColor = formatArray[formatPos].fgColor;
    r.backgroundColor = formatArray[formatPos].bgColor;
}

// 对应C++: void CompactHistoryLine::getCharacters(Character* array, int length, int startColumn)
void CompactHistoryLine::getCharacters(Character *array, int len, int startColumn) const
{
    Q_ASSERT(startColumn >= 0 && len >= 0);
    Q_ASSERT(startColumn + len <= static_cast<int>(getLength()));

    for (int i = startColumn; i < len + startColumn; i++)
        getCharacter(i, array[i - startColumn]);
}

// CompactHistoryScroll
// --------------------

// 对应C++: CompactHistoryScroll::CompactHistoryScroll(unsigned int maxLineCount)
CompactHistoryScroll::CompactHistoryScroll(unsigned int maxLineCount)
    : HistoryScroll(new CompactHistoryType(maxLineCount))
{
    setMaxNbLines(maxLineCount);
}

// 对应C++: bool CompactHistoryScroll::hasDifferentColors(const TextLine& line) const
bool CompactHistoryScroll::hasDifferentColors(const TextLine &line) const
{
    if (line.size() < 2)
        return false;
    Character c = line[0];
    for (int k = 1; k < line.size(); k++) {
        if (!line[k].equalsFormat(c))
            return true;
    }
    return false;
}

// 对应C++: void CompactHistoryScroll::addCellsVector(const TextLine& cells)
void CompactHistoryScroll::addCellsVector(const TextLine &cells)
{
    auto *line = new CompactHistoryLine(cells, blockList);

    if (lines.size() > static_cast<int>(_maxLineCount))
        delete lines.takeFirst();

    lines.append(line);
}

// 对应C++: void CompactHistoryScroll::addCells(const Character a[], int count)
void CompactHistoryScroll::addCells(const Character a[], int count)
{
    TextLine newLine;
    newLine.resize(count);
    std::memcpy(newLine.data(), a, sizeof(Character) * static_cast<size_t>(count));

    addCellsVector(newLine);
}

// 对应C++: void CompactHistoryScroll::addLine(bool previousWrapped)
void CompactHistoryScroll::addLine(bool previousWrapped)
{
    if (!lines.isEmpty())
        lines.last()->setWrapped(previousWrapped);
}

// 对应C++: int CompactHistoryScroll::getLines() const
int CompactHistoryScroll::getLines() const
{
    return lines.size();
}

// 对应C++: int CompactHistoryScroll::getLineLen(int lineNumber) const
int CompactHistoryScroll::getLineLen(int lineNumber) const
{
    Q_ASSERT(lineNumber >= 0 && lineNumber < lines.size());
    CompactHistoryLine *line = lines[lineNumber];
    return static_cast<int>(line->getLength());
}

// 对应C++: void CompactHistoryScroll::getCells(int lineNumber, int startColumn, int count, Character buffer[]) const
void CompactHistoryScroll::getCells(int lineNumber, int startColumn, int count, Character buffer[]) const
{
    if (count == 0)
        return;
    Q_ASSERT(lineNumber < lines.size());
    CompactHistoryLine *line = lines[lineNumber];
    Q_ASSERT(startColumn >= 0);
    Q_ASSERT(static_cast<unsigned int>(startColumn) <= line->getLength() - static_cast<unsigned int>(count));
    line->getCharacters(buffer, count, startColumn);
}

// 对应C++: void CompactHistoryScroll::setMaxNbLines(unsigned int lineCount)
void CompactHistoryScroll::setMaxNbLines(unsigned int lineCount)
{
    _maxLineCount = lineCount;

    while (lines.size() > static_cast<int>(lineCount))
        delete lines.takeFirst();
}

// 对应C++: bool CompactHistoryScroll::isWrappedLine(int lineNumber) const
bool CompactHistoryScroll::isWrappedLine(int lineNumber) const
{
    Q_ASSERT(lineNumber < lines.size());
    return lines[lineNumber]->isWrapped();
}

// ---------------------------------------------------------------------------
// Retention policies
// ---------------------------------------------------------------------------

// HistoryTypeNone
// 对应C++: HistoryScroll* HistoryTypeNone::scroll(HistoryScroll *old) const
HistoryScroll *HistoryTypeNone::scroll(HistoryScroll *old) const
{
    delete old;
    return new HistoryScrollNone();
}

// HistoryTypeBlockArray
// 对应C++: HistoryTypeBlockArray::HistoryTypeBlockArray(size_t size)
HistoryTypeBlockArray::HistoryTypeBlockArray(size_t size)
    : m_size(size)
{
}

// 对应C++: HistoryScroll* HistoryTypeBlockArray::scroll(HistoryScroll *old) const
HistoryScroll *HistoryTypeBlockArray::scroll(HistoryScroll *old) const
{
    delete old;
    return new HistoryScrollBlockArray(m_size);
}

// HistoryTypeFile
// 对应C++: HistoryTypeFile::HistoryTypeFile(const QString& fileName)
HistoryTypeFile::HistoryTypeFile(const QString &fileName)
    : m_fileName(fileName)
{
}

// 对应C++: HistoryScroll* HistoryTypeFile::scroll(HistoryScroll *old) const
HistoryScroll *HistoryTypeFile::scroll(HistoryScroll *old) const
{
    if (dynamic_cast<HistoryScrollFile *>(old) != nullptr)
        return old; // Unchanged.

    auto *newScroll = new HistoryScrollFile(m_fileName);

    if (old != nullptr) {
        // copy old history to new scroll
        int lines = old->getLines();
        int startLine = 0;
        if (lines > old->getType().maximumLineCount() && old->getType().maximumLineCount() > 0)
            startLine = lines - old->getType().maximumLineCount();

        int lineLength;
        auto *line = new Character[LINE_SIZE];
        for (int i = startLine; i < lines; i++) {
            lineLength = old->getLineLen(i);
            if (lineLength > LINE_SIZE)
                lineLength = LINE_SIZE;
            old->getCells(i, 0, lineLength, line);
            newScroll->addCells(line, lineLength);
            newScroll->addLine(old->isWrappedLine(i));
        }
        delete[] line;

        delete old;
    }

    return newScroll;
}

// HistoryTypeBuffer
// 对应C++: HistoryTypeBuffer::HistoryTypeBuffer(unsigned int nbLines)
HistoryTypeBuffer::HistoryTypeBuffer(unsigned int nbLines)
    : m_nbLines(static_cast<int>(nbLines))
{
}

// 对应C++: HistoryScroll* HistoryTypeBuffer::scroll(HistoryScroll *old) const
HistoryScroll *HistoryTypeBuffer::scroll(HistoryScroll *old) const
{
    if (old != nullptr) {
        auto *oldBuffer = dynamic_cast<HistoryScrollBuffer *>(old);
        if (oldBuffer != nullptr) {
            oldBuffer->setMaxNbLines(static_cast<unsigned int>(m_nbLines));
            return oldBuffer;
        }

        auto *newScroll = new HistoryScrollBuffer(static_cast<unsigned int>(m_nbLines));
        int lines = old->getLines();
        int startLine = 0;
        if (lines > m_nbLines)
            startLine = lines - m_nbLines;

        int lineLength;
        auto *line = new Character[LINE_SIZE];
        for (int i = startLine; i < lines; i++) {
            lineLength = old->getLineLen(i);
            if (lineLength > LINE_SIZE)
                lineLength = LINE_SIZE;
            old->getCells(i, 0, lineLength, line);
            newScroll->addCells(line, lineLength);
            newScroll->addLine(old->isWrappedLine(i));
        }
        delete[] line;

        delete old;
        return newScroll;
    }
    return new HistoryScrollBuffer(static_cast<unsigned int>(m_nbLines));
}

// CompactHistoryType
// 对应C++: CompactHistoryType::CompactHistoryType(unsigned int nbLines)
CompactHistoryType::CompactHistoryType(unsigned int nbLines)
    : m_nbLines(static_cast<int>(nbLines))
{
}

// 对应C++: HistoryScroll* CompactHistoryType::scroll(HistoryScroll *old) const
HistoryScroll *CompactHistoryType::scroll(HistoryScroll *old) const
{
    if (old != nullptr) {
        auto *oldBuffer = dynamic_cast<CompactHistoryScroll *>(old);
        if (oldBuffer != nullptr) {
            oldBuffer->setMaxNbLines(static_cast<unsigned int>(m_nbLines));
            return oldBuffer;
        }
        delete old;
    }
    return new CompactHistoryScroll(static_cast<unsigned int>(m_nbLines));
}

} // namespace Konsole
