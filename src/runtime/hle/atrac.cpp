// sceAtrac3plus: the high-level ATRAC3 / ATRAC3plus player API (RIFF/WAVE "at3"/"at3+" files).
//
// Implements the library's buffer state machine faithfully - full / half-way / streamed buffers,
// loop points, skipped priming frames, remaining-frame and sample accounting, stream-data requests
// and reset info - and decodes frames through the pluggable media::AtracDecoder (silence until a
// real decoder is installed). Output is always interleaved stereo s16, as with SetDataAndGetID.
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

#include "../kernel/kernel.h"
#include "../media/atrac_decoder.h"
#include "hle.h"

namespace vc2rt {
namespace {

using namespace media;

enum : u32 {
    ERR_ATRAC_API_FAIL = 0x80630002,
    ERR_ATRAC_NO_ATRACID = 0x80630003,
    ERR_ATRAC_BAD_ATRACID = 0x80630005,
    ERR_ATRAC_UNKNOWN_FORMAT = 0x80630006,
    ERR_ATRAC_BAD_CODEC_PARAMS = 0x80630008,
    ERR_ATRAC_ALL_DATA_LOADED = 0x80630009,
    ERR_ATRAC_NO_DATA = 0x80630010,
    ERR_ATRAC_SIZE_TOO_SMALL = 0x80630011,
    ERR_ATRAC_SECOND_BUFFER_NEEDED = 0x80630012,
    ERR_ATRAC_BAD_ALIGNMENT = 0x80630014,
    ERR_ATRAC_BAD_SAMPLE = 0x80630015,
    ERR_ATRAC_ADD_DATA_IS_TOO_BIG = 0x80630018,
    ERR_ATRAC_NO_LOOP_INFORMATION = 0x80630021,
    ERR_ATRAC_BUFFER_IS_EMPTY = 0x80630023,
    ERR_ATRAC_ALL_DATA_DECODED = 0x80630024,
};

// GetRemainFrame special values
constexpr s32 kAllDataOnMemory = -1;
constexpr s32 kNonLoopStreamDataOnMemory = -2;
constexpr s32 kLoopStreamDataOnMemory = -3;

enum class State : u8 {
    NoData = 1,
    AllDataLoaded = 2,
    HalfwayBuffer = 3,
    StreamedWithoutLoop = 4,
    StreamedLoopFromEnd = 5,
    StreamedLoopWithTrailer = 6,
};
bool isStreamed(State s) { return ((u8)s & 4) != 0; }

constexpr int kMaxIds = 6;
constexpr u64 kDecodeDelay = 1000;  // microseconds, roughly the PSP's per-frame decode time
constexpr int kOutputChannels = 2;

// ------------------------------------------------------------------ RIFF / WAVE parsing
struct WaveInfo {
    AtracCodec codec = AtracCodec::Atrac3Plus;
    int channels = 0;
    int blockAlign = 0;      // bytes per encoded frame
    u32 dataOff = 0;         // file offset of the first frame
    u32 dataSize = 0;
    s32 endSample = 0;       // from 'fact' (0 = derive from the data size)
    s32 firstSampleOffset = 0;
    s32 loopStart = -1, loopEnd = -1;
    std::vector<u8> fmtExtra;
};

constexpr u8 kAt3PlusGuid[16] = {0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44,
                                 0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62};

u32 le32(const u8* p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
u16 le16(const u8* p) { return (u16)(p[0] | p[1] << 8); }
constexpr u32 fourcc(char a, char b, char c, char d) { return (u32)a | (u32)b << 8 | (u32)c << 16 | (u32)d << 24; }

u32 parseWave(const u8* d, u32 len, WaveInfo& w) {
    if (len < 12 || le32(d) != fourcc('R', 'I', 'F', 'F') || le32(d + 8) != fourcc('W', 'A', 'V', 'E'))
        return len < 12 ? ERR_ATRAC_SIZE_TOO_SMALL : ERR_ATRAC_UNKNOWN_FORMAT;
    u32 off = 12;
    bool haveFmt = false, extendedFact = false;
    for (;;) {
        if (off + 8 > len) return ERR_ATRAC_SIZE_TOO_SMALL;
        const u32 id = le32(d + off);
        const u32 size = (le32(d + off + 4) + 1) & ~1u;
        const u8* c = d + off + 8;
        const u32 body = off + 8;
        if (id == fourcc('d', 'a', 't', 'a')) {
            if (!haveFmt) return ERR_ATRAC_UNKNOWN_FORMAT;
            w.dataOff = body;
            w.dataSize = le32(d + off + 4);
            if (w.firstSampleOffset == 0) w.firstSampleOffset = w.codec == AtracCodec::Atrac3 ? 0x400 : 0x800;
            if (extendedFact && w.codec == AtracCodec::Atrac3Plus) {
                // Files with the extended 'fact' chunk count their delay from a different origin.
                w.firstSampleOffset -= 0xB8;
                if (w.loopEnd >= 0) {
                    w.loopStart -= 0xB8;
                    w.loopEnd -= 0xB8;
                }
            }
            return 0;
        }
        if (body + size > len) return ERR_ATRAC_SIZE_TOO_SMALL;
        if (id == fourcc('f', 'm', 't', ' ')) {
            if (haveFmt || size < 0x20) return ERR_ATRAC_UNKNOWN_FORMAT;
            const u16 tag = le16(c);
            w.channels = le16(c + 2);
            const u32 rate = le32(c + 4);
            w.blockAlign = le16(c + 12);
            if ((w.channels != 1 && w.channels != 2) || rate != 44100 || w.blockAlign == 0) return ERR_ATRAC_UNKNOWN_FORMAT;
            if (tag == 0xFFFE) {  // WAVE_FORMAT_EXTENSIBLE with the ATRAC3plus sub-format
                if (size < 0x34 || memcmp(c + 24, kAt3PlusGuid, 16) != 0) return ERR_ATRAC_UNKNOWN_FORMAT;
                w.codec = AtracCodec::Atrac3Plus;
                if (((c[0x2A] >> 2) & 7) != w.channels) return ERR_ATRAC_UNKNOWN_FORMAT;
            } else if (tag == 0x0270) {  // ATRAC3
                w.codec = AtracCodec::Atrac3;
            } else {
                return ERR_ATRAC_UNKNOWN_FORMAT;
            }
            w.fmtExtra.assign(c + 16, c + size);
            haveFmt = true;
        } else if (id == fourcc('f', 'a', 'c', 't')) {
            if (size < 4) return ERR_ATRAC_UNKNOWN_FORMAT;
            w.endSample = (s32)le32(c);
            if (size == 8) {
                w.firstSampleOffset = (s32)le32(c + 4);
            } else if (size >= 12) {
                w.firstSampleOffset = (s32)le32(c + 8);
                extendedFact = true;
            }
        } else if (id == fourcc('s', 'm', 'p', 'l')) {
            if (w.loopStart < 0) {
                if (size < 0x20) return ERR_ATRAC_UNKNOWN_FORMAT;
                if (le32(c + 0x1C) != 0) {
                    if (size < 0x34) return ERR_ATRAC_SIZE_TOO_SMALL;
                    w.loopStart = (s32)le32(c + 0x2C);
                    w.loopEnd = (s32)le32(c + 0x30);
                    if (w.loopEnd <= w.loopStart) return ERR_ATRAC_BAD_CODEC_PARAMS;
                }
            }
        }
        off = body + size;
    }
}

// ------------------------------------------------------------------ player context
struct ResetInfo {
    u32 writePos, writable, minWrite, filePos;
};

struct Atrac {
    State state = State::NoData;
    AtracCodec codec = AtracCodec::Atrac3Plus;
    int numChan = 2;
    int frameBytes = 0;    // "sampleSize" in the library: bytes per frame
    s32 decodePos = 0;     // next sample to decode (includes the codec delay)
    s32 endSample = 0;     // last sample index
    s32 loopStart = 0, loopEnd = 0;  // 0/0 = no loop
    s32 firstValidSample = 0;
    int numSkipFrames = 0; // frames to decode and discard before output
    s32 loopNum = 0;
    u32 dataOff = 0, curFileOff = 0, fileDataEnd = 0;
    s32 streamDataByte = 0;  // buffered, not yet decoded
    u32 streamOff = 0;       // buffer offset of the next frame (streamed modes)
    u32 buffer = 0, bufferByte = 0;
    std::unique_ptr<AtracDecoder> decoder;
    std::vector<s16> scratch;
    u64 framesDecoded = 0;

    int spf() const { return atracSamplesPerFrame(codec); }
    int delaySamples() const { return codec == AtracCodec::Atrac3 ? 0x45 : 0x170; }

    // Frames that must be decoded and dropped when starting at sample `pos`.
    int skipFramesFor(s32 pos) const { return (pos & (spf() - 1)) < delaySamples() ? 2 : 1; }
    // File offset of the first frame needed to produce sample `pos` (the decoder needs one frame
    // of history, two if `pos` falls inside the codec delay).
    s32 fileOffsetFor(s32 pos) const {
        s32 o = (pos / spf() - 1) * frameBytes;
        if ((pos & (spf() - 1)) < delaySamples() && o != 0) o -= frameBytes;
        return o + (s32)dataOff;
    }
    s32 loopEndFileOffset() const { return (loopEnd / spf() + 1) * frameBytes + (s32)dataOff; }
    s32 loopStartFileOffset() const { return fileOffsetFor(loopStart); }
    // End of the whole-frame area of the ring buffer when frames are laid out from `origin`.
    s32 gridEnd(u32 origin) const {
        if ((s32)bufferByte <= (s32)origin) return (s32)bufferByte;
        return (s32)origin + (((s32)bufferByte - (s32)origin) / frameBytes) * frameBytes;
    }

    u32 nextSamples() const {
        const s32 frameEnd = (decodePos | (spf() - 1));
        const s32 last = std::min(frameEnd, endSample);
        return (u32)std::max(0, last - decodePos + 1);
    }

    s32 remainStreamNoLoop() const {
        if (streamDataByte >= (s32)fileDataEnd - (s32)curFileOff) return kNonLoopStreamDataOnMemory;
        return std::max(0, streamDataByte / frameBytes - numSkipFrames);
    }

    s32 remainStreamLooped() const {
        const s32 ls = loopStartFileOffset(), le = loopEndFileOffset();
        const s32 writeFO = (s32)curFileOff + streamDataByte;  // unwrapped file position of the write head
        s32 frames;
        if (writeFO <= le) {
            frames = streamDataByte / frameBytes;
        } else {
            // Data past the loop end is the loop body again (each pass starts with skip frames).
            const s32 loopLen = le - ls;
            const s32 beyond = writeFO - le;
            const int skip = skipFramesFor(loopStart);
            frames = (le - (s32)curFileOff) / frameBytes + (beyond / loopLen) * (loopLen / frameBytes - skip);
            const s32 part = beyond % loopLen;
            if (part > skip * frameBytes) frames += part / frameBytes - skip;
        }
        frames = std::max(0, frames - numSkipFrames);
        if (loopNum >= 0 && writeFO >= le) {
            const s32 bufferedLoops = (writeFO - le) / (le - ls);
            if (loopNum <= bufferedLoops) return kLoopStreamDataOnMemory;
        }
        return frames;
    }

    s32 remainFrames() const {
        switch (state) {
            case State::NoData: return 0;
            case State::AllDataLoaded: return kAllDataOnMemory;
            case State::HalfwayBuffer: {
                const s32 written = (s32)dataOff + streamDataByte;
                return written > (s32)curFileOff ? std::max(0, (written - (s32)curFileOff) / frameBytes - numSkipFrames) : 0;
            }
            case State::StreamedWithoutLoop: return remainStreamNoLoop();
            case State::StreamedLoopFromEnd: return remainStreamLooped();
            case State::StreamedLoopWithTrailer: return decodePos <= loopEnd ? remainStreamLooped() : remainStreamNoLoop();
        }
        return 0;
    }

    // Where the game should write more data, how much, and from which file offset.
    void streamDataInfo(u32& writePtr, u32& writable, u32& readOffset) const {
        if (state == State::AllDataLoaded) {
            writePtr = buffer;
            writable = 0;
            readOffset = 0;
            return;
        }
        if (state == State::HalfwayBuffer) {
            const u32 fo = dataOff + (u32)streamDataByte;  // buffer is a direct image of the file
            writePtr = buffer + fo;
            writable = fileDataEnd - fo;
            readOffset = fo;
            return;
        }
        const s32 end = gridEnd(streamOff);
        const s32 head = (s32)streamOff + streamDataByte;
        const bool wrapped = head >= end;
        const s32 space = std::max(0, wrapped ? end - streamDataByte : end - head);
        const u32 writeOff = (u32)(wrapped ? head - end : head);
        const s32 fileHead = (s32)curFileOff + streamDataByte;
        const bool looping = state == State::StreamedLoopFromEnd ||
                             (state == State::StreamedLoopWithTrailer && decodePos <= loopEnd);
        if (looping) {
            const s32 ls = loopStartFileOffset(), le = loopEndFileOffset(), len = le - ls;
            const s32 untilLoopEnd = fileHead >= le ? len - (fileHead - le) % len : le - fileHead;
            writable = (u32)std::min(untilLoopEnd, space);
            readOffset = (u32)(fileHead >= le ? ls + (fileHead - le) % len : fileHead);
        } else {
            writable = (u32)std::clamp((s32)fileDataEnd - fileHead, 0, space);
            readOffset = fileHead < (s32)fileDataEnd ? (u32)fileHead : 0;
        }
        writePtr = buffer + (fileHead < (s32)fileDataEnd || looping ? writeOff : 0);
    }

    ResetInfo resetInfo(s32 pos) const {
        ResetInfo r{buffer, 0, 0, 0};
        switch (state) {
            case State::NoData:
            case State::AllDataLoaded: break;
            case State::HalfwayBuffer: {
                const s32 written = (s32)dataOff + streamDataByte;
                const s32 need = (s32)dataOff + (pos / spf() + 1) * frameBytes;
                r.writePos = buffer + (u32)written;
                r.writable = fileDataEnd - (u32)written;
                r.filePos = (u32)written;
                r.minWrite = (u32)std::max(0, need - written);
                break;
            }
            case State::StreamedWithoutLoop:
            case State::StreamedLoopFromEnd:
            case State::StreamedLoopWithTrailer: {
                const s32 fo = fileOffsetFor(pos);
                const s32 bufEnd = (s32)bufferByte - (s32)bufferByte % frameBytes;
                s32 limit = (s32)fileDataEnd - fo;
                if (state == State::StreamedLoopWithTrailer && fo < loopEndFileOffset()) limit = loopEndFileOffset() - fo;
                r.writable = (u32)std::max(0, std::min(limit, bufEnd));
                r.minWrite = (u32)std::min((skipFramesFor(pos) + 1) * frameBytes, std::max(0, limit));
                r.filePos = (u32)fo;
                break;
            }
        }
        return r;
    }

    // Decodes (or skips) one frame. `out` may be null while skipping.
    u32 decodeOne(s16* out, u32& samples, u32& finish) {
        samples = 0;
        if (curFileOff + (u32)frameBytes > fileDataEnd || decodePos > endSample) {
            finish = 1;
            return ERR_ATRAC_ALL_DATA_DECODED;
        }
        if (state == State::StreamedLoopWithTrailer && decodePos > loopEnd) {
            // The tail after the loop needs sceAtracSetSecondBuffer, which this game never uses.
            finish = 1;
            return ERR_ATRAC_ALL_DATA_DECODED;
        }
        if (isStreamed(state) && streamDataByte < frameBytes) {
            finish = 0;
            return ERR_ATRAC_BUFFER_IS_EMPTY;
        }
        if (state == State::HalfwayBuffer && (s32)dataOff + streamDataByte < (s32)(curFileOff + (u32)frameBytes)) {
            finish = 0;
            return ERR_ATRAC_BUFFER_IS_EMPTY;
        }
        const u32 in = buffer + (isStreamed(state) ? streamOff : curFileOff);
        if (!memValid(in) || !memValid(in + (u32)frameBytes - 1)) return ERR_ATRAC_API_FAIL;
        const u32 n = nextSamples();
        scratch.resize((size_t)spf() * kOutputChannels);
        decoder->decodeFrame(memPtr(in), (size_t)frameBytes, scratch.data(), kOutputChannels);
        framesDecoded++;
        curFileOff += (u32)frameBytes;
        if (numSkipFrames == 0) {
            // Output starts at decodePos's position inside the frame.
            const int first = decodePos & (spf() - 1);
            if (out && n) memcpy(out, scratch.data() + (size_t)first * kOutputChannels, (size_t)n * kOutputChannels * 2);
            samples = n;
            finish = (endSample < decodePos + (s32)n && loopNum == 0) ? 1 : 0;
            decodePos += (s32)n;
            if (loopEnd != 0 && loopNum != 0 && decodePos > loopEnd) {
                curFileOff = (u32)loopStartFileOffset();
                numSkipFrames = skipFramesFor(loopStart);
                decodePos = loopStart;
                if (loopNum > 0) loopNum--;
                decoder->reset();
            }
        } else {
            numSkipFrames--;
        }
        if (isStreamed(state)) {
            streamDataByte -= frameBytes;
            streamOff += (u32)frameBytes;
            if (streamOff + (u32)frameBytes > bufferByte) streamOff = 0;
        }
        return 0;
    }

    u32 skipPending() {
        u32 s, f;
        while (numSkipFrames > 0) {
            const u32 r = decodeOne(nullptr, s, f);
            if (r) return r;
        }
        return 0;
    }

    u32 setData(u32 buf, u32 readSize, u32 bufSize) {
        if (!memValid(buf)) return kernel::ERR_ILLEGAL_ADDR;
        WaveInfo w;
        u32 r = parseWave(memPtr(buf), readSize, w);
        if (r) return r;
        if (w.blockAlign <= 0 || (u32)w.blockAlign > bufSize) return ERR_ATRAC_BAD_CODEC_PARAMS;
        codec = w.codec;
        numChan = w.channels;
        frameBytes = w.blockAlign;
        firstValidSample = w.firstSampleOffset + delaySamples();
        decodePos = firstValidSample;
        endSample = (w.endSample ? w.endSample + firstValidSample : (s32)(w.dataSize / (u32)frameBytes) * spf()) - 1;
        numSkipFrames = firstValidSample / spf();
        loopNum = 0;
        if (w.loopStart >= 0) {
            loopStart = w.loopStart + delaySamples();
            loopEnd = w.loopEnd + delaySamples();
        } else {
            loopStart = loopEnd = 0;
        }
        if (loopEnd > endSample) return ERR_ATRAC_BAD_CODEC_PARAMS;
        if ((u64)(endSample / spf()) * (u64)frameBytes >= w.dataSize) return ERR_ATRAC_BAD_CODEC_PARAMS;
        dataOff = w.dataOff;
        curFileOff = dataOff;
        fileDataEnd = w.dataOff + w.dataSize;
        buffer = buf;
        bufferByte = bufSize;
        streamOff = dataOff;
        streamDataByte = (s32)readSize - (s32)dataOff;
        if (bufSize >= fileDataEnd) {
            state = readSize >= fileDataEnd ? State::AllDataLoaded : State::HalfwayBuffer;
        } else {
            if (streamDataByte < frameBytes * 2) return ERR_ATRAC_SIZE_TOO_SMALL;
            if (loopEnd == 0) {
                state = State::StreamedWithoutLoop;
            } else if (loopEnd == endSample) {
                state = State::StreamedLoopFromEnd;
            } else {
                state = State::StreamedLoopWithTrailer;
                streamDataByte = std::min(streamDataByte, loopEndFileOffset() - (s32)dataOff + 1);
            }
        }
        AtracCodecParams p;
        p.codec = codec;
        p.channels = numChan;
        p.frameBytes = frameBytes;
        p.extra = w.fmtExtra;
        decoder = createAtracDecoder(p);
        // Prime the decoder with the frames that precede the first valid sample (if loaded).
        r = skipPending();
        if (r == ERR_ATRAC_BUFFER_IS_EMPTY) r = 0;
        wrapPartialFrame();
        return r;
    }

    // The initial read may end with part of a frame beyond the last whole-frame slot; the library
    // moves those bytes to the start of the ring where the frame continues.
    void wrapPartialFrame() {
        if (!isStreamed(state)) return;
        const s32 end = gridEnd(streamOff);
        if ((s32)streamOff + streamDataByte < end) return;
        const u32 len = bufferByte - (u32)end;
        if (len && memValid(buffer + (u32)end) && memValid(buffer + bufferByte - 1)) memmove(memPtr(buffer), memPtr(buffer + (u32)end), len);
    }
};

std::array<std::unique_ptr<Atrac>, kMaxIds> g_atrac;

Atrac* getAtrac(u32 id) { return id < (u32)kMaxIds ? g_atrac[id].get() : nullptr; }

// Common argument validation of the library: unknown id, then "no data yet".
u32 validate(u32 id, Atrac*& a) {
    a = getAtrac(id);
    if (!a) return ERR_ATRAC_BAD_ATRACID;
    if (a->state == State::NoData) return ERR_ATRAC_NO_DATA;
    return 0;
}

void write32If(u32 addr, u32 v) {
    if (addr && memValid(addr)) wr32(addr, v);
}

// ------------------------------------------------------------------ API
void sceAtracSetDataAndGetID(PspContext* c) {
    const u32 buf = A0, size = A1;
    int id = -1;
    for (int i = 0; i < kMaxIds; i++)
        if (!g_atrac[i]) {
            id = i;
            break;
        }
    if (id < 0) return (void)RET(ERR_ATRAC_NO_ATRACID);
    auto a = std::make_unique<Atrac>();
    const u32 r = a->setData(buf, size, size);
    if (r != 0 && r != ERR_ATRAC_API_FAIL) {
        LOG_WARN("ATRAC", "sceAtracSetDataAndGetID(%08x, %u) failed: %08x", buf, size, r);
        return (void)RET(r);
    }
    LOG_INFO("ATRAC", "id %d: %s %dch, %d-byte frames, samples %d..%d, loop %d..%d, data %u..%u, buffer %08x+%u, state %d, "
             "decoder '%s'",
             id, a->codec == AtracCodec::Atrac3 ? "ATRAC3" : "ATRAC3plus", a->numChan, a->frameBytes, a->firstValidSample,
             a->endSample, a->loopStart, a->loopEnd, a->dataOff, a->fileDataEnd, buf, size, (int)a->state, a->decoder->name());
    g_atrac[id] = std::move(a);
    RET(id);
}

void sceAtracReleaseAtracID(PspContext* c) {
    const u32 id = A0;
    if (!getAtrac(id)) return (void)RET(ERR_ATRAC_BAD_ATRACID);
    LOG_DEBUG("ATRAC", "release id %u (%llu frames decoded)", id, (unsigned long long)g_atrac[id]->framesDecoded);
    g_atrac[id].reset();
    RET(0);
}

void sceAtracDecodeData(PspContext* c) {
    const u32 id = A0, outAddr = A1, samplesAddr = A2, finishAddr = A3, remainAddr = A4;
    Atrac* a;
    if (u32 e = validate(id, a)) return (void)RET(e);
    if (outAddr & 1) return (void)RET(ERR_ATRAC_BAD_ALIGNMENT);
    const u32 outBytes = (u32)a->spf() * kOutputChannels * 2;
    if (outAddr && (!memValid(outAddr) || !memValid(outAddr + outBytes - 1))) return (void)RET(ERR_ATRAC_SIZE_TOO_SMALL);
    s16* out = outAddr ? (s16*)memPtr(outAddr) : nullptr;
    u32 samples = 0, finish = 0, r = 0;
    for (int tries = a->numSkipFrames + 1; tries > 0; tries--) {
        r = a->decodeOne(out, samples, finish);
        if (r) {
            samples = 0;
            break;
        }
    }
    write32If(samplesAddr, samples);
    write32If(finishAddr, finish);
    if (r == 0) write32If(remainAddr, (u32)a->remainFrames());
    if (r == 0 || r == ERR_ATRAC_API_FAIL) kernel::waitCurrentFor(kernel::WaitType::Delay, 0, kDecodeDelay, false);
    RET(r);
}

void sceAtracGetStreamDataInfo(PspContext* c) {
    const u32 id = A0;
    Atrac* a;
    if (u32 e = validate(id, a)) return (void)RET(e);
    u32 wp, wb, ro;
    a->streamDataInfo(wp, wb, ro);
    write32If(A1, wp);
    write32If(A2, wb);
    write32If(A3, ro);
    RET(0);
}

void sceAtracAddStreamData(PspContext* c) {
    const u32 id = A0, bytes = A1;
    Atrac* a;
    if (u32 e = validate(id, a)) return (void)RET(e);
    if (a->state == State::AllDataLoaded) return (void)RET(ERR_ATRAC_ALL_DATA_LOADED);
    if (a->state == State::HalfwayBuffer) {
        const u64 end = (u64)a->dataOff + (u64)a->streamDataByte + bytes;
        if (end > a->fileDataEnd) return (void)RET(ERR_ATRAC_ADD_DATA_IS_TOO_BIG);
        if (end == a->fileDataEnd) a->state = State::AllDataLoaded;
    }
    a->streamDataByte += (s32)bytes;
    RET(0);
}

void sceAtracGetRemainFrame(PspContext* c) {
    Atrac* a;
    if (u32 e = validate(A0, a)) return (void)RET(e);
    if (!memValid(A1)) return (void)RET(kernel::ERR_ILLEGAL_ADDR);
    wr32(A1, (u32)a->remainFrames());
    RET(0);
}

void sceAtracGetNextSample(PspContext* c) {
    Atrac* a;
    if (u32 e = validate(A0, a)) return (void)RET(e);
    write32If(A1, a->nextSamples());
    RET(0);
}

void sceAtracGetSoundSample(PspContext* c) {
    Atrac* a;
    if (u32 e = validate(A0, a)) return (void)RET(e);
    write32If(A1, (u32)(a->endSample - a->firstValidSample));
    write32If(A2, a->loopEnd == 0 ? 0xFFFFFFFFu : (u32)(a->loopStart - a->firstValidSample));
    write32If(A3, a->loopEnd == 0 ? 0xFFFFFFFFu : (u32)(a->loopEnd - a->firstValidSample));
    RET(0);
}

void sceAtracGetBufferInfoForResetting(PspContext* c) {
    const u32 id = A0, info = A2;
    const s32 sample = (s32)A1;
    Atrac* a;
    if (u32 e = validate(id, a)) return (void)RET(e);
    if (!memValid(info) || !memValid(info + 31)) return (void)RET(kernel::ERR_ILLEGAL_ADDR);
    if (a->state == State::StreamedLoopWithTrailer) return (void)RET(ERR_ATRAC_SECOND_BUFFER_NEEDED);  // no second buffer
    const s32 pos = sample + a->firstValidSample;
    if ((u32)pos > (u32)a->endSample) return (void)RET(ERR_ATRAC_BAD_SAMPLE);
    const ResetInfo r = a->resetInfo(pos);
    wr32(info + 0, r.writePos);
    wr32(info + 4, r.writable);
    wr32(info + 8, r.minWrite);
    wr32(info + 12, r.filePos);
    // The second buffer never needs data for a reset.
    wr32(info + 16, a->buffer);
    wr32(info + 20, 0);
    wr32(info + 24, 0);
    wr32(info + 28, 0);
    // Like the library, pending priming frames are consumed here.
    const u32 res = a->skipPending();
    RET(res == ERR_ATRAC_BUFFER_IS_EMPTY ? 0 : res);
}

// (id, sample, bytesWrittenFirstBuf, bytesWrittenSecondBuf): seek to `sample`. Before calling,
// the game has written the data requested by sceAtracGetBufferInfoForResetting.
void sceAtracResetPlayPosition(PspContext* c) {
    const u32 id = A0, bytes = A2;
    const s32 sample = (s32)A1;
    Atrac* a;
    if (u32 e = validate(id, a)) return (void)RET(e);
    if (a->state == State::StreamedLoopWithTrailer) return (void)RET(ERR_ATRAC_SECOND_BUFFER_NEEDED);
    const s32 pos = sample + a->firstValidSample;
    if ((u32)pos > (u32)a->endSample) return (void)RET(ERR_ATRAC_BAD_SAMPLE);
    a->decodePos = pos;
    a->numSkipFrames = a->skipFramesFor(pos);
    a->curFileOff = (u32)a->fileOffsetFor(pos);
    switch (a->state) {
        case State::HalfwayBuffer: a->streamDataByte += (s32)bytes; break;
        case State::StreamedWithoutLoop:
        case State::StreamedLoopFromEnd:
            a->streamOff = 0;  // the reset data was written at the start of the ring
            a->streamDataByte = (s32)bytes;
            break;
        default: break;
    }
    if (a->decoder) a->decoder->reset();
    RET(0);
}

void sceAtracGetChannel(PspContext* c) {
    Atrac* a;
    if (u32 e = validate(A0, a)) return (void)RET(e);
    write32If(A1, (u32)a->numChan);
    RET(0);
}

void sceAtracGetBitrate(PspContext* c) {
    Atrac* a;
    if (u32 e = validate(A0, a)) return (void)RET(e);
    // kbps derived from the frame size the same way the library does (44.1 kHz)
    int br = a->frameBytes * 352800 / 1000;
    br = a->codec == AtracCodec::Atrac3Plus ? (((br >> 11) + 8) & ~0xF) : ((br + 511) >> 10);
    write32If(A1, (u32)br);
    RET(0);
}

void sceAtracSetLoopNum(PspContext* c) {
    Atrac* a;
    if (u32 e = validate(A0, a)) return (void)RET(e);
    if (a->loopEnd <= 0) return (void)RET(ERR_ATRAC_NO_LOOP_INFORMATION);
    a->loopNum = (s32)A1;
    RET(0);
}

void sceAtracGetLoopStatus(PspContext* c) {
    Atrac* a;
    if (u32 e = validate(A0, a)) return (void)RET(e);
    write32If(A1, (u32)a->loopNum);
    const u32 status = a->loopEnd == 0 ? 0 : (a->loopNum != 0 ? 1 : (a->decodePos <= a->loopEnd ? 1 : 0));
    write32If(A2, status);
    RET(0);
}

const HleFunction kAtrac[] = {
    {0x2DD3E298, "sceAtracGetBufferInfoForResetting", sceAtracGetBufferInfoForResetting},
    {0x644E5607, "sceAtracResetPlayPosition", sceAtracResetPlayPosition},
    {0x31668BAA, "sceAtracGetChannel", sceAtracGetChannel},
    {0x36FAABFB, "sceAtracGetNextSample", sceAtracGetNextSample},
    {0x5D268707, "sceAtracGetStreamDataInfo", sceAtracGetStreamDataInfo},
    {0x61EB33F5, "sceAtracReleaseAtracID", sceAtracReleaseAtracID},
    {0x6A8C3CD5, "sceAtracDecodeData", sceAtracDecodeData},
    {0x7A20E7AF, "sceAtracSetDataAndGetID", sceAtracSetDataAndGetID},
    {0x7DB31251, "sceAtracAddStreamData", sceAtracAddStreamData},
    {0x868120B5, "sceAtracSetLoopNum", sceAtracSetLoopNum},
    {0x9AE849A7, "sceAtracGetRemainFrame", sceAtracGetRemainFrame},
    {0xA2BBA8BE, "sceAtracGetSoundSample", sceAtracGetSoundSample},
    {0xA554A158, "sceAtracGetBitrate", sceAtracGetBitrate},
    {0xFAA4F89B, "sceAtracGetLoopStatus", sceAtracGetLoopStatus},
};

}  // namespace

HLE_MODULE("sceAtrac3plus", kAtrac);

}  // namespace vc2rt
