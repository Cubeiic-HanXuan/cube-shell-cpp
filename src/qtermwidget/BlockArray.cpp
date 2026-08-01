// BlockArray.cpp — C++ port of qtermwidget/block_array.py
//
// See BlockArray.h for the overall description. The original used an mmap()ed
// temporary file; here we use a QFile-backed temp file with explicit
// seek/read/write, which is portable and needs no raw mmap. The ring-buffer
// index arithmetic follows upstream QTermWidget 2.2.0 BlockArray.cpp.
//
// Original copyright:
//   Copyright (C) 2000 by Stephan Kulow <coolo@kde.org>
//   Rewritten for QT4 by e_k <e_k at users.sourceforge.net>, Copyright (C)2008

#include "BlockArray.h"

#include <QByteArray>
#include <QDebug>
#include <QTemporaryFile>

// getpagesize() is POSIX-only; 4096 is a safe common page size for the
// blocksize rounding below (it only affects the in-file stride, not the wire
// format).
static int pageSize()
{
    return 4096;
}

namespace Konsole {

// 对应C++: BlockArray::BlockArray()
//    : size(0), current(static_cast<size_t>(-1)), index(static_cast<size_t>(-1)),
//      lastmap(nullptr), lastmap_index(static_cast<size_t>(-1)),
//      lastblock(nullptr), ion(-1), length(0)
BlockArray::BlockArray()
{
    // lastmap_index = static_cast<size_t>(-1);  (set by member initializers)

    // 对应C++:
    //   if (blocksize == 0)
    //       blocksize = ((sizeof(Block) / getpagesize()) + 1) * getpagesize();
    // sizeof(Block) == QTERMWIDGET_BLOCKSIZE, so blocksize ends up being one
    // page (4096) for the common 4096-byte page size.
    static size_t staticBlocksize = 0;
    if (staticBlocksize == 0) {
        staticBlocksize = ((sizeof(Block) / pageSize()) + 1) * pageSize();
    }
    blocksize = staticBlocksize;
}

// 对应C++: BlockArray::~BlockArray()
BlockArray::~BlockArray()
{
    setHistorySize(0);
    delete lastblock;
    delete lastmap;
}

// 对应C++: size_t BlockArray::append(Block * block)
size_t BlockArray::append(Block *block)
{
    if (!size) {
        return BLOCKARRAY_INVALID;
    }

    // 对应C++: ++current;
    // Python wrapped current with `(current + 1) % size` and special-cased the
    // INVALID sentinel; upstream relies on unsigned wrap of size_t(-1) -> 0.
    // We reproduce both behaviours explicitly.
    if (current == BLOCKARRAY_INVALID) {
        current = 0;
    } else {
        current = (current + 1) % size;
    }

    // Serialize the block: payload bytes then the 8-byte size at the tail,
    // exactly matching the on-disk layout of struct Block.
    QByteArray raw(static_cast<qsizetype>(blocksize), '\0');
    // block->data is a fixed ENTRIES array; copy the whole payload area.
    memcpy(raw.data(), block->data, ENTRIES);
    quint64 sz = static_cast<quint64>(block->size);
    // little-endian 8-byte size, matching the Python to_bytes(8, 'little').
    for (int b = 0; b < 8; ++b) {
        raw[static_cast<qsizetype>(blocksize) - 8 + b] =
            static_cast<char>((sz >> (8 * b)) & 0xFF);
    }

    if (!writeRawBlock(static_cast<qint64>(current), raw)) {
        qWarning() << "HistoryBuffer::add error";
        setHistorySize(0);
        delete block;
        return BLOCKARRAY_INVALID;
    }

    delete block; // 对应C++: delete block;

    // 对应C++: length++;
    length++;
    if (length > size) {
        length = size;
    }

    // 对应C++: ++index;
    if (index == BLOCKARRAY_INVALID) {
        index = 0;
    } else {
        index++;
    }

    return current;
}

// 对应C++: size_t BlockArray::newBlock()
size_t BlockArray::newBlock()
{
    if (!size) {
        return BLOCKARRAY_INVALID;
    }
    append(lastblock);
    lastblock = new Block;

    return index + 1;
}

// 对应C++: Block * BlockArray::lastBlock() const
Block *BlockArray::lastBlock() const
{
    return lastblock;
}

// 对应C++: bool BlockArray::has(size_t i) const
bool BlockArray::has(size_t i) const
{
    if (i == index + 1) {
        return true;
    }
    if (i > index) {
        return false;
    }
    if (index - i >= length) {
        return false;
    }
    return true;
}

// 对应C++: void BlockArray::unmap()
void BlockArray::unmap()
{
    delete lastmap;
    lastmap = nullptr;
    lastmap_index = BLOCKARRAY_INVALID;
}

// 对应C++: const Block * BlockArray::at(size_t i)
const Block *BlockArray::at(size_t i)
{
    if (i == index + 1) {
        return lastblock;
    }

    if (i == lastmap_index) {
        return lastmap;
    }

    if (i > index) {
        qWarning() << "BlockArray::at() i > index";
        return nullptr;
    }

    // 对应C++:
    //   size_t j = i; // (current - (index - i) + (index/size+1)*size) % size ;
    // The Python code tried to approximate this with a buggy two-branch
    // heuristic; we use the exact upstream formula instead.
    const size_t j = (current - (index - i) + (index / size + 1) * size) % size;

    if (j >= size) {
        return nullptr;
    }

    unmap();

    QByteArray raw;
    if (!readRawBlock(static_cast<qint64>(j), raw) ||
        raw.size() < static_cast<qsizetype>(blocksize)) {
        qWarning() << "mmap error";
        return nullptr;
    }

    Block *block = new Block;
    memcpy(block->data, raw.constData(), ENTRIES);
    // trailing 8-byte little-endian size
    quint64 sz = 0;
    for (int b = 0; b < 8; ++b) {
        sz |= static_cast<quint64>(static_cast<unsigned char>(
                  raw[static_cast<qsizetype>(blocksize) - 8 + b]))
              << (8 * b);
    }
    block->size = static_cast<size_t>(sz);

    lastmap = block;
    lastmap_index = i;

    return block;
}

// 对应C++: size_t BlockArray::len() const
size_t BlockArray::len() const
{
    return length;
}

// 对应C++: size_t BlockArray::getCurrent() const
size_t BlockArray::getCurrent() const
{
    return current;
}

// 对应C++: bool BlockArray::setSize(size_t newsize)
bool BlockArray::setSize(size_t newsize)
{
    if (blocksize == 0) {
        return false;
    }
    // 对应C++: return setHistorySize(newsize * 1024 / blocksize);
    size_t blocks = newsize * 1024 / blocksize;
    // Ensure at least one block when a non-zero size was requested (the Python
    // port added this for blocksize > 1KB; keep it for parity).
    if (blocks == 0 && newsize > 0) {
        blocks = 1;
    }
    return setHistorySize(blocks);
}

// 对应C++: bool BlockArray::setHistorySize(size_t newsize)
bool BlockArray::setHistorySize(size_t newsize)
{
    if (size == newsize) {
        return false;
    }

    unmap();

    if (!newsize) {
        delete lastblock;
        lastblock = nullptr;
        if (ion.isOpen()) {
            ion.close();
        }
        current = BLOCKARRAY_INVALID;
        index = BLOCKARRAY_INVALID;
        length = 0;
        size = 0;
        return true;
    }

    if (!size) {
        // Create the backing temp file.
        // 对应C++: FILE * tmp = tmpfile();
        QTemporaryFile tmp;
        tmp.setAutoRemove(true);
        if (!tmp.open()) {
            qWarning() << "konsole: cannot open temp file";
            return false;
        }
        // Detach the file path so the QFile member owns an independent handle
        // to the same (auto-removed) temp file. QTemporaryFile deletes the
        // file when `tmp` goes out of scope, so instead we keep a persistent
        // temp file by adopting its file name into our QFile.
        const QString fileName = tmp.fileName();
        tmp.setAutoRemove(false);
        tmp.close();

        ion.setFileName(fileName);
        if (!ion.open(QIODevice::ReadWrite)) {
            qWarning() << "konsole: cannot open temp file";
            QFile::remove(fileName);
            return false;
        }
        // Remove the directory entry now; the open handle keeps the storage
        // alive until we close it (POSIX semantics — matches tmpfile()).
        QFile::remove(fileName);

        Q_ASSERT(!lastblock);
        if (!lastblock) {
            lastblock = new Block;
        }
        size = newsize;
        return false;
    }

    if (newsize > size) {
        increaseBuffer();
        size = newsize;
        return false;
    } else {
        decreaseBuffer(newsize);
        ion.resize(static_cast<qint64>(length * blocksize));
        size = newsize;
        return true;
    }
}

// 对应C++: int moveBlock(FILE * fion, int cursor, int newpos, char * buffer2)
bool BlockArray::moveBlock(qint64 cursor, qint64 newpos, QByteArray &buffer)
{
    if (!ion.seek(cursor * static_cast<qint64>(blocksize))) {
        return false;
    }
    buffer = ion.read(static_cast<qsizetype>(blocksize));
    if (buffer.isEmpty()) {
        return false;
    }

    if (!ion.seek(newpos * static_cast<qint64>(blocksize))) {
        return false;
    }
    return ion.write(buffer) == buffer.size();
}

// 对应C++: void BlockArray::decreaseBuffer(size_t newsize)
void BlockArray::decreaseBuffer(size_t newsize)
{
    if (index < newsize) {
        return;
    }

    const size_t offset = (current - (newsize - 1) + size) % size;
    if (!offset) {
        return;
    }

    QByteArray buffer;

    size_t firstblock;
    if (current <= newsize) {
        firstblock = current + 1;
    } else {
        firstblock = 0;
    }

    size_t oldpos;
    for (size_t i = 0, cursor = firstblock; i < newsize; i++) {
        oldpos = (size + cursor + offset) % size;
        moveBlock(static_cast<qint64>(oldpos), static_cast<qint64>(cursor), buffer);
        if (oldpos < newsize) {
            cursor = oldpos;
        } else {
            cursor++;
        }
    }

    current = newsize - 1;
    length = newsize;
}

// 对应C++: void BlockArray::increaseBuffer()
void BlockArray::increaseBuffer()
{
    if (index < size) {
        return;
    }

    const size_t offset = (current + size + 1) % size;
    if (!offset) {
        return;
    }

    QByteArray buffer1;
    QByteArray buffer2;

    size_t runs = 1;
    size_t bpr = size; // blocks per run

    if (size % offset == 0) {
        bpr = size / offset;
        runs = offset;
    }

    for (size_t i = 0; i < runs; i++) {
        // free one block in the chain
        const size_t firstblock = (offset + i) % size;
        if (!ion.seek(static_cast<qint64>(firstblock * blocksize))) {
            return;
        }
        buffer1 = ion.read(static_cast<qsizetype>(blocksize));

        size_t cursor = firstblock;
        for (size_t j = 1; j < bpr; j++) {
            cursor = (cursor + offset) % size;
            const size_t newpos = (cursor - offset + size) % size;
            moveBlock(static_cast<qint64>(cursor), static_cast<qint64>(newpos), buffer2);
        }

        if (!ion.seek(static_cast<qint64>(i * blocksize))) {
            return;
        }
        ion.write(buffer1);
    }

    current = size - 1;
    length = size;
}

// --- raw block I/O helpers -------------------------------------------------

bool BlockArray::readRawBlock(qint64 pos, QByteArray &out)
{
    if (!ion.isOpen()) {
        return false;
    }
    if (!ion.seek(pos * static_cast<qint64>(blocksize))) {
        return false;
    }
    out = ion.read(static_cast<qsizetype>(blocksize));
    return out.size() == static_cast<qsizetype>(blocksize);
}

bool BlockArray::writeRawBlock(qint64 pos, const QByteArray &data)
{
    if (!ion.isOpen()) {
        return false;
    }
    if (!ion.seek(pos * static_cast<qint64>(blocksize))) {
        return false;
    }
    return ion.write(data) == data.size();
}

} // namespace Konsole
