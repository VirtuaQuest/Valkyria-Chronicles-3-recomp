// sceSasCore: the PSP's software synthesizer (on hardware it runs on the Media Engine).
//
// The game owns a SAS "core" structure in guest memory but every voice parameter lives inside
// the library, so we keep one native instance (no known game uses two). Each __sceSasCore call
// renders one grain (64..2048 frames, VC2 uses 1024) of 44.1 kHz audio into guest memory; the
// game then plays it through sceAudio itself.
//
// Per voice, per output sample (fixed point as on the ME):
//   source  : VAG ADPCM (28 samples / 16-byte block, loop-start / loop-end / end flags), 16-bit
//             PCM with optional loop point, PSX-style LFSR noise, or ATRAC3 (logged, silent)
//   pitch   : 0x1000 = 1.0 (0..0x4000), two-tap linear interpolation with a 12-bit fraction
//   envelope: ADSR, 6 curve types, height 0..0x40000000, stepped once per output sample; the
//             first ~32 samples after key-on are silent while the envelope "kicks in"
//   mix     : sample * env, then * volume (0x1000 = unity, signed) into the dry bus, and
//             * effect send into the send bus that feeds the reverb
// Output: mixed (interleaved stereo dry + wet, optionally added to an existing buffer by
// __sceSasCoreWithMix) or raw (four planes: dry L, dry R, send L, send R).
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "audio.h"
#include "hle.h"
#include "sas_reverb.h"

namespace vc2rt::sas {

namespace {

enum : u32 {
    ERR_INVALID_GRAIN = 0x80420001,
    ERR_INVALID_MAX_VOICES = 0x80420002,
    ERR_INVALID_OUTPUT_MODE = 0x80420003,
    ERR_INVALID_SAMPLE_RATE = 0x80420004,
    ERR_BAD_ADDRESS = 0x80420005,
    ERR_INVALID_VOICE = 0x80420010,
    ERR_INVALID_NOISE_FREQ = 0x80420011,
    ERR_INVALID_PITCH = 0x80420012,
    ERR_INVALID_ADSR_CURVE_MODE = 0x80420013,
    ERR_INVALID_PARAMETER = 0x80420014,
    ERR_INVALID_LOOP_POS = 0x80420015,
    ERR_VOICE_PAUSED = 0x80420016,
    ERR_INVALID_VOLUME = 0x80420018,
    ERR_INVALID_ADSR_RATE = 0x80420019,
    ERR_INVALID_PCM_SIZE = 0x8042001A,
    ERR_REV_INVALID_TYPE = 0x80420020,
    ERR_REV_INVALID_FEEDBACK = 0x80420021,
    ERR_REV_INVALID_DELAY = 0x80420022,
    ERR_REV_INVALID_VOLUME = 0x80420023,
    ERR_ATRAC3_ALREADY_SET = 0x80420040,
    ERR_ATRAC3_NOT_SET = 0x80420041,
    ERR_NOT_INIT = 0x80420100,
    ERR_KERNEL_BAD_ARGUMENT = 0x80000004,
};

constexpr int kVoices = 32;
constexpr int kMaxGrain = 2048;
constexpr int kVolMax = 0x1000;
constexpr int kPitchBase = 0x1000;
constexpr int kPitchMax = 0x4000;
constexpr s64 kEnvMax = 0x40000000;
constexpr int kOutputMixed = 0, kOutputRaw = 1;

enum Curve : int { LinInc = 0, LinDec = 1, LinBent = 2, ExpDec = 3, ExpInc = 4, Direct = 5 };

inline s32 clamp16(s32 v) { return std::clamp(v, -32768, 32767); }

// ------------------------------------------------------------------ envelope
struct Envelope {
    enum Phase { Off, KeyOn, Attack, Decay, Sustain, Release };
    // index 0..3 = attack, decay, sustain, release
    s32 rate[4] = {0, 0, 0, 0};
    int curve[4] = {LinInc, LinDec, LinDec, LinDec};
    s32 sustainLevel = 0;
    Phase phase = Off;
    s64 height = 0;
    int keyOnSteps = 0;

    s32 value() const { return (s32)std::min<s64>(height, kEnvMax); }

    void enter(Phase p) {
        if (height > kEnvMax) height = kEnvMax;
        phase = p;
    }
    void keyOn() {
        phase = KeyOn;
        keyOnSteps = 0;
        if (height > kEnvMax) height = kEnvMax;
    }
    void keyOff() { enter(Release); }
    void end() {
        phase = Off;
        height = 0;
    }
    bool freshKeyOn() const { return phase == KeyOn && keyOnSteps == 0; }

    void walk(int c, s32 r) {
        switch (c) {
        case LinInc: height += r; break;
        case LinDec: height -= r; break;
        case LinBent: height += height <= kEnvMax * 3 / 4 ? r : r / 4; break;
        case ExpDec:
            // h *= (1 - r/2^32), with the ME's rounding (always makes progress towards 0)
            height = height + (((kEnvMax - height) * (s64)r) >> 32) - (s64)(((u32)r + 3u) >> 2);
            break;
        case ExpInc:
            // approaches the maximum like a charging capacitor, plus a constant so it arrives
            height = height + (((kEnvMax - height) * (s64)r) >> 32) + 0x4000;
            break;
        case Direct: height = r; break;
        }
    }

    void step() {
        switch (phase) {
        case Off: break;
        case KeyOn:
            // ~32 samples at (almost) zero before the attack starts
            if (++keyOnSteps >= 32) {
                height = 0;
                enter(Attack);
            } else {
                height = keyOnSteps - 1;
            }
            break;
        case Attack:
            walk(curve[0], rate[0]);
            if (height >= kEnvMax || height < 0) enter(Decay);
            break;
        case Decay:
            walk(curve[1], rate[1]);
            if (height < sustainLevel) enter(Sustain);
            break;
        case Sustain:
            walk(curve[2], rate[2]);
            if (height <= 0) {
                height = 0;
                enter(Release);
            }
            break;
        case Release:
            walk(curve[3], rate[3]);
            if (height <= 0) {
                height = 0;
                phase = Off;
            }
            break;
        }
    }
};

// SPU-style ADSR register encoding used by __sceSasSetSimpleADSR.
s32 simpleRate(int n, int base) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    s32 r = (s32)(((u32)(7 - (n & 3)) << base) >> (n >> 2));
    return r ? r : 1;
}
s32 shiftRate(int n) { return n == 0 ? 0x7FFFFFFF : (s32)(0x80000000u >> n); }

void setSimpleEnvelope(Envelope& e, u32 env1, u32 env2) {
    e.rate[0] = simpleRate((int)(env1 >> 8), 26);
    e.curve[0] = (env1 & 0x8000) ? LinBent : LinInc;
    e.rate[1] = shiftRate((int)((env1 >> 4) & 0xF));
    e.curve[1] = ExpDec;
    e.sustainLevel = (s32)(((env1 & 0xF) + 1) << 26);
    e.curve[2] = (int)((env2 >> 14) & 3);  // 0 lin+, 1 lin-, 2 bent (exp+), 3 exp-
    e.rate[2] = simpleRate((int)(env2 >> 6), e.curve[2] == ExpDec ? 24 : 26);
    bool relExp = (env2 & 0x20) != 0;
    e.curve[3] = relExp ? ExpDec : LinDec;
    int rn = (int)(env2 & 0x1F);
    if (rn == 31) e.rate[3] = 0;
    else if (relExp) e.rate[3] = shiftRate(rn);
    else if (rn == 30) e.rate[3] = 0x40000000;
    else if (rn == 29) e.rate[3] = 1;
    else e.rate[3] = (s32)(0x10000000u >> rn);
}

// ------------------------------------------------------------------ VAG ADPCM
// Prediction filters (c1, -c2): the five SPU filters, then what the decoder reads past the end of
// its table for the invalid indices 5..15 (hardware-observed values).
inline void vagCoef(int f, int& c1, int& c2) {
    static const u8 raw[16][2] = {{0, 0},  {60, 0}, {115, 52}, {98, 55}, {122, 60}, {0, 0}, {0, 0},  {52, 0},
                                  {55, 2}, {60, 125}, {0, 0}, {0, 91},  {0, 0},    {2, 216}, {125, 6}, {0, 151}};
    c1 = raw[f][0];
    c2 = -(int)raw[f][1];
}

struct VagStream {
    u32 base = 0;
    int blocks = 0;
    int next = 0;  // index of the next block to decode
    int loopBlock = 0;
    bool loopEnabled = false, jumpPending = false, ended = true;
    s16 buf[28]{};
    int pos = 28;
    s32 h1 = 0, h2 = 0;

    void start(u32 addr, int size, bool loop) {
        base = addr;
        blocks = size > 0 ? size / 16 : 0;
        next = 0;
        loopBlock = 0;
        loopEnabled = loop;
        jumpPending = false;
        ended = blocks == 0 || !memValid(addr) || !memValid(addr + (u32)blocks * 16 - 1);
        pos = 28;
        h1 = h2 = 0;
    }

    bool decode() {
        if (jumpPending) {
            next = loopBlock;
            jumpPending = false;
        }
        if (next >= blocks) return false;
        const u8* p = memPtr(base + (u32)next * 16);
        int shift = p[0] & 0xF, filter = p[0] >> 4, flags = p[1];
        if (flags == 7) return false;           // end marker block: not played
        if (flags == 6) loopBlock = next;       // loop start
        if (flags == 3 && loopEnabled) jumpPending = true;  // loop end: repeat after this block
        int c1, c2;
        vagCoef(filter, c1, c2);
        for (int i = 0; i < 14; i++) {
            u8 d = p[2 + i];
            for (int k = 0; k < 2; k++) {
                int nib = k ? (d >> 4) : (d & 0xF);
                s32 s = ((s32)(s16)(u16)(nib << 12)) >> shift;
                s32 v = clamp16(s + ((h1 * c1 + h2 * c2) >> 6));
                h2 = h1;
                h1 = v;
                buf[i * 2 + k] = (s16)v;
            }
        }
        next++;
        pos = 0;
        return true;
    }

    s16 sample() {
        if (ended) return 0;
        if (pos >= 28 && !decode()) {
            ended = true;
            return 0;
        }
        return buf[pos++];
    }
};

// ------------------------------------------------------------------ voices
enum class Source { None, Vag, Pcm, Noise, Atrac3 };

struct Voice {
    Source type = Source::None;
    bool playing = false, paused = false, on = false;
    // VAG
    u32 vagAddr = 0;
    int vagSize = 0;
    bool loop = false;
    VagStream vag;
    // PCM
    u32 pcmAddr = 0;
    int pcmSize = 0, pcmLoopPos = 0, pcmPos = 0;
    bool pcmEnded = false;
    // noise (SPU LFSR)
    int noiseFreq = 0;
    s32 noiseTimer = 0;
    u16 noiseLevel = 1;
    // pitch / interpolation window
    int pitch = kPitchBase;
    u32 frac = 0;
    s32 cur = 0, nxt = 0;
    // levels
    int volL = kVolMax, volR = kVolMax, fxL = kVolMax, fxR = kVolMax;
    Envelope env;
    u32 atracCtx = 0;

    s32 fetch() {
        switch (type) {
        case Source::Vag: return vag.sample();
        case Source::Pcm: {
            if (pcmEnded) return 0;
            if (pcmPos >= pcmSize) {
                if (!loop) {
                    pcmEnded = true;
                    return 0;
                }
                pcmPos = pcmLoopPos;
            }
            u32 a = pcmAddr + (u32)pcmPos * 2;
            pcmPos++;
            return memValid(a) ? (s16)rd16(a) : 0;
        }
        default: return 0;
        }
    }

    s32 noise() {
        // PlayStation SPU noise generator: 15-bit LFSR clocked at a rate set by shift/step.
        int stepv = (noiseFreq & 3) + 4, shift = noiseFreq >> 2;
        noiseTimer -= stepv;
        if (noiseTimer < 0) {
            u16 l = noiseLevel;
            u16 parity = (u16)(((l >> 15) ^ (l >> 12) ^ (l >> 11) ^ (l >> 10) ^ 1) & 1);
            noiseLevel = (u16)((l << 1) | parity);
            noiseTimer += 0x20000 >> shift;
            if (noiseTimer < 0) noiseTimer += 0x20000 >> shift;
        }
        return (s16)noiseLevel;
    }

    bool sourceEnded() const {
        switch (type) {
        case Source::Vag: return vag.ended;
        case Source::Pcm: return pcmEnded;
        default: return false;
        }
    }

    void keyOn() {
        env.keyOn();
        if (type == Source::Vag) {
            if (!memValid(vagAddr)) {
                LOG_WARN("SAS", "key on with invalid VAG address %08x", vagAddr);
                return;
            }
            vag.start(vagAddr, vagSize, loop);
        } else if (type == Source::Pcm) {
            pcmPos = 0;
            pcmEnded = false;
        }
        playing = true;
        on = true;
        paused = false;
        frac = 0;
        cur = nxt = 0;
    }
};

struct Instance {
    int grain = 0;
    int outputMode = kOutputMixed;
    Voice voices[kVoices];
    Reverb reverb;
    int revType = -1, revDelay = 0, revFeedback = 0;
    int evolL = 0, evolR = 0;
    bool dryOn = true, wetOn = false;
    s32 dry[kMaxGrain * 2]{};
    s32 send[kMaxGrain * 2]{};
    s32 wet[kMaxGrain * 2]{};
};
Instance* g_sas = nullptr;
WavDump g_sasDump;
bool g_dumpTried = false;
u64 g_grains = 0;

Instance& sas() {
    if (!g_sas) g_sas = new Instance();
    return *g_sas;
}

void mixVoice(Instance& s, Voice& v) {
    if (v.type == Source::Vag && !v.vagAddr) return;
    if (v.type == Source::Pcm && !v.pcmAddr) return;
    int start = 0;
    if (v.env.freshKeyOn()) {
        // Output starts ~32 source samples after key-on (one more for VAG); the envelope runs
        // its key-on steps meanwhile and the source is not consumed.
        int delay = (v.type == Source::Pcm && v.pitch > kPitchBase) ? 32 : (32 * v.pitch) >> 12;
        if (v.type == Source::Vag) delay++;
        delay = std::min(delay, s.grain);
        for (int i = 0; i < delay; i++) v.env.step();
        start = delay;
        v.cur = v.fetch();
        v.nxt = v.fetch();
        v.frac = 0;
    }
    const bool isNoise = v.type == Source::Noise;
    const int pitch = v.pitch;
    s32* dry = s.dry;
    s32* send = s.send;
    for (int i = start; i < s.grain; i++) {
        s32 x;
        if (isNoise) {
            x = v.noise();
        } else {
            x = v.cur - (((v.cur - v.nxt) * (s32)v.frac) >> 12);
            v.frac += (u32)pitch;
            while (v.frac >= (u32)kPitchBase) {
                v.frac -= kPitchBase;
                v.cur = v.nxt;
                v.nxt = v.fetch();
            }
        }
        s32 e = (v.env.value() + (1 << 14)) >> 15;  // 30-bit height -> 15 bits, rounded
        v.env.step();
        x = (x * e + (1 << 14)) >> 15;
        dry[i * 2] += (x * v.volL) >> 12;
        dry[i * 2 + 1] += (x * v.volR) >> 12;
        send[i * 2] += (x * v.fxL) >> 12;
        send[i * 2 + 1] += (x * v.fxR) >> 12;
    }
    if (v.sourceEnded()) v.env.end();
    if (v.env.phase == Envelope::Off) {
        v.playing = false;
        v.on = false;
    }
}

void render(Instance& s, u32 outAddr, u32 inAddr, int inVolL, int inVolR) {
    int g = s.grain;
    memset(s.dry, 0, sizeof(s32) * g * 2);
    memset(s.send, 0, sizeof(s32) * g * 2);
    for (Voice& v : s.voices)
        if (v.playing && !v.paused) mixVoice(s, v);

    if (s.outputMode == kOutputRaw) {
        s16* out = (s16*)memPtr(outAddr);
        for (int i = 0; i < g; i++) {
            out[i] = (s16)clamp16(s.dry[i * 2]);
            out[g + i] = (s16)clamp16(s.dry[i * 2 + 1]);
            out[2 * g + i] = (s16)clamp16(s.send[i * 2]);
            out[3 * g + i] = (s16)clamp16(s.send[i * 2 + 1]);
        }
        return;
    }
    if (s.wetOn) s.reverb.process(s.send, s.wet, g, s.evolL, s.evolR);
    s16* out = (s16*)memPtr(outAddr);
    const s16* in = inAddr ? (const s16*)memPtr(inAddr) : nullptr;
    for (int i = 0; i < g * 2; i += 2) {
        s32 l = 0, r = 0;
        if (in) {  // the existing buffer, scaled by the WithMix volumes (0x1000 = unity)
            l = (s32)std::clamp<s64>(((s64)in[i] * inVolL) >> 12, -0x40000000, 0x40000000);
            r = (s32)std::clamp<s64>(((s64)in[i + 1] * inVolR) >> 12, -0x40000000, 0x40000000);
        }
        if (s.dryOn) {
            l += s.dry[i];
            r += s.dry[i + 1];
        }
        if (s.wetOn) {
            l += s.wet[i];
            r += s.wet[i + 1];
        }
        out[i] = (s16)clamp16(l);
        out[i + 1] = (s16)clamp16(r);
    }
    if (g_sasDump.isOpen()) g_sasDump.write(out, (size_t)g);
}

bool outRangeOk(u32 addr, u32 bytes) { return addr && memValid(addr) && memValid(addr + bytes - 1); }

// ------------------------------------------------------------------ HLE
#define VOICE_ARG(reg)                                     \
    s32 vn = (s32)(reg);                                   \
    if (vn < 0 || vn >= kVoices) return (void)RET(ERR_INVALID_VOICE); \
    Voice& v = sas().voices[vn]

void sceSasInit(PspContext* c) {
    u32 core = A0, grain = A1, maxVoices = A2, mode = A3, rate = A4;
    if (!memValid(core) || (core & 0x3F)) return (void)RET(ERR_BAD_ADDRESS);
    if (maxVoices == 0 || maxVoices > (u32)kVoices) return (void)RET(ERR_INVALID_MAX_VOICES);
    if (grain < 0x40 || grain > 0x800 || (grain & 0x1F)) return (void)RET(ERR_INVALID_GRAIN);
    if (mode != 0 && mode != 1) return (void)RET(ERR_INVALID_OUTPUT_MODE);
    if (rate != 44100) return (void)RET(ERR_INVALID_SAMPLE_RATE);
    delete g_sas;
    g_sas = new Instance();
    g_sas->grain = (int)grain;
    g_sas->outputMode = (int)mode;
    if (!g_dumpTried) {
        g_dumpTried = true;
        openDumpFromEnv(g_sasDump, "VC2_SAS_DUMP", 44100, 2);
    }
    LOG_INFO("SAS", "init core=%08x grain=%u voices=%u mode=%u", core, grain, maxVoices, mode);
    RET(0);
}

void sceSasCore(PspContext* c) {
    Instance& s = sas();
    if (!s.grain) return (void)RET(ERR_NOT_INIT);
    u32 bytes = (u32)s.grain * (s.outputMode == kOutputMixed ? 4 : 8);
    if (!outRangeOk(A1, bytes)) return (void)RET(ERR_INVALID_PARAMETER);
    render(s, A1, 0, 0, 0);
    g_grains++;
    RET(0);
}

void sceSasCoreWithMix(PspContext* c) {
    Instance& s = sas();
    if (!s.grain) return (void)RET(ERR_NOT_INIT);
    if (!outRangeOk(A1, (u32)s.grain * 4)) return (void)RET(ERR_INVALID_PARAMETER);
    if (s.outputMode == kOutputRaw) return (void)RET(ERR_KERNEL_BAD_ARGUMENT);
    render(s, A1, A1, (s32)A2, (s32)A3);
    g_grains++;
    RET(0);
}

void sceSasSetVoice(PspContext* c) {
    VOICE_ARG(A1);
    u32 addr = A2;
    s32 size = (s32)A3, loop = (s32)A4;
    if (size == 0 || ((u32)size & 0xF)) return (void)RET(ERR_INVALID_PARAMETER);
    if (loop != 0 && loop != 1) return (void)RET(ERR_INVALID_LOOP_POS);
    if (!memValid(addr)) {
        LOG_WARN("SAS", "SetVoice %d: ignoring invalid VAG address %08x", vn, addr);
        return (void)RET(0);
    }
    if (v.type == Source::Atrac3) return (void)RET(ERR_ATRAC3_ALREADY_SET);
    if (size < 0) size = 0;  // accepted, but plays nothing
    bool changed = v.type != Source::Vag || v.vagAddr != addr || v.vagSize != size || v.loop != (loop != 0);
    v.type = Source::Vag;
    v.vagAddr = addr;
    v.vagSize = size;
    v.loop = loop != 0;
    if (v.on) v.playing = true;
    if (changed) v.vag.start(addr, size, v.loop);
    RET(0);
}

void sceSasSetVoicePCM(PspContext* c) {
    VOICE_ARG(A1);
    u32 addr = A2;
    s32 size = (s32)A3, loopPos = (s32)A4;
    if (size <= 0 || size > 0x10000) return (void)RET(ERR_INVALID_PCM_SIZE);
    if (loopPos >= size) return (void)RET(ERR_INVALID_LOOP_POS);
    if (!memValid(addr)) {
        LOG_WARN("SAS", "SetVoicePCM %d: ignoring invalid address %08x", vn, addr);
        return (void)RET(0);
    }
    if (v.type == Source::Atrac3) return (void)RET(ERR_ATRAC3_ALREADY_SET);
    v.type = Source::Pcm;
    v.pcmAddr = addr;
    v.pcmSize = size;
    v.pcmPos = 0;
    v.pcmEnded = false;
    v.pcmLoopPos = loopPos >= 0 ? loopPos : 0;
    v.loop = loopPos >= 0;
    v.playing = true;
    RET(0);
}

void sceSasSetVoiceATRAC3(PspContext* c) {
    VOICE_ARG(A1);
    u32 ctx = A2;
    if ((ctx & 3) || !memValid(ctx) || !memValid(ctx + 255)) return (void)RET(ERR_INVALID_PARAMETER);
    if (v.type == Source::Atrac3) return (void)RET(ERR_ATRAC3_ALREADY_SET);
    static int warned = 0;
    if (warned++ < 4) LOG_WARN("SAS", "ATRAC3 voice %d (context %08x) is not supported; it will be silent", vn, ctx);
    v.type = Source::Atrac3;
    v.loop = false;
    v.playing = true;
    v.atracCtx = ctx;
    if (memValid(A0 + 56 * vn + 20)) wr32(A0 + 56 * vn + 20, ctx);
    RET(0);
}

void sceSasConcatenateATRAC3(PspContext* c) {
    VOICE_ARG(A1);
    (void)v;
    RET(0);
}

void sceSasUnsetATRAC3(PspContext* c) {
    VOICE_ARG(A1);
    if (v.type != Source::Atrac3) return (void)RET(ERR_ATRAC3_NOT_SET);
    v.type = Source::None;
    v.playing = v.on = v.paused = false;
    v.atracCtx = 0;
    if (memValid(A0 + 56 * vn + 20)) wr32(A0 + 56 * vn + 20, 0);
    RET(0);
}

void sceSasSetVolume(PspContext* c) {
    VOICE_ARG(A1);
    s32 l = (s32)A2, r = (s32)A3, el = (s32)A4, er = (s32)A5;
    if (std::abs(l) > kVolMax || std::abs(r) > kVolMax || std::abs(el) > kVolMax || std::abs(er) > kVolMax)
        return (void)RET(ERR_INVALID_VOLUME);
    v.volL = l;
    v.volR = r;
    v.fxL = el;
    v.fxR = er;
    RET(0);
}

void sceSasSetPitch(PspContext* c) {
    VOICE_ARG(A1);
    s32 p = (s32)A2;
    if (p < 0 || p > kPitchMax) return (void)RET(ERR_INVALID_PITCH);
    v.pitch = p;
    RET(0);
}

void sceSasSetKeyOn(PspContext* c) {
    VOICE_ARG(A1);
    if (v.paused || v.on) return (void)RET(ERR_VOICE_PAUSED);
    v.keyOn();
    RET(0);
}

void sceSasSetKeyOff(PspContext* c) {
    VOICE_ARG(A1);
    if (v.paused || !v.on) return (void)RET(ERR_VOICE_PAUSED);
    v.on = false;
    v.env.keyOff();
    RET(0);
}

void sceSasSetNoise(PspContext* c) {
    VOICE_ARG(A1);
    s32 f = (s32)A2;
    if (f < 0 || f >= 64) return (void)RET(ERR_INVALID_NOISE_FREQ);
    v.type = Source::Noise;
    v.noiseFreq = f;
    RET(0);
}

void sceSasSetPause(PspContext* c) {
    Instance& s = sas();
    u32 bits = A1;
    bool pause = A2 != 0;
    for (int i = 0; i < kVoices; i++)
        if (bits & (1u << i)) s.voices[i].paused = pause;
    RET(0);
}

void sceSasGetPauseFlag(PspContext* c) {
    u32 f = 0;
    for (int i = 0; i < kVoices; i++)
        if (sas().voices[i].paused) f |= 1u << i;
    RET(f);
}

void sceSasGetEndFlag(PspContext* c) {
    u32 f = 0;
    for (int i = 0; i < kVoices; i++)
        if (!sas().voices[i].playing) f |= 1u << i;
    RET(f);
}

void sceSasSetSL(PspContext* c) {
    VOICE_ARG(A1);
    v.env.sustainLevel = (s32)A2;
    RET(0);
}

void sceSasSetADSR(PspContext* c) {
    VOICE_ARG(A1);
    u32 flag = A2;
    s32 rates[4] = {(s32)A3, (s32)A4, (s32)A5, (s32)A6};
    u32 invalid = 0;
    for (int i = 0; i < 4; i++)
        if (rates[i] < 0) invalid |= 1u << i;
    if (invalid & flag) return (void)RET(ERR_INVALID_ADSR_RATE);
    for (int i = 0; i < 4; i++)
        if (flag & (1u << i)) v.env.rate[i] = rates[i];
    RET(0);
}

void sceSasSetADSRmode(PspContext* c) {
    VOICE_ARG(A1);
    u32 flag = A2;
    // the top bit of each mode is ignored by the PSP
    int m[4] = {(int)(A3 & 0x7FFFFFFF), (int)(A4 & 0x7FFFFFFF), (int)(A5 & 0x7FFFFFFF), (int)(A6 & 0x7FFFFFFF)};
    u32 invalid = 0;
    if (m[0] > 5 || (m[0] & 1) != 0) invalid |= 1;  // attack: increasing curves only
    if (m[1] > 5 || (m[1] & 1) != 1) invalid |= 2;  // decay: decreasing curves / direct
    if (m[2] > 5) invalid |= 4;
    if (m[3] > 5 || (m[3] & 1) != 1) invalid |= 8;  // release: decreasing curves / direct
    if (invalid & flag) return (void)RET(ERR_INVALID_ADSR_CURVE_MODE);
    for (int i = 0; i < 4; i++)
        if (flag & (1u << i)) v.env.curve[i] = m[i];
    RET(0);
}

void sceSasSetSimpleADSR(PspContext* c) {
    VOICE_ARG(A1);
    if ((A3 >> 13) & 1) return (void)RET(ERR_INVALID_ADSR_CURVE_MODE);
    setSimpleEnvelope(v.env, A2 & 0xFFFF, A3 & 0xFFFF);
    RET(0);
}

void sceSasGetEnvelopeHeight(PspContext* c) {
    VOICE_ARG(A1);
    RET(v.env.value());
}

void sceSasGetAllEnvelopeHeights(PspContext* c) {
    u32 dst = A1;
    if (!memValid(dst) || !memValid(dst + kVoices * 4 - 1)) return (void)RET(ERR_INVALID_PARAMETER);
    for (int i = 0; i < kVoices; i++) wr32(dst + i * 4, (u32)sas().voices[i].env.value());
    RET(0);
}

void sceSasRevType(PspContext* c) {
    s32 t = (s32)A1;
    if (t < -1 || t > 8) return (void)RET(ERR_REV_INVALID_TYPE);
    Instance& s = sas();
    if (t != s.revType) LOG_INFO("SAS", "reverb type %d (%s)", t, Reverb::name(t));
    s.revType = t;
    s.reverb.setType(t);
    RET(0);
}

void sceSasRevParam(PspContext* c) {
    s32 d = (s32)A1, f = (s32)A2;
    if (d < 0 || d >= 128) return (void)RET(ERR_REV_INVALID_DELAY);
    if (f < 0 || f >= 128) return (void)RET(ERR_REV_INVALID_FEEDBACK);
    Instance& s = sas();
    s.revDelay = d;
    s.revFeedback = f;
    s.reverb.setParams(d, f);
    RET(0);
}

void sceSasRevEVOL(PspContext* c) {
    if (A1 > 0x1000 || A2 > 0x1000) return (void)RET(ERR_REV_INVALID_VOLUME);
    sas().evolL = (int)A1;
    sas().evolR = (int)A2;
    RET(0);
}

void sceSasRevVON(PspContext* c) {
    sas().dryOn = A1 != 0;
    sas().wetOn = A2 != 0;
    RET(0);
}

void sceSasGetGrain(PspContext* c) { RET(sas().grain); }

void sceSasSetGrain(PspContext* c) {
    s32 g = (s32)A1;
    if (g < 0x40 || g > 0x800 || (g & 0x1F)) return (void)RET(ERR_INVALID_GRAIN);
    sas().grain = g;
    RET(0);
}

void sceSasGetOutputmode(PspContext* c) { RET(sas().outputMode); }

void sceSasSetOutputmode(PspContext* c) {
    if (A1 != 0 && A1 != 1) return (void)RET(ERR_INVALID_OUTPUT_MODE);
    if ((int)A1 != sas().outputMode) LOG_INFO("SAS", "output mode %s", A1 ? "raw (multichannel)" : "mixed");
    sas().outputMode = (int)A1;
    RET(0);
}

const HleFunction kSas[] = {
    {0x42778A9F, "__sceSasInit", sceSasInit},
    {0xA3589D81, "__sceSasCore", sceSasCore},
    {0x50A14DFC, "__sceSasCoreWithMix", sceSasCoreWithMix},
    {0x68A46B95, "__sceSasGetEndFlag", sceSasGetEndFlag},
    {0x440CA7D8, "__sceSasSetVolume", sceSasSetVolume},
    {0xAD84D37F, "__sceSasSetPitch", sceSasSetPitch},
    {0x99944089, "__sceSasSetVoice", sceSasSetVoice},
    {0xE1CD9561, "__sceSasSetVoicePCM", sceSasSetVoicePCM},
    {0xB7660A23, "__sceSasSetNoise", sceSasSetNoise},
    {0x019B25EB, "__sceSasSetADSR", sceSasSetADSR},
    {0x9EC3676A, "__sceSasSetADSRmode", sceSasSetADSRmode},
    {0x5F9529F6, "__sceSasSetSL", sceSasSetSL},
    {0x74AE582A, "__sceSasGetEnvelopeHeight", sceSasGetEnvelopeHeight},
    {0x07F58C24, "__sceSasGetAllEnvelopeHeights", sceSasGetAllEnvelopeHeights},
    {0xCBCD4F79, "__sceSasSetSimpleADSR", sceSasSetSimpleADSR},
    {0xA0CF2FA4, "__sceSasSetKeyOff", sceSasSetKeyOff},
    {0x76F01ACA, "__sceSasSetKeyOn", sceSasSetKeyOn},
    {0xF983B186, "__sceSasRevVON", sceSasRevVON},
    {0xD5A229C9, "__sceSasRevEVOL", sceSasRevEVOL},
    {0x33D4AB37, "__sceSasRevType", sceSasRevType},
    {0x267A6DD2, "__sceSasRevParam", sceSasRevParam},
    {0x2C8E6AB3, "__sceSasGetPauseFlag", sceSasGetPauseFlag},
    {0x787D04D5, "__sceSasSetPause", sceSasSetPause},
    {0xBD11B7C2, "__sceSasGetGrain", sceSasGetGrain},
    {0xD1E0A01E, "__sceSasSetGrain", sceSasSetGrain},
    {0xE175EF66, "__sceSasGetOutputmode", sceSasGetOutputmode},
    {0xE855BF76, "__sceSasSetOutputmode", sceSasSetOutputmode},
    {0x4AA9EAD6, "__sceSasSetVoiceATRAC3", sceSasSetVoiceATRAC3},
    {0x7497EA85, "__sceSasConcatenateATRAC3", sceSasConcatenateATRAC3},
    {0xF6107F00, "__sceSasUnsetATRAC3", sceSasUnsetATRAC3},
};

}  // namespace

HLE_MODULE("sceSasCore", kSas);

}  // namespace vc2rt::sas
