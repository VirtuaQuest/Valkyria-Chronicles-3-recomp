#pragma once
// PSMF (PSP movie format) header parsing and a packet-driven MPEG-PS demuxer that splits the
// H.264 video stream (0xE0..0xEF) into access units and the ATRAC3plus audio of private stream 1
// (0xBD, PSP sub-stream header) into codec frames, each with pts/dts in 90 kHz units.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace vc2rt::media {

constexpr uint32_t kPsmfMagic = 0x464D5350;  // "PSMF" read little endian
constexpr int kPsmfPacketSize = 2048;
constexpr int64_t kNoTimestamp = -1;
constexpr int64_t kVideoTimestampStep = 3003;  // 90000 / 29.97

struct PsmfStream {
    uint8_t streamId = 0;     // 0xE0.. video, 0xBD private stream 1
    uint8_t privateId = 0;    // sub-stream for private stream 1
    uint16_t type = 0;        // reserved
};

struct PsmfHeader {
    uint32_t magic = 0;
    uint32_t rawVersion = 0;    // "0015" as little-endian u32
    int version = -1;           // 0..3 for 0012..0015, -1 unknown
    uint32_t streamOffset = 0;  // byte offset of the MPEG-PS data
    uint32_t streamSize = 0;    // size of the MPEG-PS data in bytes
    int64_t firstTimestamp = 0; // 90 kHz
    int64_t lastTimestamp = 0;
    int width = 0, height = 0;  // from the first video stream entry
    std::vector<PsmfStream> streams;

    bool magicOk() const { return magic == kPsmfMagic; }
    bool offsetOk() const { return streamOffset != 0 && (streamOffset & 2047) == 0; }
    bool valid() const { return magicOk() && version >= 0 && offsetOk(); }
    int64_t durationTicks() const { return lastTimestamp - firstTimestamp; }
};

// Parses the PSMF header at `buf` (len bytes available, at least 0x90 needed for the stream table).
// Always fills what it can; check valid()/magicOk().
void parsePsmfHeader(const uint8_t* buf, size_t len, PsmfHeader& out);

struct AccessUnit {
    std::vector<uint8_t> data;  // video: Annex-B NAL units; audio: one codec frame (no PSMF frame header)
    int64_t pts = kNoTimestamp;
    int64_t dts = kNoTimestamp;
    uint32_t esSize = 0;        // bytes as the PSP reports it (audio includes the 8-byte frame header)
    uint8_t frameHeader[8] = {};  // audio only: the PSMF ATRAC frame header (0F D0 xx xx ...)
};

class PsDemuxer {
public:
    PsDemuxer() { reset(); }
    void reset();

    // Selects the elementary streams to extract (others are dropped). -1 disables.
    void setVideoStream(int num) { videoId_ = num < 0 ? -1 : 0xE0 + (num & 0xF); }
    void setAudioStream(int num) { audioChannel_ = num < 0 ? -1 : (num & 0xF); }

    // Feeds one PSMF packet (normally exactly 2048 bytes = one pack). Returns false if the data
    // does not look like MPEG-PS (e.g. a PSMF header packet), in which case it is ignored.
    bool feedPacket(const uint8_t* pkt, size_t len);
    // No more data will arrive: the trailing video access unit becomes complete.
    void setEndOfStream() { eos_ = true; }
    bool endOfStream() const { return eos_; }

    bool hasVideoAu() { return findVideoAu(nullptr, nullptr); }
    bool popVideoAu(AccessUnit& au);
    bool hasAudioFrame();
    bool popAudioFrame(AccessUnit& au);

    // Nothing buffered for the stream any more (and nothing incomplete).
    bool videoDrained() const { return vbuf_.size() - vpos_ < 4; }
    bool audioDrained() const { return abuf_.size() - apos_ < 8; }

    uint64_t packetsFed() const { return packetsFed_; }

private:
    struct PtsMark {
        size_t begin, end;  // payload range in the ES buffer (absolute offsets)
        int64_t pts, dts;
    };
    void parsePes(int id, const uint8_t* p, size_t len);
    bool findVideoAu(size_t* start, size_t* end);
    void compact();

    int videoId_ = 0xE0, audioChannel_ = 0;
    bool eos_ = false;
    uint64_t packetsFed_ = 0;

    // video ES (absolute offsets = base + index)
    std::vector<uint8_t> vbuf_;
    size_t vpos_ = 0;        // start of the next (unpopped) AU
    size_t vscan_ = 0;       // scan resume point when looking for the AU end
    size_t vbase_ = 0;       // absolute offset of vbuf_[0]
    std::deque<PtsMark> vmarks_;
    int64_t vLastPts_ = kNoTimestamp, vLastDts_ = kNoTimestamp;
    bool vStarted_ = false;  // the first AU start has been located
    bool vSeenVcl_ = false;  // the AU being scanned already contains a slice

    // audio ES (frames: 8-byte PSMF header + payload)
    std::vector<uint8_t> abuf_;
    size_t apos_ = 0;
    size_t abase_ = 0;
    std::deque<PtsMark> amarks_;
    int64_t aLastPts_ = kNoTimestamp;
    double aPtsFrac_ = 0;
    bool aSynced_ = false;
};

}  // namespace vc2rt::media
