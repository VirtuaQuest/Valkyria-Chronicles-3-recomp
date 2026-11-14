#include "inflate.h"

#include <cstring>

namespace vc2 {

namespace {

struct BitReader {
    const u8* in;
    size_t len, pos = 0;
    u32 buf = 0;
    int count = 0;
    bool overrun = false;

    // Reads `n` (0..16) bits, least significant first.
    u32 bits(int n) {
        while (count < n) {
            if (pos >= len) {
                overrun = true;
                return 0;
            }
            buf |= (u32)in[pos++] << count;
            count += 8;
        }
        const u32 v = buf & ((1u << n) - 1);
        buf >>= n;
        count -= n;
        return v;
    }
    void alignToByte() {
        buf = 0;
        count = 0;
    }
};

// Canonical Huffman code: number of codes per length and the symbols sorted by code.
struct Huffman {
    u16 count[16];
    u16 symbol[288];

    // Returns false if the lengths describe more codes than fit (over-subscribed).
    bool build(const u8* lengths, int n) {
        memset(count, 0, sizeof count);
        for (int i = 0; i < n; i++) count[lengths[i]]++;
        count[0] = 0;
        int left = 1;
        for (int len = 1; len < 16; len++) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false;
        }
        u16 offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + count[len];
        for (int i = 0; i < n; i++)
            if (lengths[i]) symbol[offs[lengths[i]]++] = (u16)i;
        return true;
    }

    // Decodes one symbol bit by bit; -1 on an invalid code or input overrun.
    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++) {
            code |= (int)br.bits(1);
            if (br.overrun) return -1;
            const int n = count[len];
            if (code - n < first) return symbol[index + (code - first)];
            index += n;
            first += n;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

const u16 kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const u8 kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const u16 kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const u8 kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

struct Output {
    u8* out;
    size_t cap, pos = 0;
};

bool inflateCodes(BitReader& br, Output& o, const Huffman& lit, const Huffman& dist) {
    for (;;) {
        const int sym = lit.decode(br);
        if (sym < 0) return false;
        if (sym < 256) {
            if (o.pos >= o.cap) return false;
            o.out[o.pos++] = (u8)sym;
        } else if (sym == 256) {
            return true;
        } else {
            const int li = sym - 257;
            if (li >= 29) return false;
            const size_t length = kLenBase[li] + br.bits(kLenExtra[li]);
            const int di = dist.decode(br);
            if (di < 0 || di >= 30) return false;
            const size_t d = kDistBase[di] + br.bits(kDistExtra[di]);
            if (br.overrun || d > o.pos || o.pos + length > o.cap) return false;
            for (size_t i = 0; i < length; i++, o.pos++) o.out[o.pos] = o.out[o.pos - d];  // may overlap
        }
    }
}

bool inflateStored(BitReader& br, Output& o) {
    br.alignToByte();
    if (br.pos + 4 > br.len) return false;
    const u32 len = br.in[br.pos] | (br.in[br.pos + 1] << 8);
    const u32 nlen = br.in[br.pos + 2] | (br.in[br.pos + 3] << 8);
    br.pos += 4;
    if (len != (~nlen & 0xFFFF) || br.pos + len > br.len || o.pos + len > o.cap) return false;
    memcpy(o.out + o.pos, br.in + br.pos, len);
    br.pos += len;
    o.pos += len;
    return true;
}

struct FixedCodes {
    Huffman lit, dist;
    FixedCodes() {
        u8 l[288];
        int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        lit.build(l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        dist.build(l, 30);
    }
};

bool inflateFixed(BitReader& br, Output& o) {
    static const FixedCodes fixed;  // thread-safe one-time initialisation
    return inflateCodes(br, o, fixed.lit, fixed.dist);
}

bool inflateDynamic(BitReader& br, Output& o) {
    static const u8 kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const int nlen = (int)br.bits(5) + 257, ndist = (int)br.bits(5) + 1, ncode = (int)br.bits(4) + 4;
    if (br.overrun || nlen > 286 || ndist > 30) return false;
    u8 lengths[320] = {};
    for (int i = 0; i < ncode; i++) lengths[kOrder[i]] = (u8)br.bits(3);
    Huffman code;
    if (!code.build(lengths, 19)) return false;
    memset(lengths, 0, sizeof lengths);
    for (int i = 0; i < nlen + ndist;) {
        const int sym = code.decode(br);
        if (sym < 0) return false;
        if (sym < 16) {
            lengths[i++] = (u8)sym;
            continue;
        }
        u8 value = 0;
        int repeat;
        if (sym == 16) {
            if (i == 0) return false;
            value = lengths[i - 1];
            repeat = 3 + (int)br.bits(2);
        } else if (sym == 17) {
            repeat = 3 + (int)br.bits(3);
        } else {
            repeat = 11 + (int)br.bits(7);
        }
        if (br.overrun || i + repeat > nlen + ndist) return false;
        while (repeat--) lengths[i++] = value;
    }
    if (lengths[256] == 0) return false;  // no end-of-block code
    Huffman lit, dist;
    if (!lit.build(lengths, nlen) || !dist.build(lengths + nlen, ndist)) return false;
    return inflateCodes(br, o, lit, dist);
}

}  // namespace

bool inflateRaw(const u8* in, size_t inLen, u8* out, size_t outCap, size_t* outLen) {
    BitReader br{in, inLen};
    Output o{out, outCap};
    for (;;) {
        const u32 last = br.bits(1);
        const u32 type = br.bits(2);
        if (br.overrun) return false;
        bool ok;
        switch (type) {
            case 0: ok = inflateStored(br, o); break;
            case 1: ok = inflateFixed(br, o); break;
            case 2: ok = inflateDynamic(br, o); break;
            default: ok = false;
        }
        if (!ok || br.overrun) return false;
        if (last) break;
    }
    if (outLen) *outLen = o.pos;
    return true;
}

}  // namespace vc2
