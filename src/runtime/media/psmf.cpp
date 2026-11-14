#include "psmf.h"

#include <algorithm>
#include <cstring>

namespace vc2rt::media {

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
int64_t be48(const uint8_t* p) {
    int64_t v = 0;
    for (int i = 0; i < 6; i++) v = (v << 8) | p[i];
    return v;
}
uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// 33-bit PES timestamp from its 5-byte encoding.
int64_t readPesTs(const uint8_t* p) {
    return ((int64_t)(p[0] >> 1) & 7) << 30 | (int64_t)p[1] << 22 | (int64_t)(p[2] >> 1) << 15 | (int64_t)p[3] << 7 |
           (int64_t)(p[4] >> 1);
}

constexpr double kAudioTicksPerFrame = 2048.0 * 90000.0 / 44100.0;  // ATRAC3plus @ 44.1 kHz

}  // namespace

void parsePsmfHeader(const uint8_t* buf, size_t len, PsmfHeader& h) {
    h = PsmfHeader{};
    if (len < 16) return;
    h.magic = le32(buf);
    h.rawVersion = le32(buf + 4);
    switch (h.rawVersion) {
        case 0x32313030: h.version = 0; break;  // "0012"
        case 0x33313030: h.version = 1; break;  // "0013"
        case 0x34313030: h.version = 2; break;  // "0014"
        case 0x35313030: h.version = 3; break;  // "0015"
        default: h.version = -1; break;
    }
    h.streamOffset = be32(buf + 8);
    h.streamSize = be32(buf + 12);
    if (len >= 0x60) {
        h.firstTimestamp = be48(buf + 0x54);
        h.lastTimestamp = be48(buf + 0x5A);
    }
    // Stream table: count (u16 BE) at 0x80, then 16-byte entries.
    if (len >= 0x82) {
        int n = be16(buf + 0x80);
        for (int i = 0; i < n && 0x82 + (size_t)i * 16 + 16 <= len; i++) {
            const uint8_t* e = buf + 0x82 + i * 16;
            PsmfStream s;
            s.streamId = e[0];
            s.privateId = e[1];
            h.streams.push_back(s);
            if ((s.streamId & 0xF0) == 0xE0 && h.width == 0) {
                h.width = e[0x0C] * 16;
                h.height = e[0x0D] * 16;
            }
        }
    }
}

// ------------------------------------------------------------------ demuxer
void PsDemuxer::reset() {
    eos_ = false;
    packetsFed_ = 0;
    vbuf_.clear();
    vpos_ = vscan_ = vbase_ = 0;
    vmarks_.clear();
    vLastPts_ = vLastDts_ = kNoTimestamp;
    vStarted_ = false;
    abuf_.clear();
    apos_ = abase_ = 0;
    amarks_.clear();
    aLastPts_ = kNoTimestamp;
    aPtsFrac_ = 0;
    aSynced_ = false;
}

bool PsDemuxer::feedPacket(const uint8_t* p, size_t len) {
    // A PSMF pack always starts with a pack header; anything else (the PSMF header itself, padding
    // sectors) is not stream data.
    if (len < 14 || be32(p) != 0x000001BA) return false;
    packetsFed_++;
    size_t i = 0;
    while (i + 4 <= len) {
        if (p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 1) {
            i++;  // resync on the next start code
            continue;
        }
        const uint8_t code = p[i + 3];
        if (code == 0xBA) {  // pack header (MPEG-2: 14 bytes + stuffing)
            if (i + 14 > len) break;
            if ((p[i + 4] & 0xC0) == 0x40) {
                i += 14 + (p[i + 13] & 7);
            } else {
                i += 12;  // MPEG-1 pack header
            }
            continue;
        }
        if (code == 0xB9) {  // MPEG program end
            eos_ = true;
            i += 4;
            continue;
        }
        if (code < 0xBB) {  // not a system / PES start code
            i += 4;
            continue;
        }
        if (i + 6 > len) break;
        const size_t plen = be16(p + i + 4);
        const size_t avail = std::min(plen, len - (i + 6));
        if (code == 0xBD || (code & 0xF0) == 0xE0) parsePes(code, p + i + 6, avail);
        i += 6 + plen;
    }
    return true;
}

void PsDemuxer::parsePes(int id, const uint8_t* p, size_t len) {
    if (len < 3 || (p[0] & 0xC0) != 0x80) return;  // MPEG-2 PES only (PSMF)
    const uint8_t flags = p[1];
    const size_t hdrLen = p[2];
    if (3 + hdrLen > len) return;
    int64_t pts = kNoTimestamp, dts = kNoTimestamp;
    if ((flags & 0x80) && hdrLen >= 5) {
        pts = dts = readPesTs(p + 3);
        if ((flags & 0x40) && hdrLen >= 10) dts = readPesTs(p + 8);
    }
    const uint8_t* pay = p + 3 + hdrLen;
    size_t payLen = len - 3 - hdrLen;

    if (id != 0xBD) {
        if (id != videoId_) return;
        const size_t begin = vbase_ + vbuf_.size();
        vbuf_.insert(vbuf_.end(), pay, pay + payLen);
        if (pts != kNoTimestamp) vmarks_.push_back({begin, begin + payLen, pts, dts});
        return;
    }

    // Private stream 1, PSP sub-stream header: channel, 0, offset of the first frame header (BE16)
    if (payLen < 4) return;
    const int channel = pay[0];
    if (audioChannel_ < 0 || channel != audioChannel_) return;
    const size_t firstFrame = be16(pay + 2);
    pay += 4;
    payLen -= 4;
    size_t skip = 0;
    if (!aSynced_) {
        // Drop the tail of a frame whose beginning we never saw.
        if (firstFrame >= payLen) return;
        skip = firstFrame;
        aSynced_ = true;
    }
    const size_t begin = abase_ + abuf_.size();
    abuf_.insert(abuf_.end(), pay + skip, pay + payLen);
    if (pts != kNoTimestamp && firstFrame < payLen) {
        const size_t frameStart = begin + (firstFrame - skip);
        amarks_.push_back({frameStart, begin + (payLen - skip), pts, pts});
    }
}

// Locates the next complete access unit: [start, end) absolute offsets.
bool PsDemuxer::findVideoAu(size_t* outStart, size_t* outEnd) {
    const size_t end = vbase_ + vbuf_.size();
    auto at = [&](size_t abs) { return vbuf_[abs - vbase_]; };
    auto isStart = [&](size_t abs) { return at(abs) == 0 && at(abs + 1) == 0 && at(abs + 2) == 1; };

    if (!vStarted_) {
        // The first AU begins at the first NAL unit.
        size_t s = vpos_;
        while (s + 3 < end && !isStart(s)) s++;
        if (s + 3 >= end) {
            vpos_ = s;
            return false;
        }
        vpos_ = s;
        vscan_ = s;
        vStarted_ = true;
        vSeenVcl_ = false;
    }
    // Continue scanning for the NAL that begins the following AU.
    size_t i = vscan_;
    while (i + 4 < end) {
        if (!isStart(i)) {
            i++;
            continue;
        }
        const int type = at(i + 3) & 0x1F;
        bool vcl = type >= 1 && type <= 5;
        bool newAu = false;
        if (vSeenVcl_) {
            if (type == 9 || type == 6 || type == 7 || type == 8 || (type >= 14 && type <= 18)) newAu = true;
            else if (vcl && (at(i + 4) & 0x80)) newAu = true;  // first_mb_in_slice == 0
        }
        if (newAu) {
            if (outStart) *outStart = vpos_;
            if (outEnd) *outEnd = i;
            vscan_ = i;
            return true;
        }
        if (vcl) vSeenVcl_ = true;
        i += 3;
    }
    vscan_ = std::max(vscan_, i);
    if (eos_ && end > vpos_ + 4) {
        if (outStart) *outStart = vpos_;
        if (outEnd) *outEnd = end;
        return true;
    }
    return false;
}

bool PsDemuxer::popVideoAu(AccessUnit& au) {
    size_t s, e;
    if (!findVideoAu(&s, &e)) return false;
    au.data.assign(vbuf_.begin() + (s - vbase_), vbuf_.begin() + (e - vbase_));
    au.esSize = (uint32_t)au.data.size();
    // The PES timestamp belongs to the first AU whose first byte lies inside that PES. An AU starts
    // with a 4-byte start code (zero_byte + 00 00 01), so its first byte is the zero before s.
    const size_t key = (s > vbase_ && vbuf_[s - 1 - vbase_] == 0) ? s - 1 : s;
    while (!vmarks_.empty() && vmarks_.front().end <= key) vmarks_.pop_front();
    if (!vmarks_.empty() && vmarks_.front().begin <= key) {
        au.pts = vmarks_.front().pts;
        au.dts = vmarks_.front().dts;
        vmarks_.pop_front();
    } else if (vLastPts_ != kNoTimestamp) {
        au.pts = vLastPts_ + kVideoTimestampStep;
        au.dts = vLastDts_ + kVideoTimestampStep;
    } else {
        au.pts = au.dts = kNoTimestamp;
    }
    vLastPts_ = au.pts;
    vLastDts_ = au.dts;
    vpos_ = e;
    vscan_ = e;  // rescan the first NAL of the next AU (it may be a VCL NAL)
    vSeenVcl_ = false;
    compact();
    return true;
}

bool PsDemuxer::hasAudioFrame() {
    for (;;) {
        const size_t avail = abase_ + abuf_.size() - apos_;
        if (avail < 8) return false;
        const uint8_t* h = abuf_.data() + (apos_ - abase_);
        if (h[0] == 0x0F && h[1] == 0xD0) {
            const size_t frame = 8 + ((((size_t)h[2] & 3) << 8) | h[3]) * 8 + 8;
            return avail >= frame;
        }
        apos_++;  // lost sync: hunt for the next frame header
    }
}

bool PsDemuxer::popAudioFrame(AccessUnit& au) {
    if (!hasAudioFrame()) return false;
    const uint8_t* h = abuf_.data() + (apos_ - abase_);
    const size_t payload = ((((size_t)h[2] & 3) << 8) | h[3]) * 8 + 8;
    memcpy(au.frameHeader, h, 8);
    au.data.assign(h + 8, h + 8 + payload);
    au.esSize = (uint32_t)(8 + payload);
    const size_t s = apos_;
    while (!amarks_.empty() && amarks_.front().end <= s) amarks_.pop_front();
    if (!amarks_.empty() && amarks_.front().begin <= s) {
        au.pts = au.dts = amarks_.front().pts;
        aPtsFrac_ = 0;
        amarks_.pop_front();
    } else if (aLastPts_ != kNoTimestamp) {
        const double t = (double)aLastPts_ + aPtsFrac_ + kAudioTicksPerFrame;
        au.pts = au.dts = (int64_t)t;
        aPtsFrac_ = t - (double)au.pts;
    } else {
        au.pts = au.dts = kNoTimestamp;
    }
    aLastPts_ = au.pts;
    apos_ += 8 + payload;
    compact();
    return true;
}

void PsDemuxer::compact() {
    const size_t vdead = vpos_ - vbase_;
    if (vdead > (1u << 20) || (vdead > 0 && vdead == vbuf_.size())) {
        vbuf_.erase(vbuf_.begin(), vbuf_.begin() + vdead);
        vbase_ += vdead;
    }
    const size_t adead = apos_ - abase_;
    if (adead > (256u << 10) || (adead > 0 && adead == abuf_.size())) {
        abuf_.erase(abuf_.begin(), abuf_.begin() + adead);
        abase_ += adead;
    }
}

}  // namespace vc2rt::media
