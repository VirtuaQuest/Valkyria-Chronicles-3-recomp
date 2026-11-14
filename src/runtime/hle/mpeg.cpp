// sceMpeg: PSMF movie playback (ringbuffer, demux, H.264 video via Media Foundation, ATRAC3plus
// audio via the pluggable decoder in media/atrac_decoder.h).
//
// Data flow, as on the PSP:
//   sceMpegRingbufferPut   -> calls the game's read callback, which fills 2048-byte packets
//   sceMpegGetAvcAu / GetAtracAu -> the demuxer pulls packets out of the ring (freeing them) until
//                            one access unit of the requested stream is complete
//   sceMpegAvcDecode       -> H.264 decode + colour conversion into the game's frame buffer
//   sceMpegAtracDecode     -> 2048 stereo samples per AU (silence until a real decoder is plugged in)
// The ring stays filled until the demuxer consumes packets, so sceMpegRingbufferAvailableSize gives
// the game the same back-pressure (and the same "all packets free" end-of-movie signal) as the PSP.
#include <map>
#include <memory>

#include "../kernel/kernel.h"
#include "../media/atrac_decoder.h"
#include "../media/h264_decoder.h"
#include "../media/psmf.h"
#include "hle.h"

namespace vc2rt {
namespace {

using namespace media;

// ------------------------------------------------------------------ constants
enum : u32 {
    ERR_MPEG_NO_DATA = 0x80618001,
    ERR_MPEG_BAD_VERSION = 0x80610002,
    ERR_MPEG_INVALID_ADDR = 0x80610103,
    ERR_MPEG_ALREADY_INIT = 0x80618005,
    ERR_MPEG_NOT_YET_INIT = 0x80618009,
    ERR_MPEG_NO_MEMORY = 0x80610022,
    ERR_MPEG_INVALID_VALUE = 0x806101fe,
    ERR_MPEG_AVC_INVALID_VALUE = 0x806201fe,
    ERR_MPEG_AVC_DECODE_FATAL = 0x80628002,
};

constexpr u32 kPacket = 2048;
constexpr u32 kRingbufferPerPacketOverhead = 104;  // PSP library bookkeeping per packet
constexpr u32 kMpegMemSize = 0x10000;             // libmpeg 0105+
constexpr u32 kAvcEsSize = 2048;
constexpr u32 kAtracEsSize = 2112;
constexpr u32 kAtracOutputSize = 8192;  // 2048 samples * 2 channels * s16
constexpr int kAtracSamplesPerAu = 2048;
constexpr int kMaxEsBuffers = 2;

enum StreamType : int { STREAM_AVC = 0, STREAM_ATRAC = 1, STREAM_PCM = 2, STREAM_DATA = 3, STREAM_AUDIO = 15 };

// Simulated hardware latencies (microseconds). They also keep a decode loop from monopolising the
// cooperative scheduler.
constexpr u64 kAvcDecodeDelay = 3000;
constexpr u64 kAtracDecodeDelay = 1000;
constexpr u64 kNoDataDelay = 500;

// SceMpegRingbuffer (guest memory)
struct RingOff {
    enum : u32 {
        packets = 0, packetsRead = 4, packetsWritePos = 8, packetsAvail = 12, packetSize = 16, data = 20,
        callback = 24, callbackArg = 28, dataUpperBound = 32, semaId = 36, mpeg = 40, gp = 44,
    };
};

// SceMpegAu (guest memory): pts/dts are stored high word first.
struct GuestAu {
    s64 pts, dts;
    u32 esBuffer, esSize;
};
GuestAu readAu(u32 a) {
    GuestAu au;
    au.pts = (s64)((u64)rd32(a) << 32 | rd32(a + 4));
    au.dts = (s64)((u64)rd32(a + 8) << 32 | rd32(a + 12));
    au.esBuffer = rd32(a + 16);
    au.esSize = rd32(a + 20);
    return au;
}
void writeAu(u32 a, const GuestAu& au) {
    wr32(a, (u32)((u64)au.pts >> 32));
    wr32(a + 4, (u32)au.pts);
    wr32(a + 8, (u32)((u64)au.dts >> 32));
    wr32(a + 12, (u32)au.dts);
    wr32(a + 16, au.esBuffer);
    wr32(a + 20, au.esSize);
}

struct StreamReg {
    int type = 0;
    int num = 0;
};

struct MpegCtx {
    u32 mpegAddr = 0;  // SceMpeg* the game passes around (holds the handle)
    u32 handle = 0;    // data + 0x30
    u32 ringAddr = 0;
    u32 frameWidth = 0;
    int pixelMode = 3;  // ABGR8888
    int decodeMode = -1;
    bool esBuf[kMaxEsBuffers] = {};
    std::map<u32, StreamReg> streams;
    u32 nextStreamId = 0;

    PsmfHeader hdr;
    bool hdrValid = false;
    PsDemuxer demux;
    u64 streamPackets = 0;  // MPEG-PS packets pulled from the ring
    u64 packetsPut = 0;

    // video
    H264Decoder avc;
    bool avcTried = false;
    bool haveVideoAu = false;
    AccessUnit videoAu;
    int videoAus = 0, videoFrames = 0;
    bool videoEndLogged = false;
    int detailWidth = 0, detailHeight = 0;
    int frameStatus = 0;
    s64 lastFramePts = kNoTimestamp;

    // audio
    std::unique_ptr<AtracDecoder> atrac;
    bool haveAudioAu = false;
    AccessUnit audioAu;
    int audioAus = 0;
    bool audioEndLogged = false;
};

// By handle. Deliberately never destroyed: contexts still alive at process exit hold Media
// Foundation objects whose DLLs may already be unloaded by then.
std::map<u32, std::unique_ptr<MpegCtx>>& g_mpegs = *new std::map<u32, std::unique_ptr<MpegCtx>>;
bool g_mpegInit = false;

MpegCtx* getCtx(u32 mpegAddr) {
    if (!mpegAddr || !memValid(mpegAddr)) return nullptr;
    auto it = g_mpegs.find(rd32(mpegAddr));
    return it == g_mpegs.end() ? nullptr : it->second.get();
}

void delay(u64 us) { kernel::waitCurrentFor(kernel::WaitType::Delay, 0, us, false); }

s64 tsOrDefault(s64 ts, s64 fallback) { return ts == kNoTimestamp ? fallback : ts; }

void resetStreamState(MpegCtx& m, bool newMovie) {
    m.demux.reset();
    for (auto& [id, s] : m.streams) {
        if (s.type == STREAM_AVC) m.demux.setVideoStream(s.num);
        if (s.type == STREAM_ATRAC || s.type == STREAM_AUDIO) m.demux.setAudioStream(s.num);
    }
    m.streamPackets = 0;
    m.packetsPut = 0;
    m.haveVideoAu = m.haveAudioAu = false;
    if (newMovie) m.videoAus = m.videoFrames = m.audioAus = 0;
    m.videoEndLogged = m.audioEndLogged = false;
    m.frameStatus = 0;
    m.lastFramePts = kNoTimestamp;
    if (m.avc.isOpen()) m.avc.flush();
    if (m.atrac) m.atrac->reset();
}

// ------------------------------------------------------------------ ring / demux plumbing
// Moves one packet from the ring into the demuxer. Returns false if the ring is empty.
bool pullPacket(MpegCtx& m) {
    const u32 r = m.ringAddr;
    if (!r || !memValid(r)) return false;
    const s32 packets = (s32)rd32(r + RingOff::packets);
    const s32 avail = (s32)rd32(r + RingOff::packetsAvail);
    if (packets <= 0 || avail <= 0) return false;
    const u32 readPos = rd32(r + RingOff::packetsRead) % (u32)packets;
    const u32 addr = rd32(r + RingOff::data) + readPos * kPacket;
    if (memValid(addr) && memValid(addr + kPacket - 1)) {
        if (m.demux.feedPacket(memPtr(addr), kPacket)) {
            m.streamPackets++;
            if (m.hdrValid && m.streamPackets * kPacket >= m.hdr.streamSize) m.demux.setEndOfStream();
        }
    }
    wr32(r + RingOff::packetsRead, (readPos + 1) % (u32)packets);
    wr32(r + RingOff::packetsAvail, (u32)(avail - 1));
    return true;
}

bool fetchVideoAu(MpegCtx& m, AccessUnit& au) {
    for (;;) {
        if (m.demux.popVideoAu(au)) return true;
        if (!pullPacket(m)) return false;
    }
}

bool fetchAudioAu(MpegCtx& m, AccessUnit& au) {
    for (;;) {
        if (m.demux.popAudioFrame(au)) return true;
        if (!pullPacket(m)) return false;
    }
}

bool ensureAvcDecoder(MpegCtx& m) {
    if (m.avc.isOpen()) return true;
    if (m.avcTried) return false;
    m.avcTried = true;
    if (!m.avc.open()) {
        LOG_ERROR("MPEG", "H.264 decoder unavailable: %s (movies will be black)", m.avc.error().c_str());
        return false;
    }
    LOG_INFO("MPEG", "H.264 decoder: %s", m.avc.decoderName().c_str());
    return true;
}

// Writes a decoded frame into the game's buffer. Returns false if the buffer is not writable.
bool writeFrame(MpegCtx& m, const VideoFrame& f, u32 buffer, u32 frameWidth) {
    const int bpp = m.pixelMode == 3 ? 4 : 2;
    const u32 bytes = frameWidth * (u32)f.height * (u32)bpp;
    if (!memValid(buffer) || !memValid(buffer + bytes - 1)) {
        LOG_WARN("MPEG", "frame buffer %08x (+%u) is not valid guest memory", buffer, bytes);
        return false;
    }
    convertNv12ToPsp(f, memPtr(buffer), (int)frameWidth, m.pixelMode);
    return true;
}

void writeBlackFrame(MpegCtx& m, u32 buffer, u32 frameWidth) {
    VideoFrame f;
    f.width = m.detailWidth > 0 ? m.detailWidth : 480;
    f.height = m.detailHeight > 0 ? m.detailHeight : 272;
    f.stride = f.width;
    f.nv12.assign((size_t)f.width * f.height, 16);
    f.nv12.resize((size_t)f.width * f.height * 3 / 2, 128);
    writeFrame(m, f, buffer, frameWidth);
}

// ------------------------------------------------------------------ library init
void sceMpegInit(PspContext* c) {
    g_mpegInit = true;
    RET(0);
}

void sceMpegFinish(PspContext* c) {
    g_mpegInit = false;
    RET(0);
}

void sceMpegQueryMemSize(PspContext* c) { RET(kMpegMemSize); }

// ------------------------------------------------------------------ ringbuffer
void sceMpegRingbufferQueryMemSize(PspContext* c) { RET((s32)A0 * (kPacket + kRingbufferPerPacketOverhead)); }

void sceMpegRingbufferConstruct(PspContext* c) {
    const u32 ring = A0, packets = A1, data = A2, size = A3, cb = A4, cbArg = A5;
    if (!memValid(ring)) return (void)RET(kernel::ERR_ILLEGAL_ADDR);
    if ((s32)size < 0 || (u64)packets * (kPacket + kRingbufferPerPacketOverhead) > size) {
        LOG_WARN("MPEG", "RingbufferConstruct: %u packets do not fit %u bytes", packets, size);
        return (void)RET(ERR_MPEG_NO_MEMORY);
    }
    wr32(ring + RingOff::packets, packets);
    wr32(ring + RingOff::packetsRead, 0);
    wr32(ring + RingOff::packetsWritePos, 0);
    wr32(ring + RingOff::packetsAvail, 0);
    wr32(ring + RingOff::packetSize, kPacket);
    wr32(ring + RingOff::data, data);
    wr32(ring + RingOff::callback, cb);
    wr32(ring + RingOff::callbackArg, cbArg);
    wr32(ring + RingOff::dataUpperBound, data + packets * kPacket);
    wr32(ring + RingOff::mpeg, 0);
    wr32(ring + RingOff::gp, c->r[28]);
    LOG_DEBUG("MPEG", "RingbufferConstruct(%08x, %u packets, data %08x, cb %08x arg %08x)", ring, packets, data, cb, cbArg);
    RET(0);
}

void sceMpegRingbufferDestruct(PspContext* c) { RET(0); }

void sceMpegRingbufferAvailableSize(PspContext* c) {
    const u32 ring = A0;
    if (!memValid(ring)) return (void)RET(kernel::ERR_ILLEGAL_ADDR);
    const s32 packets = (s32)rd32(ring + RingOff::packets);
    const s32 avail = (s32)rd32(ring + RingOff::packetsAvail);
    RET(packets - avail);
}

void sceMpegRingbufferPut(PspContext* c) {
    const u32 ring = A0;
    s32 want = (s32)A1;
    const s32 available = (s32)A2;
    if (!memValid(ring)) return (void)RET(ERR_MPEG_INVALID_ADDR);
    const s32 packets = (s32)rd32(ring + RingOff::packets);
    if (packets <= 0) return (void)RET(ERR_MPEG_INVALID_VALUE);
    want = std::min(want, available);
    want = std::min(want, packets - (s32)rd32(ring + RingOff::packetsAvail));
    if (want <= 0) return (void)RET(0);
    const u32 cb = rd32(ring + RingOff::callback);
    if (!cb) {
        LOG_WARN("MPEG", "RingbufferPut without a callback");
        return (void)RET(0);
    }
    const u32 sp = (c->r[29] - 0x100) & ~0xFu;
    const u32 gp = rd32(ring + RingOff::gp) ? rd32(ring + RingOff::gp) : c->r[28];
    s32 total = 0;
    // The callback receives contiguous runs; a put that wraps the ring is split in two calls.
    while (want > 0) {
        const u32 writePos = rd32(ring + RingOff::packetsWritePos) % (u32)packets;
        const s32 chunk = std::min(want, packets - (s32)writePos);
        const u32 dst = rd32(ring + RingOff::data) + writePos * kPacket;
        const s32 got = (s32)callGuestSimple(cb, sp, gp, {dst, (u32)chunk, rd32(ring + RingOff::callbackArg)});
        if (got < 0) {
            LOG_WARN("MPEG", "ringbuffer read callback returned %08x", (u32)got);
            if (total == 0) return (void)RET(got);
            break;
        }
        const s32 added = std::min(got, chunk);
        if (added == 0) break;
        // Re-read: other threads may have consumed packets while the callback was blocked on I/O.
        wr32(ring + RingOff::packetsWritePos, (writePos + (u32)added) % (u32)packets);
        wr32(ring + RingOff::packetsAvail, rd32(ring + RingOff::packetsAvail) + (u32)added);
        total += added;
        want -= added;
        if (added < chunk) break;
    }
    // Looked up only now: the callback may block on I/O while another thread deletes the player.
    if (MpegCtx* m = getCtx(rd32(ring + RingOff::mpeg))) m->packetsPut += (u64)total;
    RET(total);
}

// ------------------------------------------------------------------ create / delete
void sceMpegCreate(PspContext* c) {
    const u32 mpeg = A0, data = A1, size = A2, ring = A3, frameWidth = A4;
    if (!memValid(mpeg) || !memValid(data)) return (void)RET(ERR_MPEG_INVALID_ADDR);
    if (size < kMpegMemSize) return (void)RET(ERR_MPEG_NO_MEMORY);
    const u32 handle = data + 0x30;
    // The library's own header in the work area (games occasionally peek at it).
    memcpy(memPtr(handle), "LIBMPEG\0", 8);
    memcpy(memPtr(handle + 8), "001\0", 4);
    wr32(handle + 12, 0xFFFFFFFF);
    wr32(handle + 16, ring);
    wr32(handle + 20, memValid(ring) ? rd32(ring + RingOff::dataUpperBound) : 0);
    wr32(mpeg, handle);
    if (memValid(ring)) wr32(ring + RingOff::mpeg, mpeg);

    auto m = std::make_unique<MpegCtx>();
    m->mpegAddr = mpeg;
    m->handle = handle;
    m->ringAddr = memValid(ring) ? ring : 0;
    m->frameWidth = frameWidth;
    m->demux.setVideoStream(-1);
    m->demux.setAudioStream(-1);
    LOG_INFO("MPEG", "sceMpegCreate(mpeg %08x, work %08x+%x, ring %08x, width %u)", mpeg, data, size, ring, frameWidth);
    g_mpegs[handle] = std::move(m);
    RET(0);
}

void sceMpegDelete(PspContext* c) {
    MpegCtx* m = getCtx(A0);
    if (!m) return (void)RET(ERR_MPEG_INVALID_VALUE);
    LOG_INFO("MPEG", "sceMpegDelete: %d video AUs, %d frames shown, %d audio AUs", m->videoAus, m->videoFrames,
             m->audioAus);
    g_mpegs.erase(m->handle);
    RET(0);
}

// ------------------------------------------------------------------ PSMF header
void sceMpegQueryStreamOffset(PspContext* c) {
    const u32 buf = A1, outOff = A2;
    MpegCtx* m = getCtx(A0);
    if (!m) return (void)RET(ERR_MPEG_INVALID_VALUE);
    if (!memValid(buf) || !memValid(outOff)) return (void)RET(ERR_MPEG_INVALID_ADDR);
    PsmfHeader h;
    parsePsmfHeader(memPtr(buf), kPacket, h);
    if (!h.magicOk() || !h.offsetOk()) {
        wr32(outOff, 0);
        return (void)RET(ERR_MPEG_INVALID_VALUE);
    }
    if (h.version < 0) {
        wr32(outOff, 0);
        return (void)RET(ERR_MPEG_BAD_VERSION);
    }
    // A new movie starts: analyse the header like the library does.
    m->hdr = h;
    m->hdrValid = true;
    m->detailWidth = h.width;
    m->detailHeight = h.height;
    resetStreamState(*m, true);
    LOG_INFO("MPEG", "movie: PSMF %.4s, data at 0x%x, %u bytes, %dx%d, %.2fs", (const char*)memPtr(buf + 4),
             h.streamOffset, h.streamSize, h.width, h.height, h.durationTicks() / 90000.0);
    wr32(outOff, h.streamOffset);
    RET(0);
}

void sceMpegQueryStreamSize(PspContext* c) {
    const u32 buf = A0, outSize = A1;
    if (!memValid(buf) || !memValid(outSize)) return (void)RET(ERR_MPEG_INVALID_ADDR);
    PsmfHeader h;
    parsePsmfHeader(memPtr(buf), kPacket, h);
    if (!h.magicOk() || (h.streamOffset & 2047) != 0) {
        wr32(outSize, 0);
        return (void)RET(ERR_MPEG_INVALID_VALUE);
    }
    wr32(outSize, h.streamSize);
    RET(0);
}

// ------------------------------------------------------------------ streams
void sceMpegRegistStream(PspContext* c) {
    MpegCtx* m = getCtx(A0);
    if (!m) return (void)RET(0);  // the library returns a NULL stream on failure
    const int type = (int)A1, num = (int)A2;
    switch (type) {
        case STREAM_AVC: m->demux.setVideoStream(num); break;
        case STREAM_ATRAC:
        case STREAM_AUDIO: m->demux.setAudioStream(num); break;
        case STREAM_PCM:
        case STREAM_DATA: LOG_WARN("MPEG", "stream type %d not supported (ignored)", type); break;
        default: return (void)RET(0);
    }
    // Stream handles are addresses inside the library work area on the PSP; keep them unique and
    // non-zero.
    const u32 id = m->handle + 0x100 + 0x20 * (m->nextStreamId++ & 0x3F);
    m->streams[id] = StreamReg{type, num};
    RET(id);
}

void sceMpegUnRegistStream(PspContext* c) {
    MpegCtx* m = getCtx(A0);
    if (!m) return (void)RET(ERR_MPEG_INVALID_VALUE);
    auto it = m->streams.find(A1);
    if (it != m->streams.end()) {
        if (it->second.type == STREAM_AVC) {
            m->demux.setVideoStream(-1);
            m->haveVideoAu = false;
        } else if (it->second.type == STREAM_ATRAC || it->second.type == STREAM_AUDIO) {
            m->demux.setAudioStream(-1);
            m->haveAudioAu = false;
        }
        m->streams.erase(it);
    }
    RET(0);
}

void sceMpegMallocAvcEsBuf(PspContext* c) {
    MpegCtx* m = getCtx(A0);
    if (!m) return (void)RET(0);
    for (int i = 0; i < kMaxEsBuffers; i++)
        if (!m->esBuf[i]) {
            m->esBuf[i] = true;
            return (void)RET(i + 1);
        }
    RET(0);
}

void sceMpegFreeAvcEsBuf(PspContext* c) {
    MpegCtx* m = getCtx(A0);
    if (!m) return (void)RET(ERR_MPEG_INVALID_VALUE);
    const int buf = (int)A1;
    if (buf == 0) return (void)RET(ERR_MPEG_INVALID_VALUE);
    if (buf >= 1 && buf <= kMaxEsBuffers) m->esBuf[buf - 1] = false;
    RET(0);
}

void sceMpegInitAu(PspContext* c) {
    MpegCtx* m = getCtx(A0);
    const u32 esBuf = A1, auAddr = A2;
    if (!m) return (void)RET(ERR_MPEG_INVALID_VALUE);
    if (!memValid(auAddr)) return (void)RET(ERR_MPEG_INVALID_ADDR);
    GuestAu au{};
    au.esBuffer = esBuf;
    if (esBuf >= 1 && esBuf <= (u32)kMaxEsBuffers && m->esBuf[esBuf - 1]) {
        au.esSize = kAvcEsSize;  // AVC ES buffer from sceMpegMallocAvcEsBuf
        au.pts = 0;
        au.dts = 0;
    } else {
        au.esSize = kAtracEsSize;  // ATRAC ES buffer (game memory)
        au.pts = 0;
        au.dts = kNoTimestamp;
    }
    writeAu(auAddr, au);
    RET(0);
}

void sceMpegQueryAtracEsSize(PspContext* c) {
    MpegCtx* m = getCtx(A0);
    if (!m) return (void)RET(ERR_MPEG_INVALID_VALUE);
    if (!memValid(A1) || !memValid(A2)) return (void)RET(ERR_MPEG_INVALID_ADDR);
    wr32(A1, kAtracEsSize);
    wr32(A2, kAtracOutputSize);
    RET(0);
}

void sceMpegFlushAllStream(PspContext* c) {
    MpegCtx* m = getCtx(A0);
    if (!m) return (void)RET(ERR_MPEG_INVALID_VALUE);
    LOG_DEBUG("MPEG", "sceMpegFlushAllStream after %d video AUs / %d audio AUs", m->videoAus, m->audioAus);
    resetStreamState(*m, false);
    if (m->ringAddr) {
        wr32(m->ringAddr + RingOff::packetsRead, 0);
        wr32(m->ringAddr + RingOff::packetsWritePos, 0);
        wr32(m->ringAddr + RingOff::packetsAvail, 0);
    }
    RET(0);
}

// ------------------------------------------------------------------ access units
void sceMpegGetAvcAu(PspContext* c) {
    MpegCtx* m = getCtx(A0);
    const u32 sid = A1, auAddr = A2, attrAddr = A3;
    if (!m) return (void)RET(ERR_MPEG_INVALID_VALUE);
    auto it = m->streams.find(sid);
    if (it == m->streams.end() || it->second.type != STREAM_AVC) return (void)RET(ERR_MPEG_INVALID_VALUE);
    if (!memValid(auAddr)) return (void)RET(ERR_MPEG_INVALID_ADDR);
    // An AU fetched earlier but never decoded is skipped by the PSP; still feed it to the decoder
    // so the reference chain stays intact.
    if (m->haveVideoAu && ensureAvcDecoder(*m)) {
        m->avc.decode(m->videoAu.data.data(), m->videoAu.data.size(), m->videoAu.pts);
        VideoFrame dropped;
        while (m->avc.popFrame(dropped)) {
        }
    }
    m->haveVideoAu = false;
    GuestAu au = readAu(auAddr);
    if (!fetchVideoAu(*m, m->videoAu)) {
        if (m->demux.endOfStream() && !m->videoEndLogged) {
            m->videoEndLogged = true;
            LOG_INFO("MPEG", "video stream end after %d AUs", m->videoAus);
        }
        if (m->demux.endOfStream()) {
            au.dts = kNoTimestamp;
            writeAu(auAddr, au);
        }
        delay(kNoDataDelay);
        return (void)RET(ERR_MPEG_NO_DATA);
    }
    m->haveVideoAu = true;
    const s64 base = m->hdrValid ? m->hdr.firstTimestamp : 90000;
    au.pts = tsOrDefault(m->videoAu.pts, base + (s64)m->videoAus * kVideoTimestampStep);
    au.dts = tsOrDefault(m->videoAu.dts, au.pts - kVideoTimestampStep);
    au.esSize = m->videoAu.esSize;
    m->videoAu.pts = au.pts;
    writeAu(auAddr, au);
    m->videoAus++;
    if (attrAddr && memValid(attrAddr)) wr32(attrAddr, 1);
    RET(0);
}

void sceMpegGetAtracAu(PspContext* c) {
    MpegCtx* m = getCtx(A0);
    const u32 sid = A1, auAddr = A2, attrAddr = A3;
    if (!m) return (void)RET(ERR_MPEG_INVALID_VALUE);
    auto it = m->streams.find(sid);
    if (it == m->streams.end() || (it->second.type != STREAM_ATRAC && it->second.type != STREAM_AUDIO))
        return (void)RET(ERR_MPEG_INVALID_VALUE);
    if (!memValid(auAddr)) return (void)RET(ERR_MPEG_INVALID_ADDR);
    GuestAu au = readAu(auAddr);
    m->haveAudioAu = false;
    if (!fetchAudioAu(*m, m->audioAu)) {
        if (m->demux.endOfStream()) {
            if (!m->audioEndLogged) {
                m->audioEndLogged = true;
                LOG_INFO("MPEG", "audio stream end after %d AUs", m->audioAus);
            }
            au.dts = kNoTimestamp;
            writeAu(auAddr, au);
        }
        delay(kNoDataDelay);
        return (void)RET(ERR_MPEG_NO_DATA);
    }
    m->haveAudioAu = true;
    const s64 base = m->hdrValid ? m->hdr.firstTimestamp : 90000;
    au.pts = au.dts = tsOrDefault(m->audioAu.pts, base + (s64)m->audioAus * kAtracSamplesPerAu * 90000 / 44100);
    au.esSize = m->audioAu.esSize;
    writeAu(auAddr, au);
    m->audioAus++;
    if (attrAddr && memValid(attrAddr)) wr32(attrAddr, 0);
    RET(0);
}

// ------------------------------------------------------------------ video decode
void sceMpegAvcDecodeMode(PspContext* c) {
    MpegCtx* m = getCtx(A0);
    if (!m) return (void)RET(ERR_MPEG_INVALID_VALUE);
    if (!memValid(A1)) return (void)RET(ERR_MPEG_INVALID_ADDR);
    const s32 mode = (s32)rd32(A1);
    const s32 pixel = (s32)rd32(A1 + 4);
    if (pixel < 0 || pixel > 3) return (void)RET(ERR_MPEG_AVC_INVALID_VALUE);
    m->decodeMode = mode;
    m->pixelMode = pixel;
    RET(0);
}

void sceMpegAvcDecode(PspContext* c) {
    MpegCtx* m = getCtx(A0);
    const u32 auAddr = A1, bufPtr = A3, initPtr = A4;
    u32 frameWidth = A2;
    if (!m) return (void)RET(ERR_MPEG_INVALID_VALUE);
    if (!memValid(auAddr) || !memValid(bufPtr) || !memValid(initPtr)) return (void)RET(ERR_MPEG_INVALID_ADDR);
    if (frameWidth == 0 || frameWidth > 4096) frameWidth = m->frameWidth ? m->frameWidth : 512;
    const u32 buffer = rd32(bufPtr);
    if (!m->haveVideoAu) {
        // Nothing fetched with sceMpegGetAvcAu since the last decode.
        wr32(initPtr, 0);
        m->frameStatus = 0;
        return (void)RET(ERR_MPEG_AVC_DECODE_FATAL);
    }
    m->haveVideoAu = false;
    bool shown = false;
    if (ensureAvcDecoder(*m)) {
        if (!m->avc.decode(m->videoAu.data.data(), m->videoAu.data.size(), m->videoAu.pts))
            LOG_WARN("MPEG", "AU %d: %s", m->videoAus, m->avc.error().c_str());
        VideoFrame f;
        if (m->avc.popFrame(f)) {
            m->detailWidth = f.width;
            m->detailHeight = f.height;
            m->lastFramePts = f.pts >= 0 ? f.pts : m->videoAu.pts;
            shown = writeFrame(*m, f, buffer, frameWidth);
        }
    } else {
        writeBlackFrame(*m, buffer, frameWidth);
        m->lastFramePts = m->videoAu.pts;
        shown = true;
    }
    if (shown) {
        m->videoFrames++;
        GuestAu au = readAu(auAddr);
        au.pts = m->lastFramePts;
        writeAu(auAddr, au);
    }
    m->frameStatus = shown ? 1 : 0;
    wr32(initPtr, (u32)m->frameStatus);
    delay(kAvcDecodeDelay);
    RET(0);
}

// ---- YCbCr output path. The PSP media engine's YCbCr buffer layout is undocumented, so frames are
// decoded and reported (init/status words, AU timestamps) but no pixels are written.
//
// Valkyria Chronicles III imports these because its CRI movie library (player context at
// 0x08d56678) supports an output format 5 = YCbCr, but the game never selects it: the only player
// creation (f_08b16f68 -> f_08c33920) zeroes the 0x24-byte parameter block, so format (param+0x20,
// copied to ctx+0x54) is 0. Formats 0/1 use sceMpegAvcDecode with pixel mode 3 (2 -> 0, 3 -> 1,
// 4 -> 2; table at 0x08cd263c) and the frame is copied to a texture as frameWidth*height*4 bytes
// (f_08b16eb8). Nothing in the game reads a YCbCr buffer, so it defines no layout to match.
// For reference, the library's format-5 path is: QueryYCbCrSize(mode 1, w, h) for each frame-pool
// slot, InitYCbCr(mpeg, 1, w, h, slot) (f_08c35404), DecodeYCbCr(mpeg, au, &slot, &init) and on
// init == 1 hands the slot out as a frame (f_08c344bc); DecodeStopYCbCr(mpeg, &slot, &status)
// flushes the last frame when status == 1 (f_08c34688). Any non-zero return is an error.
void sceMpegAvcQueryYCbCrSize(PspContext* c) {
    const u32 width = A2, height = A3, resultAddr = A4;
    if ((width & 15) || (height & 15) || width > 480 || height > 272) return (void)RET(ERR_MPEG_INVALID_VALUE);
    if (memValid(resultAddr)) wr32(resultAddr, (width / 2) * (height / 2) * 6 + 128);
    RET(0);
}
void sceMpegAvcInitYCbCr(PspContext* c) {
    const u32 width = A2, height = A3, buffer = A4;
    if (!getCtx(A0)) return (void)RET(ERR_MPEG_INVALID_VALUE);
    if (!memValid(buffer)) return (void)RET(ERR_MPEG_INVALID_ADDR);
    static bool warned = false;
    if (!warned) {
        warned = true;
        LOG_WARN("MPEG", "YCbCr movie output requested (%ux%u, buffer %08x): frames advance but pixels are not written",
                 width, height, buffer);
    }
    RET(0);
}
void sceMpegAvcDecodeYCbCr(PspContext* c) {
    MpegCtx* m = getCtx(A0);
    const u32 auAddr = A1, initPtr = A3;
    if (!m) return (void)RET(ERR_MPEG_INVALID_VALUE);
    if (!memValid(auAddr) || !memValid(initPtr)) return (void)RET(ERR_MPEG_INVALID_ADDR);
    if (!m->haveVideoAu) {
        wr32(initPtr, 0);
        m->frameStatus = 0;
        return (void)RET(ERR_MPEG_AVC_DECODE_FATAL);
    }
    m->haveVideoAu = false;
    bool shown = false;
    if (ensureAvcDecoder(*m)) {
        m->avc.decode(m->videoAu.data.data(), m->videoAu.data.size(), m->videoAu.pts);
        VideoFrame f;
        if (m->avc.popFrame(f)) {
            m->detailWidth = f.width;
            m->detailHeight = f.height;
            m->lastFramePts = f.pts >= 0 ? f.pts : m->videoAu.pts;
            shown = true;
        }
    } else {
        m->lastFramePts = m->videoAu.pts;
        shown = true;
    }
    if (shown) {
        m->videoFrames++;
        GuestAu au = readAu(auAddr);
        au.pts = m->lastFramePts;
        writeAu(auAddr, au);
    }
    m->frameStatus = shown ? 1 : 0;
    wr32(initPtr, (u32)m->frameStatus);
    delay(kAvcDecodeDelay);
    RET(0);
}
// Like sceMpegAvcDecodeStop: status 1 if the decoder still held a frame (the caller then shows it).
void sceMpegAvcDecodeStopYCbCr(PspContext* c) {
    MpegCtx* m = getCtx(A0);
    const u32 bufPtr = A1, statusPtr = A2;
    if (!m) return (void)RET(ERR_MPEG_INVALID_VALUE);
    if (!memValid(bufPtr) || !memValid(statusPtr)) return (void)RET(ERR_MPEG_INVALID_ADDR);
    u32 status = 0;
    if (m->avc.isOpen()) {
        m->avc.drain();
        VideoFrame f;
        if (m->avc.popFrame(f)) {
            status = 1;
            m->videoFrames++;
        }
        m->avc.flush();
    }
    wr32(statusPtr, status);
    LOG_INFO("MPEG", "sceMpegAvcDecodeStopYCbCr: %d frames decoded (last frame %s)", m->videoFrames,
             status ? "flushed" : "none");
    RET(0);
}

void sceMpegAvcDecodeDetail(PspContext* c) {
    MpegCtx* m = getCtx(A0);
    const u32 d = A1;
    if (!m) return (void)RET(ERR_MPEG_INVALID_VALUE);
    if (!memValid(d) || !memValid(d + 35)) return (void)RET(ERR_MPEG_INVALID_ADDR);
    wr32(d + 0, 1);  // decode result: success
    wr32(d + 4, (u32)m->videoFrames);
    wr32(d + 8, (u32)m->detailWidth);
    wr32(d + 12, (u32)m->detailHeight);
    wr32(d + 16, 0);  // crop rect left/right/top/bottom
    wr32(d + 20, 0);
    wr32(d + 24, 0);
    wr32(d + 28, 0);
    wr32(d + 32, (u32)m->frameStatus);
    RET(0);
}

// Outputs the last frame still held by the decoder (end of movie).
void sceMpegAvcDecodeStop(PspContext* c) {
    MpegCtx* m = getCtx(A0);
    u32 frameWidth = A1;
    const u32 bufPtr = A2, statusPtr = A3;
    if (!m) return (void)RET(ERR_MPEG_INVALID_VALUE);
    if (!memValid(bufPtr) || !memValid(statusPtr)) return (void)RET(ERR_MPEG_INVALID_ADDR);
    if (frameWidth == 0 || frameWidth > 4096) frameWidth = m->frameWidth ? m->frameWidth : 512;
    u32 status = 0;
    if (m->avc.isOpen()) {
        m->avc.drain();
        VideoFrame f;
        if (m->avc.popFrame(f) && writeFrame(*m, f, rd32(bufPtr), frameWidth)) {
            status = 1;
            m->videoFrames++;
        }
        m->avc.flush();
    }
    wr32(statusPtr, status);
    LOG_INFO("MPEG", "sceMpegAvcDecodeStop: %d frames shown (last frame %s)", m->videoFrames, status ? "flushed" : "none");
    RET(0);
}

void sceMpegAvcDecodeFlush(PspContext* c) {
    MpegCtx* m = getCtx(A0);
    if (!m) return (void)RET(ERR_MPEG_INVALID_VALUE);
    if (m->avc.isOpen()) m->avc.flush();
    m->haveVideoAu = false;
    RET(0);
}

// ------------------------------------------------------------------ audio decode
void sceMpegAtracDecode(PspContext* c) {
    MpegCtx* m = getCtx(A0);
    const u32 buffer = A2;
    if (!m) return (void)RET(ERR_MPEG_INVALID_VALUE);
    if (!memValid(buffer) || !memValid(buffer + kAtracOutputSize - 1)) return (void)RET(ERR_MPEG_INVALID_ADDR);
    s16* out = (s16*)memPtr(buffer);
    if (m->haveAudioAu) {
        if (!m->atrac || m->atrac->params().frameBytes != (int)m->audioAu.data.size()) {
            AtracCodecParams p;
            p.codec = AtracCodec::Atrac3Plus;
            p.frameBytes = (int)m->audioAu.data.size();
            // PSMF frame header byte 2: bits 2..4 = channel configuration (1 mono, 2 stereo)
            p.channels = ((m->audioAu.frameHeader[2] >> 2) & 7) == 1 ? 1 : 2;
            p.extra.assign(m->audioAu.frameHeader, m->audioAu.frameHeader + 8);
            m->atrac = createAtracDecoder(p);
            LOG_INFO("MPEG", "movie audio: ATRAC3plus %d ch, %d-byte frames, decoder '%s'", p.channels, p.frameBytes,
                     m->atrac->name());
        }
        m->atrac->decodeFrame(m->audioAu.data.data(), m->audioAu.data.size(), out, 2);
        m->haveAudioAu = false;
    } else {
        memset(out, 0, kAtracOutputSize);
    }
    delay(kAtracDecodeDelay);
    RET(0);
}

const HleFunction kMpeg[] = {
    {0x0E3C2E9D, "sceMpegAvcDecode", sceMpegAvcDecode},
    {0x0F6C18D7, "sceMpegAvcDecodeDetail", sceMpegAvcDecodeDetail},
    {0x13407F13, "sceMpegRingbufferDestruct", sceMpegRingbufferDestruct},
    {0x167AFD9E, "sceMpegInitAu", sceMpegInitAu},
    {0x21FF80E4, "sceMpegQueryStreamOffset", sceMpegQueryStreamOffset},
    {0x37295ED8, "sceMpegRingbufferConstruct", sceMpegRingbufferConstruct},
    {0x42560F23, "sceMpegRegistStream", sceMpegRegistStream},
    {0x4571CC64, "sceMpegAvcDecodeFlush", sceMpegAvcDecodeFlush},
    {0x591A4AA2, "sceMpegUnRegistStream", sceMpegUnRegistStream},
    {0x606A4649, "sceMpegDelete", sceMpegDelete},
    {0x611E9E11, "sceMpegQueryStreamSize", sceMpegQueryStreamSize},
    {0x682A619B, "sceMpegInit", sceMpegInit},
    {0x707B7629, "sceMpegFlushAllStream", sceMpegFlushAllStream},
    {0x740FCCD1, "sceMpegAvcDecodeStop", sceMpegAvcDecodeStop},
    {0x800C44DF, "sceMpegAtracDecode", sceMpegAtracDecode},
    {0x874624D6, "sceMpegFinish", sceMpegFinish},
    {0xA11C7026, "sceMpegAvcDecodeMode", sceMpegAvcDecodeMode},
    {0xA780CF7E, "sceMpegMallocAvcEsBuf", sceMpegMallocAvcEsBuf},
    {0xB240A59E, "sceMpegRingbufferPut", sceMpegRingbufferPut},
    {0xB5F6DC87, "sceMpegRingbufferAvailableSize", sceMpegRingbufferAvailableSize},
    {0xC132E22F, "sceMpegQueryMemSize", sceMpegQueryMemSize},
    {0xCEB870B1, "sceMpegFreeAvcEsBuf", sceMpegFreeAvcEsBuf},
    {0xD7A29F46, "sceMpegRingbufferQueryMemSize", sceMpegRingbufferQueryMemSize},
    {0xD8C5F121, "sceMpegCreate", sceMpegCreate},
    {0xE1CE83A7, "sceMpegGetAtracAu", sceMpegGetAtracAu},
    {0xF8DCB679, "sceMpegQueryAtracEsSize", sceMpegQueryAtracEsSize},
    {0xFE246728, "sceMpegGetAvcAu", sceMpegGetAvcAu},
    {0x211A057C, "sceMpegAvcQueryYCbCrSize", sceMpegAvcQueryYCbCrSize},
    {0x67179B1B, "sceMpegAvcInitYCbCr", sceMpegAvcInitYCbCr},
    {0xF0EB1125, "sceMpegAvcDecodeYCbCr", sceMpegAvcDecodeYCbCr},
    {0xF2930C9C, "sceMpegAvcDecodeStopYCbCr", sceMpegAvcDecodeStopYCbCr},
};

}  // namespace

HLE_MODULE("sceMpeg", kMpeg);

}  // namespace vc2rt
