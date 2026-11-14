// sceAudio: the PSP's PCM output path, mixed on the host and fed to SDL at 44.1 kHz.
//
// Hardware model (what games observe):
//  * Channels 0-7 hold a single buffer *pointer* each; the driver never copies. Its mixer reads one
//    64-sample DMA block per 64/44100 s straight out of guest memory, scales each channel by its
//    volume (0x8000 = unity, 0xFFFF = max, i.e. about +6 dB), sums in 32 bits and clamps once.
//  * sceAudioOutput* while a buffer is still playing: the non-blocking calls fail with
//    CHANNEL_BUSY; the blocking calls park the thread with its buffer in the channel's one
//    "waiting" slot. When the playing buffer runs out, the waiting buffer starts on the very next
//    block and only then does the thread resume (return value = the channel's sample count). So a
//    blocking caller is released when the previous buffer has been consumed and the new one is
//    queued, which gives the classic double buffering. A second thread arriving while one is
//    already parked gets CHANNEL_BUSY.
//  * The Output2/SRC channel is a separate DMA with two descriptors that bypasses the mixer
//    channels; its first buffer after an idle period returns at once, later ones block until a
//    descriptor completes. sceAudioSRCChReserve can set the input rate (8-48 kHz), resampled
//    here to 44.1 kHz by linear interpolation that runs continuously across buffer boundaries.
//
// Host pacing: the emulated DMA clock is slaved to the host device. Each tick (4 ms) we produce
// just enough 64-sample blocks to bring SDL's queue back up to the configured latency, so the
// long-term rate is exactly the device's rate (no drift, no unbounded latency growth). Guest
// threads cannot run while a tick is mixing, so once a channel's buffer runs out with nothing
// queued behind it ("starving") we stop producing for this tick and let the woken thread refill
// it; the host queue absorbs the pause. A live stream's (high priority) thread refills right after
// the tick that woke it, so a channel still dry after kStarveWaitUs has simply stopped and is
// mixed as silence again. If the queue falls to the low-water mark first, we give up waiting and
// mix starving channels as silence (a late refill; counted as a gap). With no audio device the
// same logic runs against a virtual queue drained by the wall clock.
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <vector>

#include "../host/config.h"
#include "../host/host.h"
#include "../kernel/kernel.h"
#include "audio.h"
#include "hle.h"

namespace vc2rt {

// ------------------------------------------------------------------ WAV dump helper
bool WavDump::open(const char* path, int rate, int channels) {
    close();
    f_ = fopen(path, "wb");
    if (!f_) return false;
    rate_ = rate;
    channels_ = channels;
    frames_ = framesAtHeader_ = 0;
    writeHeader();
    return true;
}

void WavDump::writeHeader() {
    u32 dataBytes = (u32)std::min<u64>(frames_ * channels_ * 2, 0xFFFFFFF0ull);
    u8 h[44];
    auto put32 = [&](int o, u32 v) { for (int i = 0; i < 4; i++) h[o + i] = (u8)(v >> (8 * i)); };
    auto put16 = [&](int o, u16 v) { h[o] = (u8)v; h[o + 1] = (u8)(v >> 8); };
    memcpy(h, "RIFF", 4);
    put32(4, 36 + dataBytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, (u16)channels_);
    put32(24, (u32)rate_);
    put32(28, (u32)(rate_ * channels_ * 2));
    put16(32, (u16)(channels_ * 2));
    put16(34, 16);
    memcpy(h + 36, "data", 4);
    put32(40, dataBytes);
    long pos = ftell(f_);
    fseek(f_, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), f_);
    if (pos > (long)sizeof(h)) fseek(f_, pos, SEEK_SET);
    fflush(f_);
    framesAtHeader_ = frames_;
}

void WavDump::write(const s16* samples, size_t frames) {
    if (!f_ || !frames) return;
    fwrite(samples, 2 * channels_, frames, f_);
    frames_ += frames;
    if (frames_ - framesAtHeader_ >= (u64)rate_) writeHeader();
}

void WavDump::close() {
    if (!f_) return;
    writeHeader();
    fclose(f_);
    f_ = nullptr;
}

bool openDumpFromEnv(WavDump& dump, const char* envVar, int rate, int channels) {
    const char* path = getenv(envVar);
    if (!path || !*path) return false;
    if (!dump.open(path, rate, channels)) {
        LOG_WARN("AUDIO", "%s: cannot write %s", envVar, path);
        return false;
    }
    LOG_INFO("AUDIO", "%s: dumping to %s", envVar, path);
    return true;
}

namespace {
using namespace kernel;

enum : u32 {
    ERR_AUDIO_NOT_INIT = 0x80260001,
    ERR_AUDIO_BUSY = 0x80260002,
    ERR_AUDIO_INVALID_CHANNEL = 0x80260003,
    ERR_AUDIO_NO_CHANNELS = 0x80260005,
    ERR_AUDIO_SIZE_NOT_ALIGNED = 0x80260006,
    ERR_AUDIO_INVALID_FORMAT = 0x80260007,
    ERR_AUDIO_NOT_RESERVED = 0x80260008,
    ERR_AUDIO_INVALID_FREQUENCY = 0x8026000A,
    ERR_AUDIO_INVALID_VOLUME = 0x8026000B,
    ERR_AUDIO_ALREADY_RESERVED = 0x80268002,
    ERR_KERNEL_INVALID_SIZE = 0x80000104,
    ERR_SRC_FORMAT_4 = 0x80000003,
    ERR_KERNEL_ILLEGAL_CONTEXT = 0x80020064,
};

constexpr int kNumChannels = 8;
constexpr u32 kSrcWaitId = 0x100;  // wait id for the Output2/SRC channel
constexpr u32 kBlock = 64;         // hardware DMA block, in sample frames
constexpr u32 kSampleMax = 65536 - 64;
constexpr int kOutRate = 44100;
constexpr u32 kFmtStereo = 0x00, kFmtMono = 0x10;
constexpr u64 kTickMicros = 4000;
constexpr u64 kStarveWaitUs = 12000;  // how long a dry channel may hold the mixer back

struct Channel {
    bool reserved = false;
    u32 sampleCount = 0;
    u32 format = kFmtStereo;
    u32 leftVol = 0, rightVol = 0;
    // the buffer the DMA is playing
    u32 addr = 0;
    u32 remaining = 0;
    // the one parked blocking caller and its buffer
    u32 waitThread = 0;
    u32 waitAddr = 0;
    s32 waitLv = 0, waitRv = 0;
    u64 starveSince = 0;  // host time it ran dry with nothing queued (0 = not starving)
};

struct SrcChannel {
    bool reserved = false;
    u32 sampleCount = 0;
    u32 format = kFmtStereo;
    u32 vol = 0;
    u32 freq = 0;  // 0 = output rate
    struct Buf {
        u32 addr = 0, samples = 0;
    } bufs[2];
    int count = 0;
    u32 played = 0;  // frames of bufs[0] consumed
    u32 frac = 0;    // 16.16 position fraction
    bool completion = false;
    struct Waiter {
        u32 thread;
        s32 result;
    };
    std::deque<Waiter> waiters;
    u64 starveSince = 0;
};

Channel g_ch[kNumChannels];
SrcChannel g_src;

s32 g_mix[kBlock * 2];
std::vector<s16> g_out;
WavDump g_dump;
u64 g_lastTick = 0;
u64 g_tickNow = 1;  // time of the tick being mixed (drain timestamps; never 0)
double g_virtQueued = 0;  // used when no host device is open
bool g_everPushed = false;

struct Stats {
    u64 blocks = 0, underruns = 0, stalls = 0, gaps = 0, ended = 0;
    size_t minQueued = ~(size_t)0, maxQueued = 0;
    u64 lastReport = 0;
    u64 chFrames[kNumChannels + 1] = {};  // frames played per channel (8 = SRC) this period
    int chPeak[kNumChannels + 1] = {};    // peak |sample| per channel this period
} g_stats;

bool threadParkedOn(u32 uid, u32 waitId) {
    Thread* t = uid ? getThread(uid) : nullptr;
    return t && (t->state == ThreadState::Waiting || t->state == ThreadState::WaitSuspended) &&
           t->waitType == WaitType::Audio && t->waitId == waitId;
}

// ------------------------------------------------------------------ mixer channels
s32 enqueue(Channel& ch, u32 addr, s32 lv, s32 rv) {
    if (!ch.reserved) return (s32)ERR_AUDIO_NOT_INIT;
    if (ch.addr) return (s32)ERR_AUDIO_BUSY;  // one buffer slot, no queue behind it
    ch.remaining = ch.sampleCount;
    if (lv >= 0) ch.leftVol = (u32)lv;
    if (rv >= 0) ch.rightVol = (u32)rv;
    ch.addr = addr;  // a null pointer is accepted and leaves the channel idle
    if (addr) ch.starveSince = 0;
    return (s32)ch.sampleCount;
}

s32 output(int idx, s32 lv, s32 rv, u32 addr, bool blocking) {
    Channel& ch = g_ch[idx];
    s32 r = enqueue(ch, addr, lv, rv);
    if (!blocking || r != (s32)ERR_AUDIO_BUSY) return r;
    if (ch.waitThread && threadParkedOn(ch.waitThread, (u32)idx)) return (s32)ERR_AUDIO_BUSY;
    Thread* t = currentThread();
    if (!t) return (s32)ERR_KERNEL_ILLEGAL_CONTEXT;
    ch.waitThread = t->uid;
    ch.waitAddr = addr;
    ch.waitLv = lv;
    ch.waitRv = rv;
    // channelFinished() installs our buffer and supplies the return value.
    return waitCurrent(WaitType::Audio, (u32)idx, 0, false);
}

void channelFinished(int idx) {
    Channel& ch = g_ch[idx];
    ch.addr = 0;
    ch.remaining = 0;
    if (ch.waitThread) {
        u32 uid = ch.waitThread;
        ch.waitThread = 0;
        if (threadParkedOn(uid, (u32)idx)) {
            s32 r = enqueue(ch, ch.waitAddr, ch.waitLv, ch.waitRv);
            wakeThread(getThread(uid), r);
            if (ch.addr) return;  // seamless hand-over
        }
    }
    if (ch.reserved && !ch.starveSince) ch.starveSince = g_tickNow;
}

void mixChannelBlock(int idx) {
    Channel& ch = g_ch[idx];
    if (!ch.addr) return;
    u32 n = std::min(ch.remaining, kBlock);
    bool mono = ch.format == kFmtMono;
    u32 stride = mono ? 2 : 4;
    if (n && memValid(ch.addr) && memValid(ch.addr + n * stride - 1)) {
        const s16* p = (const s16*)memPtr(ch.addr);
        s32 lv = (s32)ch.leftVol, rv = (s32)ch.rightVol;
        int peak = g_stats.chPeak[idx];
        for (u32 i = 0; i < n; i++) {
            s32 l = mono ? p[i] : p[i * 2];
            s32 r = mono ? p[i] : p[i * 2 + 1];
            peak = std::max(peak, std::max(std::abs(l), std::abs(r)));
            g_mix[i * 2] += (l * lv) >> 15;  // |s16 * 0xFFFF| < 2^31
            g_mix[i * 2 + 1] += (r * rv) >> 15;
        }
        g_stats.chPeak[idx] = peak;
    }
    g_stats.chFrames[idx] += n;
    ch.addr += n * stride;
    ch.remaining -= n;
    if (ch.remaining == 0) channelFinished(idx);
}

// ------------------------------------------------------------------ Output2 / SRC channel
void srcDescriptorDone() {
    while (!g_src.waiters.empty()) {
        SrcChannel::Waiter w = g_src.waiters.front();
        g_src.waiters.pop_front();
        if (threadParkedOn(w.thread, kSrcWaitId)) {
            wakeThread(getThread(w.thread), w.result);
            return;
        }
    }
    g_src.completion = true;  // nobody listening: the next caller consumes it
}

void srcFrame(u32 addr, bool mono, s32& l, s32& r) {
    u32 stride = mono ? 2 : 4;
    if (!addr || !memValid(addr) || !memValid(addr + stride - 1)) {
        l = r = 0;
        return;
    }
    const s16* p = (const s16*)memPtr(addr);
    l = p[0];
    r = mono ? p[0] : p[1];
}

void mixSrcBlock() {
    SrcChannel& s = g_src;
    if (!s.count) return;
    u32 inRate = s.freq ? s.freq : (u32)kOutRate;
    u32 step = (u32)(((u64)inRate << 16) / (u32)kOutRate);
    bool mono = s.format == kFmtMono;
    u32 stride = mono ? 2 : 4;
    s64 vol = s.vol;
    for (u32 i = 0; i < kBlock && s.count; i++) {
        s32 l0, r0;
        srcFrame(s.bufs[0].addr + s.played * stride, mono, l0, r0);
        s32 l = l0, r = r0;
        if (step != 0x10000 && s.frac) {
            // interpolate towards the next frame, which may be the first of the next descriptor
            u32 nextAddr = s.played + 1 < s.bufs[0].samples ? s.bufs[0].addr + (s.played + 1) * stride
                                                            : (s.count > 1 ? s.bufs[1].addr : 0);
            s32 l1 = l0, r1 = r0;
            if (nextAddr) srcFrame(nextAddr, mono, l1, r1);
            l = l0 + (s32)(((s64)(l1 - l0) * s.frac) >> 16);
            r = r0 + (s32)(((s64)(r1 - r0) * s.frac) >> 16);
        }
        g_mix[i * 2] += (s32)((l * vol) >> 15);
        g_mix[i * 2 + 1] += (s32)((r * vol) >> 15);
        g_stats.chPeak[kNumChannels] = std::max(g_stats.chPeak[kNumChannels], std::max(std::abs(l), std::abs(r)));
        g_stats.chFrames[kNumChannels]++;
        s.frac += step;
        u32 adv = s.frac >> 16;
        s.frac &= 0xFFFF;
        while (adv && s.count) {
            u32 take = std::min(adv, s.bufs[0].samples - s.played);
            s.played += take;
            adv -= take;
            if (s.played >= s.bufs[0].samples) {
                s.bufs[0] = s.bufs[1];
                s.bufs[1] = {};
                s.count--;
                s.played = 0;
                srcDescriptorDone();
            }
        }
    }
    if (!s.count && s.reserved && !s.starveSince) s.starveSince = g_tickNow;
}

s32 srcOutputBlocking(u32 vol, u32 addr) {
    SrcChannel& s = g_src;
    if (!s.reserved) return (s32)ERR_AUDIO_NOT_RESERVED;
    if (s.count >= 2) return (s32)ERR_AUDIO_BUSY;  // both descriptors armed: refused, no wait
    s32 result = 0;
    if (addr) {
        s.vol = vol;
        bool wasIdle = s.count == 0;
        if (wasIdle) {
            // starting the DMA signals a completion, so the first output returns immediately
            s.completion = true;
            s.played = 0;
            s.frac = 0;
        }
        s.bufs[s.count].addr = addr;
        s.bufs[s.count].samples = s.sampleCount;
        s.count++;
        s.starveSince = 0;
        result = (s32)s.sampleCount;
    } else if (s.count == 0) {
        return 0;
    }
    Thread* t = currentThread();
    if (!t) return (s32)ERR_KERNEL_ILLEGAL_CONTEXT;
    if (s.completion) {
        s.completion = false;
        return result;
    }
    s.waiters.push_back({t->uid, result});
    return waitCurrent(WaitType::Audio, kSrcWaitId, 0, false);
}

void srcReset() {
    for (const SrcChannel::Waiter& w : g_src.waiters)
        if (threadParkedOn(w.thread, kSrcWaitId)) wakeThread(getThread(w.thread), (s32)ERR_AUDIO_NOT_RESERVED);
    g_src = SrcChannel{};
    g_src.completion = true;  // releasing signals a completion that the next reserve's first call consumes
}

bool srcSampleCountOk(u32 n) { return n >= 17 && n <= 4111; }
bool srcFreqOk(u32 f) {
    switch (f) {
    case 0: case 8000: case 11025: case 12000: case 16000: case 22050: case 24000: case 32000: case 44100: case 48000:
        return true;
    default:
        return false;
    }
}

// ------------------------------------------------------------------ host side
void mixBlock(s16* out, int masterVol) {
    memset(g_mix, 0, sizeof(g_mix));
    // Deliberately not gated on `reserved`: a buffer in flight keeps playing after a release.
    for (int i = 0; i < kNumChannels; i++) mixChannelBlock(i);
    mixSrcBlock();
    for (u32 k = 0; k < kBlock * 2; k++) {
        s32 v = masterVol == 100 ? g_mix[k] : (s32)(((s64)g_mix[k] * masterVol) / 100);
        out[k] = (s16)std::clamp(v, -32768, 32767);
    }
    g_stats.blocks++;
}

// True if a channel ran dry recently enough that its owner may still refill it.
bool anyStarving(u64 now) {
    bool any = false;
    auto check = [&](u64& since) {
        if (!since) return;
        if (now > since + kStarveWaitUs) {
            since = 0;  // the stream stopped; it plays silence like any idle channel
            g_stats.ended++;
        } else {
            any = true;
        }
    };
    check(g_src.starveSince);
    for (Channel& ch : g_ch) check(ch.starveSince);
    return any;
}

void giveUpStarving() {
    bool any = g_src.starveSince != 0;
    g_src.starveSince = 0;
    for (Channel& ch : g_ch) {
        any |= ch.starveSince != 0;
        ch.starveSince = 0;
    }
    if (any) g_stats.gaps++;
}

void tick(u64 now) {
    g_tickNow = now ? now : 1;
    u64 elapsed = g_lastTick ? now - g_lastTick : 0;
    g_lastTick = now;
    int latMs = std::clamp(config().audioLatencyMs, 20, 500);
    size_t target = (size_t)latMs * kOutRate / 1000;
    size_t lowWater = std::max<size_t>(target / 4, 512);
    bool dev = host::audioDeviceOpen();
    size_t queued;
    if (dev) {
        queued = host::audioQueuedFrames();
    } else {
        g_virtQueued = std::max(0.0, g_virtQueued - elapsed * (kOutRate / 1e6));
        queued = (size_t)g_virtQueued;
    }
    if (g_everPushed && queued < kBlock) g_stats.underruns++;  // device (nearly) ran dry
    g_stats.minQueued = std::min(g_stats.minQueued, queued);
    g_stats.maxQueued = std::max(g_stats.maxQueued, queued);

    size_t want = queued < target ? (target - queued) / kBlock * kBlock : 0;
    g_out.resize(std::max<size_t>(g_out.size(), want * 2));
    int masterVol = std::clamp(config().masterVolume, 0, 100);
    size_t produced = 0;
    while (produced < want) {
        if (anyStarving(now)) {
            if (queued + produced >= lowWater) {
                g_stats.stalls++;
                break;  // let the guest refill; the host queue covers the pause
            }
            giveUpStarving();
        }
        mixBlock(&g_out[produced * 2], masterVol);
        produced += kBlock;
    }
    if (produced) {
        if (dev) host::audioPush(g_out.data(), produced);
        else g_virtQueued += (double)produced;
        g_everPushed = true;
        if (g_dump.isOpen()) g_dump.write(g_out.data(), produced);
    }

    if (!g_stats.lastReport) g_stats.lastReport = now;
    if (now - g_stats.lastReport >= 10000000ull) {
        LOG_INFO("AUDIO",
                 "mixer: %llu blocks, %llu host underruns, %llu refill stalls, %llu late refills, %llu stream ends, "
                 "queue %zu..%zu frames (target %zu)",
                 (unsigned long long)g_stats.blocks, (unsigned long long)g_stats.underruns,
                 (unsigned long long)g_stats.stalls, (unsigned long long)g_stats.gaps, (unsigned long long)g_stats.ended,
                 g_stats.minQueued == ~(size_t)0 ? 0 : g_stats.minQueued, g_stats.maxQueued, target);
        char buf[512];
        int len = 0;
        for (int i = 0; i <= kNumChannels; i++)
            if (g_stats.chFrames[i] && len < (int)sizeof(buf) - 64)
                len += snprintf(buf + len, sizeof(buf) - len, " %s%d: %llu frames peak %d;", i == kNumChannels ? "src" : "ch",
                                i == kNumChannels ? 0 : i, (unsigned long long)g_stats.chFrames[i], g_stats.chPeak[i]);
        if (len) LOG_INFO("AUDIO", "channels (last 10 s):%s", buf);
        memset(g_stats.chFrames, 0, sizeof(g_stats.chFrames));
        memset(g_stats.chPeak, 0, sizeof(g_stats.chPeak));
        g_stats.lastReport = now;
        g_stats.minQueued = ~(size_t)0;
        g_stats.maxQueued = 0;
    }
}

// ------------------------------------------------------------------ HLE: mixer channels
bool badChannel(u32 ch) { return ch >= (u32)kNumChannels; }

int findFree() {
    for (int i = kNumChannels - 1; i >= 0; i--)
        if (g_ch[i].sampleCount == 0 && g_ch[i].addr == 0 && !g_ch[i].reserved) return i;
    return -1;
}

void sceAudioChReserve(PspContext* c) {
    s32 idx = (s32)A0;
    u32 count = A1, fmt = A2;
    if (idx < 0) {
        idx = findFree();
        if (idx < 0) return (void)RET(ERR_AUDIO_NO_CHANNELS);
    }
    if (badChannel((u32)idx) || g_ch[idx].reserved) return (void)RET(ERR_AUDIO_INVALID_CHANNEL);
    if ((count & 63) != 0 || count == 0 || count > kSampleMax) return (void)RET(ERR_AUDIO_SIZE_NOT_ALIGNED);
    if (fmt != kFmtMono && fmt != kFmtStereo) return (void)RET(ERR_AUDIO_INVALID_FORMAT);
    Channel& ch = g_ch[idx];
    ch.reserved = true;
    ch.sampleCount = count;
    ch.format = fmt;
    ch.leftVol = ch.rightVol = 0;
    ch.starveSince = 0;
    LOG_DEBUG("AUDIO", "channel %d reserved: %u samples, %s", idx, count, fmt == kFmtMono ? "mono" : "stereo");
    RET(idx);
}

void sceAudioChRelease(PspContext* c) {
    if (badChannel(A0)) return (void)RET(ERR_AUDIO_INVALID_CHANNEL);
    Channel& ch = g_ch[A0];
    if (!ch.reserved) return (void)RET(ERR_AUDIO_NOT_RESERVED);
    if (ch.waitThread && threadParkedOn(ch.waitThread, A0)) return (void)RET(ERR_AUDIO_BUSY);
    ch.reserved = false;
    ch.sampleCount = 0;
    ch.starveSince = 0;
    ch.waitThread = 0;
    RET(0);
}

void sceAudioOutput(PspContext* c) {
    if ((s32)A1 > 0xFFFF) return (void)RET(ERR_AUDIO_INVALID_VOLUME);
    if (badChannel(A0)) return (void)RET(ERR_AUDIO_INVALID_CHANNEL);
    RET(output((int)A0, (s32)A1, (s32)A1, A2, false));
}
void sceAudioOutputBlocking(PspContext* c) {
    if ((s32)A1 > 0xFFFF) return (void)RET(ERR_AUDIO_INVALID_VOLUME);
    if (badChannel(A0)) return (void)RET(ERR_AUDIO_INVALID_CHANNEL);
    RET(output((int)A0, (s32)A1, (s32)A1, A2, true));
}
void sceAudioOutputPanned(PspContext* c) {
    if ((s32)A1 > 0xFFFF || (s32)A2 > 0xFFFF) return (void)RET(ERR_AUDIO_INVALID_VOLUME);
    if (badChannel(A0)) return (void)RET(ERR_AUDIO_INVALID_CHANNEL);
    RET(output((int)A0, (s32)A1, (s32)A2, A3, false));
}
void sceAudioOutputPannedBlocking(PspContext* c) {
    if ((A1 | A2) > 0xFFFF) return (void)RET(ERR_AUDIO_INVALID_VOLUME);
    if (badChannel(A0)) return (void)RET(ERR_AUDIO_INVALID_CHANNEL);
    RET(output((int)A0, (s32)A1, (s32)A2, A3, true));
}

void sceAudioChangeChannelConfig(PspContext* c) {
    if (badChannel(A0)) return (void)RET(ERR_AUDIO_INVALID_CHANNEL);
    Channel& ch = g_ch[A0];
    if ((ch.waitThread && threadParkedOn(ch.waitThread, A0)) || ch.addr) return (void)RET(ERR_AUDIO_BUSY);
    if (!ch.reserved) return (void)RET(ERR_AUDIO_NOT_RESERVED);
    if (A1 != kFmtMono && A1 != kFmtStereo) return (void)RET(ERR_AUDIO_INVALID_FORMAT);
    ch.format = A1;
    RET(0);
}

void sceAudioChangeChannelVolume(PspContext* c) {
    s32 lv = (s32)A1, rv = (s32)A2;
    if (lv > 0xFFFF || rv > 0xFFFF) return (void)RET(ERR_AUDIO_INVALID_VOLUME);
    if (badChannel(A0)) return (void)RET(ERR_AUDIO_INVALID_CHANNEL);
    if (lv >= 0) g_ch[A0].leftVol = (u32)lv;
    if (rv >= 0) g_ch[A0].rightVol = (u32)rv;
    RET(0);
}

void sceAudioSetChannelDataLen(PspContext* c) {
    u32 len = A1;
    if (badChannel(A0)) return (void)RET(ERR_AUDIO_INVALID_CHANNEL);
    if ((len & 63) != 0 || len == 0 || len > kSampleMax) return (void)RET(ERR_AUDIO_SIZE_NOT_ALIGNED);
    Channel& ch = g_ch[A0];
    if (ch.waitThread && threadParkedOn(ch.waitThread, A0)) return (void)RET(ERR_AUDIO_BUSY);
    if (!ch.reserved) return (void)RET(ERR_AUDIO_NOT_INIT);
    ch.sampleCount = len;
    RET(0);
}

// Remaining samples of the playing buffer plus a full buffer if a thread is parked behind it.
s32 restLength(u32 chn, bool needAddr) {
    const Channel& ch = g_ch[chn];
    s32 rest = (!needAddr || ch.addr) ? (s32)ch.remaining : 0;
    if (ch.waitThread && threadParkedOn(ch.waitThread, chn)) rest += (s32)ch.sampleCount;
    return rest;
}
void sceAudioGetChannelRestLength(PspContext* c) {
    if (badChannel(A0)) return (void)RET(ERR_AUDIO_INVALID_CHANNEL);
    RET(restLength(A0, true));
}
void sceAudioGetChannelRestLen(PspContext* c) {
    if (badChannel(A0)) return (void)RET(ERR_AUDIO_INVALID_CHANNEL);
    RET(restLength(A0, false));
}

void sceAudioInit(PspContext* c) { RET(0); }
void sceAudioEnd(PspContext* c) { RET(0); }

// ------------------------------------------------------------------ HLE: Output2 / SRC
void sceAudioOutput2Reserve(PspContext* c) {
    u32 count = A0 & 0x7FFFFFFF;
    if (!srcSampleCountOk(count)) return (void)RET(ERR_KERNEL_INVALID_SIZE);
    if (g_src.reserved) return (void)RET(ERR_AUDIO_ALREADY_RESERVED);
    bool completion = g_src.completion;
    g_src = SrcChannel{};
    g_src.completion = completion;
    g_src.reserved = true;
    g_src.sampleCount = count;
    g_src.format = kFmtStereo;
    g_src.freq = 0;
    RET(0);
}
void sceAudioOutput2Release(PspContext* c) {
    if (!g_src.reserved) return (void)RET(ERR_AUDIO_NOT_RESERVED);
    if (g_src.count) return (void)RET(ERR_AUDIO_ALREADY_RESERVED);
    srcReset();
    RET(0);
}
void sceAudioOutput2OutputBlocking(PspContext* c) {
    if (A0 > 0xFFFFF) return (void)RET(ERR_AUDIO_INVALID_VOLUME);
    RET(srcOutputBlocking(A0, A1));
}
void sceAudioOutput2ChangeLength(PspContext* c) {
    if (A0 - 17 >= 0xFFF) return (void)RET(ERR_AUDIO_SIZE_NOT_ALIGNED);
    if (!g_src.reserved) return (void)RET(ERR_AUDIO_NOT_RESERVED);
    g_src.sampleCount = A0;  // buffers already handed over keep their length
    RET(0);
}
void sceAudioOutput2GetRestSample(PspContext* c) {
    if (!g_src.reserved) return (void)RET(ERR_AUDIO_NOT_RESERVED);
    RET((u32)g_src.count * g_src.sampleCount);
}
void sceAudioSRCChReserve(PspContext* c) {
    u32 count = A0 & 0x7FFFFFFF, freq = A1, fmt = A2;
    if (fmt == 4) return (void)RET(ERR_SRC_FORMAT_4);
    if (fmt != 2) return (void)RET(ERR_KERNEL_INVALID_SIZE);
    if (!srcSampleCountOk(count)) return (void)RET(ERR_KERNEL_INVALID_SIZE);
    if (!srcFreqOk(freq)) return (void)RET(ERR_AUDIO_INVALID_FREQUENCY);
    if (g_src.reserved) return (void)RET(ERR_AUDIO_ALREADY_RESERVED);
    bool completion = g_src.completion;
    g_src = SrcChannel{};
    g_src.completion = completion;
    g_src.reserved = true;
    g_src.sampleCount = count;
    g_src.format = kFmtStereo;
    g_src.freq = freq;
    LOG_INFO("AUDIO", "SRC channel reserved: %u samples at %u Hz", count, freq ? freq : kOutRate);
    RET(0);
}
void sceAudioSRCChRelease(PspContext* c) { sceAudioOutput2Release(c); }
void sceAudioSRCOutputBlocking(PspContext* c) { sceAudioOutput2OutputBlocking(c); }

const HleFunction kAudio[] = {
    {0x80F1F7E0, "sceAudioInit", sceAudioInit},
    {0x210567F7, "sceAudioEnd", sceAudioEnd},
    {0x5EC81C55, "sceAudioChReserve", sceAudioChReserve},
    {0x6FC46853, "sceAudioChRelease", sceAudioChRelease},
    {0x8C1009B2, "sceAudioOutput", sceAudioOutput},
    {0x136CAF51, "sceAudioOutputBlocking", sceAudioOutputBlocking},
    {0xE2D56B2D, "sceAudioOutputPanned", sceAudioOutputPanned},
    {0x13F592BC, "sceAudioOutputPannedBlocking", sceAudioOutputPannedBlocking},
    {0x95FD0C2D, "sceAudioChangeChannelConfig", sceAudioChangeChannelConfig},
    {0xB7E1D8E7, "sceAudioChangeChannelVolume", sceAudioChangeChannelVolume},
    {0xCB2E439E, "sceAudioSetChannelDataLen", sceAudioSetChannelDataLen},
    {0xB011922F, "sceAudioGetChannelRestLength", sceAudioGetChannelRestLength},
    {0xE9D97901, "sceAudioGetChannelRestLen", sceAudioGetChannelRestLen},
    {0x01562BA3, "sceAudioOutput2Reserve", sceAudioOutput2Reserve},
    {0x43196845, "sceAudioOutput2Release", sceAudioOutput2Release},
    {0x2D53F36E, "sceAudioOutput2OutputBlocking", sceAudioOutput2OutputBlocking},
    {0x63F2889C, "sceAudioOutput2ChangeLength", sceAudioOutput2ChangeLength},
    {0x647CEF33, "sceAudioOutput2GetRestSample", sceAudioOutput2GetRestSample},
    {0x38553111, "sceAudioSRCChReserve", sceAudioSRCChReserve},
    {0x5C37C0AE, "sceAudioSRCChRelease", sceAudioSRCChRelease},
    {0xE0727056, "sceAudioSRCOutputBlocking", sceAudioSRCOutputBlocking},
};
}  // namespace

void audioInit() {
    host::audioInit();
    openDumpFromEnv(g_dump, "VC2_AUDIO_DUMP", kOutRate, 2);
    addPeriodicTimer(kTickMicros, [](u64 now) { tick(now); });
}

HLE_MODULE("sceAudio", kAudio);

}  // namespace vc2rt
