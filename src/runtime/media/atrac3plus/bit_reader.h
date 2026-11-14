#pragma once
// MSB-first bit reader and table-driven Huffman (VLC) decoder shared by the ATRAC decoders.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vc2rt::media::atrac {

// Reads bits MSB first from a buffer that the owner pads with at least kPadBytes zero bytes.
// Reading past the end yields zero bits; the position saturates 8 bits beyond the end so that
// bitsLeft() of a truncated frame stays slightly negative instead of running away.
class BitReader {
public:
    static constexpr size_t kPadBytes = 16;

    void init(const uint8_t* data, size_t bytes) {
        data_ = data;
        sizeBits_ = (int64_t)bytes * 8;
        limit_ = sizeBits_ + 8;
        pos_ = 0;
    }
    // n = 0..25
    uint32_t peek(int n) const {
        if (n == 0) return 0;
        const uint8_t* p = data_ + (pos_ >> 3);
        uint32_t w = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
        return (w << (pos_ & 7)) >> (32 - n);
    }
    void skip(int n) {
        pos_ += n;
        if (pos_ > limit_) pos_ = limit_;
    }
    uint32_t read(int n) {
        uint32_t v = peek(n);
        skip(n);
        return v;
    }
    int read1() { return (int)read(1); }
    int64_t bitsLeft() const { return sizeBits_ - pos_; }
    int64_t position() const { return pos_; }
    void seek(int64_t bit) { pos_ = bit < 0 ? 0 : (bit > limit_ ? limit_ : bit); }

private:
    const uint8_t* data_ = nullptr;
    int64_t sizeBits_ = 0, limit_ = 0, pos_ = 0;
};

// Owns a padded copy of one encoded frame so the reader never touches memory outside it.
class FrameBuffer {
public:
    const uint8_t* load(const uint8_t* data, size_t size) {
        buf_.resize(size + BitReader::kPadBytes);
        if (size) memcpy(buf_.data(), data, size);
        memset(buf_.data() + size, 0, BitReader::kPadBytes);
        return buf_.data();
    }

private:
    std::vector<uint8_t> buf_;
};

// Single-level lookup Huffman decoder. Each entry covers all codes sharing its prefix; an
// unassigned prefix decodes as -1 and consumes no bits (as the reference decoder does).
class Vlc {
public:
    // Explicit code words: bits[i] is the length of codes[i] (0 = symbol unused).
    void buildExplicit(int numCodes, const uint8_t* bits, const uint8_t* codes8, const uint16_t* codes16,
                       const uint8_t* symbols);
    // Canonical code book: cb = {min_len, max_len, count[min_len], ..., count[max_len]}; codes are
    // assigned in increasing order, symbol i taking symbols[i] (or i if symbols is null).
    void buildCanonical(const uint8_t* cb, const uint8_t* symbols);

    bool valid() const { return bits_ > 0; }
    int bits() const { return bits_; }

    int decode(BitReader& br) const {
        const Entry e = table_[br.peek(bits_)];
        br.skip(e.len);
        return e.sym;
    }

private:
    struct Entry {
        int16_t sym = -1;
        uint8_t len = 0;
    };
    void insert(uint32_t code, int len, int sym);
    int bits_ = 0;
    std::vector<Entry> table_;
};

inline int signExtend(int v, int bits) {
    const unsigned shift = 32 - bits;
    return (int)((unsigned)v << shift) >> shift;
}

}  // namespace vc2rt::media::atrac
