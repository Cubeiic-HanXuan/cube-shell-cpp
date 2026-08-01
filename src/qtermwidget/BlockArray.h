#pragma once

// BlockArray.h — C++ port of qtermwidget/block_array.py
//
// A file-backed ring buffer of fixed-size blocks used to implement the
// terminal scroll-back history. The file acts as a swap area: only the most
// recently touched block is kept in memory, the rest lives in a temporary
// file. Ported from the Python PySide6 version (converted from Konsole /
// QTermWidget), but implemented cleanly with QFile + seek/read/write rather
// than the raw mmap()/lseek()/write() the original C used.
//
// Original copyright:
//   Copyright (C) 2000 by Stephan Kulow <coolo@kde.org>
//   Rewritten for QT4 by e_k <e_k at users.sourceforge.net>, Copyright (C)2008

#include <QFile>
#include <QtGlobal>

namespace Konsole {

// 对应C++: #define QTERMWIDGET_BLOCKSIZE (1 << 12)
inline constexpr int QTERMWIDGET_BLOCKSIZE = 1 << 12; // 4096 bytes
// 对应C++: #define ENTRIES ((QTERMWIDGET_BLOCKSIZE - sizeof(size_t)))
// On a 64-bit build sizeof(size_t) == 8, so each block holds BLOCKSIZE - 8
// bytes of payload plus an 8-byte size field at the tail.
inline constexpr int ENTRIES = QTERMWIDGET_BLOCKSIZE - 8;

// One fixed-size block of history data.
// 对应C++: struct Block { unsigned char data[ENTRIES]; size_t size; };
struct Block {
    unsigned char data[ENTRIES] = {};
    size_t size = 0;
};

// Sentinel meaning "no such index" — the C++ size_t(-1).
// The Python code simulated this with (1<<32)-1 which is wrong on 64-bit;
// here we keep the true std::numeric_limits<size_t>::max() semantics.
// 对应C++: size_t(-1)
inline constexpr size_t BLOCKARRAY_INVALID = static_cast<size_t>(-1);

// Manages the block array backing the history file.
//
// Creates a history file holding a maximum number of blocks; if more blocks
// are requested, the oldest blocks are dropped.
// 对应C++: class BlockArray
class BlockArray {
public:
    // 对应C++: BlockArray()
    BlockArray();
    // 对应C++: ~BlockArray()
    ~BlockArray();

    BlockArray(const BlockArray &) = delete;
    BlockArray &operator=(const BlockArray &) = delete;

    // Adds a block at the end of history. This may drop other blocks.
    //
    // The ownership of `block` is transferred. Returns a unique index number
    // for accessing it later (if not yet dropped). Returns BLOCKARRAY_INVALID
    // if the history is closed (size == 0) or the write failed.
    // 对应C++: size_t append(Block * block)
    size_t append(Block *block);

    // Creates a new (in-memory, not yet flushed) block and returns its index.
    // 对应C++: size_t newBlock()
    size_t newBlock();

    // Returns the not-yet-flushed block, or nullptr.
    // 对应C++: Block * lastBlock() const
    Block *lastBlock() const;

    // Returns true if the block with index `i` is available.
    // 对应C++: bool has(size_t i) const
    bool has(size_t i) const;

    // Returns the block with index `i`, or nullptr if it is unavailable.
    // The returned pointer is owned by the BlockArray and is invalidated by
    // the next at()/append()/setHistorySize() call.
    // 对应C++: const Block * at(size_t i)
    const Block *at(size_t i);

    // Returns the number of blocks currently stored.
    // 对应C++: size_t len() const
    size_t len() const;

    // Convenience: set the history size in kilobytes.
    // 对应C++: bool setSize(size_t newsize)
    bool setSize(size_t newsize);

    // Reorders blocks as needed. If `newsize` is 0, the history is cleared
    // completely. Indexes returned by append() keep their meaning but may
    // become invalid after this call.
    // 对应C++: bool setHistorySize(size_t newsize)
    bool setHistorySize(size_t newsize);

    // Current write cursor position within the ring.
    // 对应C++: size_t getCurrent() const
    size_t getCurrent() const;

private:
    // 对应C++: void unmap()
    void unmap();
    // 对应C++: void increaseBuffer()
    void increaseBuffer();
    // 对应C++: void decreaseBuffer(size_t newsize)
    void decreaseBuffer(size_t newsize);
    // helper: move one block from `cursor` to `newpos`
    // 对应C++: int moveBlock(FILE * fion, int cursor, int newpos, char * buffer2)
    bool moveBlock(qint64 cursor, qint64 newpos, QByteArray &buffer);

    // Read one raw on-disk block (BLOCKSIZE bytes incl. the trailing size_t)
    // at block position `pos` into `out`. Returns false on failure.
    bool readRawBlock(qint64 pos, QByteArray &out);
    // Write one raw on-disk block at block position `pos`.
    bool writeRawBlock(qint64 pos, const QByteArray &data);

    size_t size = 0;                  // 对应C++: size_t size
    size_t current = BLOCKARRAY_INVALID;    // 对应C++: size_t current
    size_t index = BLOCKARRAY_INVALID;      // 对应C++: size_t index
    Block *lastmap = nullptr;         // 对应C++: Block * lastmap
    size_t lastmap_index = BLOCKARRAY_INVALID; // 对应C++: size_t lastmap_index
    Block *lastblock = nullptr;       // 对应C++: Block * lastblock
    size_t length = 0;                // 对应C++: size_t length
    size_t blocksize = 0;             // 对应C++: static int blocksize

    // The backing store. Opened lazily by setHistorySize().
    QFile ion;                        // 对应C++: int ion (fd) — QFile here
};

} // namespace Konsole
