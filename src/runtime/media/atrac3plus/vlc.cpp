#include <algorithm>

#include "bit_reader.h"

namespace vc2rt::media::atrac {

void Vlc::insert(uint32_t code, int len, int sym) {
    // every table index whose top `len` bits equal `code`
    const uint32_t first = code << (bits_ - len);
    const uint32_t count = 1u << (bits_ - len);
    for (uint32_t i = 0; i < count; i++) {
        Entry& e = table_[first + i];
        e.sym = (int16_t)sym;
        e.len = (uint8_t)len;
    }
}

void Vlc::buildExplicit(int numCodes, const uint8_t* bits, const uint8_t* codes8, const uint16_t* codes16,
                        const uint8_t* symbols) {
    int maxLen = 0;
    for (int i = 0; i < numCodes; i++) maxLen = std::max(maxLen, (int)bits[i]);
    bits_ = maxLen;
    table_.assign((size_t)1 << maxLen, Entry{});
    for (int i = 0; i < numCodes; i++) {
        if (!bits[i]) continue;
        const uint32_t code = codes8 ? codes8[i] : codes16[i];
        insert(code, bits[i], symbols ? symbols[i] : i);
    }
}

void Vlc::buildCanonical(const uint8_t* cb, const uint8_t* symbols) {
    const int minLen = cb[0], maxLen = cb[1];
    bits_ = maxLen;
    table_.assign((size_t)1 << maxLen, Entry{});
    uint32_t code = 0;
    int index = 0;
    for (int len = minLen; len <= maxLen; len++) {
        for (int n = cb[2 + len - minLen]; n > 0; n--, index++, code++)
            insert(code, len, symbols ? symbols[index] : index);
        code <<= 1;
    }
}

}  // namespace vc2rt::media::atrac
