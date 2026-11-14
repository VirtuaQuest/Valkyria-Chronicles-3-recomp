#pragma once
// PARAM.SFO parser.
#include "types.h"
#include <map>

namespace vc2 {

struct SfoData {
    std::map<std::string, std::string> strings;
    std::map<std::string, u32> ints;
    std::string get(const std::string& k) const {
        auto it = strings.find(k);
        return it == strings.end() ? std::string() : it->second;
    }
};

inline bool parseSfo(const Bytes& d, SfoData& out) {
    if (d.size() < 20 || rd32(d.data()) != 0x46535000) return false;  // "\0PSF"
    u32 keyTable = rd32(&d[8]), dataTable = rd32(&d[12]), count = rd32(&d[16]);
    for (u32 i = 0; i < count; i++) {
        size_t o = 20 + i * 16;
        if (o + 16 > d.size()) return false;
        u16 keyOff = rd16(&d[o]);
        u16 fmt = rd16(&d[o + 2]);
        u32 len = rd32(&d[o + 4]);
        u32 dataOff = rd32(&d[o + 12]);
        size_t ko = keyTable + keyOff, dO = dataTable + dataOff;
        if (ko >= d.size() || dO + len > d.size()) return false;
        std::string key((const char*)&d[ko]);
        if (fmt == 0x0404) {
            out.ints[key] = rd32(&d[dO]);
        } else {
            std::string v((const char*)&d[dO], len);
            while (!v.empty() && v.back() == 0) v.pop_back();
            out.strings[key] = v;
        }
    }
    return true;
}

}  // namespace vc2
