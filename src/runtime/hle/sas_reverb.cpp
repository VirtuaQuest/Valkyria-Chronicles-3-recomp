// sceSas reverb.
//
// The SAS effect processor is the PlayStation SPU reverb (a pair of IIR "same/different side
// reflection" filters, four comb taps and two all-pass stages, running at 22.05 kHz) driven by a
// table of register presets. The processing below follows the published SPU reverb formula
// (nocash's psx-spx documentation); the preset register values are the PSP's (they match the
// documented PS1 presets except Room, Echo and Delay). Echo and Delay are computed presets:
// sceSasRevParam's delay/feedback rewrite seven of their registers.
//
// Addressing uses SPU register semantics: tap registers are in 8-byte units (4 samples of the
// 16-bit work area, which advances one sample per 22.05 kHz step) and the preset size is in
// bytes; every preset's size is exactly its largest tap * 8, which is what that scaling
// predicts. This gives musically sensible decay times (RT60 roughly 0.9 s Room, 1.9 s Hall,
// 3 s Space) and Echo delays of 6..740 ms over sceSasRevParam's 0..127.
//
// Approximations (documented, not bit exact):
//  * The hardware's 44.1 <-> 22.05 kHz conversion is replaced by a [1 2 1]/4 decimator and a
//    linear-interpolating upsampler, so there are no imaging artefacts. Levels follow the ME:
//    the send bus enters the reverb at a quarter of its level and the return is scaled by EVOL
//    (0x1000 = unity) at baseband.
//  * Type -1 (off) with the wet path enabled passes the send bus through at full rate, scaled
//    by EVOL, which is what the hardware's separate "no reverb" loop does.
#include "sas_reverb.h"

#include <algorithm>
#include <cstring>

namespace vc2rt::sas {

namespace {
// Work-area size followed by the 30 SPU reverb registers in hardware order: dAPF1 dAPF2 vIIR
// vCOMB1-4 vWALL vAPF1 vAPF2 mLSAME mRSAME mLCOMB1 mRCOMB1 mLCOMB2 mRCOMB2 dLSAME dRSAME mLDIFF
// mRDIFF mLCOMB3 mRCOMB3 mLCOMB4 mRCOMB4 dLDIFF dRDIFF mLAPF1 mRAPF1 mLAPF2 mRAPF2.
struct Preset {
    const char* name;
    s32 size;
    u16 reg[30];
};
const Preset kPresets[9] = {
    {"Room", 0x26C0,
     {0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374,
      0x01EF, 0x0336, 0x01B7, 0x0335, 0x01B6, 0x0334, 0x01B5, 0x0334, 0x01B5, 0x0334, 0x01B5, 0x01B4, 0x0136, 0x00B8, 0x005C}},
    {"Studio Small", 0x1F40,
     {0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00, 0x5280, 0x4EC0, 0x03E4, 0x031B, 0x03A4, 0x02AF, 0x0372,
      0x0266, 0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7, 0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026}},
    {"Studio Medium", 0x4840,
     {0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0, 0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F, 0x07A2,
      0x0616, 0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7, 0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080}},
    {"Studio Large", 0x6FE0,
     {0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680, 0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C, 0x0BD9,
      0x0973, 0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2, 0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA}},
    {"Hall", 0xADE0,
     {0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000, 0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2, 0x10BD, 0x11BC,
      0x0DC1, 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD, 0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A}},
    {"Space", 0xF6C0,
     {0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000, 0x6000, 0x5400, 0x1ED6, 0x1A31, 0x1D14, 0x183B, 0x1BC2,
      0x16B2, 0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D, 0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232}},
    {"Echo", 0x18040,
     {0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100, 0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000,
      0x0000, 0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004}},
    {"Delay", 0x18040,
     {0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000,
      0x0000, 0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004}},
    {"Pipe", 0x3C00,
     {0x0017, 0x0013, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0x8500, 0x5F80, 0x54C0, 0x0371, 0x02AF, 0x02E5, 0x01DF, 0x02B0,
      0x01D7, 0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059, 0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014}},
};
constexpr int kEcho = 6, kDelay = 7;

inline s32 clamp16(s64 v) { return (s32)std::clamp<s64>(v, -32768, 32767); }
// Q15 multiply, rounded: plain truncation makes the feedback loops settle into a small
// negative DC limit cycle instead of decaying to silence.
inline s32 mulq15(s32 a, s32 b) { return (s32)(((s64)a * b + 0x4000) >> 15); }
}  // namespace

Reverb::Reverb() {}

const char* Reverb::name(int type) {
    if (type == -1) return "Off";
    if (type < 0 || type > 8) return "?";
    return kPresets[type].name;
}

void Reverb::setType(int type) {
    if (type < -1 || type > 8 || type == type_) return;
    type_ = type;
    pos_ = 0;
    prevOddL_ = prevOddR_ = prevOutL_ = prevOutR_ = 0;
    if (type_ >= 0) ring_.assign((size_t)kPresets[type_].size / 2, 0);  // bytes -> samples
    else ring_.clear();
    load();
}

void Reverb::setParams(int delay, int feedback) {
    delay_ = delay;
    feedback_ = feedback;
    load();
}

void Reverb::load() {
    if (type_ < 0) return;
    const Preset& p = kPresets[type_];
    r_.size = p.size;
    s16* dst = &r_.dAPF1;
    for (int i = 0; i < 30; i++) dst[i] = (s16)p.reg[i];
    if (type_ == kEcho || type_ == kDelay) {
        // The ME re-derives the echo length and feedback from sceSasRevParam (D = delay + 1).
        int d16 = (delay_ + 1) * 16;
        r_.mLSAME = (s16)(d16 * 2 - r_.dAPF1);
        r_.mRSAME = (s16)(d16 - r_.dAPF2);
        r_.mLCOMB1 = (s16)(r_.mRCOMB1 + d16);
        r_.dLSAME = (s16)(r_.dRSAME + d16);
        r_.mLAPF1 = (s16)(r_.mLAPF2 + d16);
        r_.mRAPF1 = (s16)(d16 + (r_.mLAPF2 >> 1));
        r_.vWALL = (s16)(-(feedback_ << 8));
    }
}

s16& Reverb::at(int reg, int back) {
    int n = (int)ring_.size();
    int i = (pos_ + reg * 4 - back) % n;  // registers are in 8-byte (4-sample) units
    if (i < 0) i += n;
    return ring_[(size_t)i];
}

void Reverb::step22k(s32 Lin, s32 Rin, s32& outL, s32& outR) {
    const Regs& r = r_;
    auto reflect = [&](int dst, int src, s32 in) {
        s32 prev = at(dst, 1);  // the sample before the write position
        at(dst) = (s16)clamp16(mulq15(in + mulq15(at(src), r.vWALL) - prev, r.vIIR) + prev);
    };
    // same side and cross-side reflections (each an IIR low-passed, wall-attenuated feedback)
    reflect(r.mLSAME, r.dLSAME, Lin);
    reflect(r.mRSAME, r.dRSAME, Rin);
    reflect(r.mLDIFF, r.dRDIFF, Lin);
    reflect(r.mRDIFF, r.dLDIFF, Rin);
    // early echo: comb taps
    s32 L = mulq15(r.vCOMB1, at(r.mLCOMB1)) + mulq15(r.vCOMB2, at(r.mLCOMB2)) + mulq15(r.vCOMB3, at(r.mLCOMB3)) +
            mulq15(r.vCOMB4, at(r.mLCOMB4));
    s32 R = mulq15(r.vCOMB1, at(r.mRCOMB1)) + mulq15(r.vCOMB2, at(r.mRCOMB2)) + mulq15(r.vCOMB3, at(r.mRCOMB3)) +
            mulq15(r.vCOMB4, at(r.mRCOMB4));
    // late reverb: two all-pass stages
    auto allpass = [&](s32 x, int m, int d, s16 v) {
        s32 delayed = at(m - d);
        s32 t = clamp16(x - mulq15(v, delayed));
        at(m) = (s16)t;
        return mulq15(t, v) + delayed;
    };
    L = allpass(L, r.mLAPF1, r.dAPF1, r.vAPF1);
    R = allpass(R, r.mRAPF1, r.dAPF1, r.vAPF1);
    L = allpass(L, r.mLAPF2, r.dAPF2, r.vAPF2);
    R = allpass(R, r.mRAPF2, r.dAPF2, r.vAPF2);
    outL = clamp16(L);
    outR = clamp16(R);
    pos_ = (pos_ + 1) % (int)ring_.size();
}

void Reverb::process(const s32* send, s32* out, int frames, int volL, int volR) {
    if (type_ < 0) {
        for (int i = 0; i < frames; i++) {
            out[i * 2] = (clamp16(send[i * 2]) * volL) >> 12;
            out[i * 2 + 1] = (clamp16(send[i * 2 + 1]) * volR) >> 12;
        }
        return;
    }
    for (int i = 0; i + 1 < frames; i += 2) {
        s32 eL = clamp16(send[i * 2]), eR = clamp16(send[i * 2 + 1]);
        s32 oL = clamp16(send[i * 2 + 2]), oR = clamp16(send[i * 2 + 3]);
        s32 inL = (prevOddL_ + 2 * eL + oL) >> 2;
        s32 inR = (prevOddR_ + 2 * eR + oR) >> 2;
        prevOddL_ = oL;
        prevOddR_ = oR;
        s32 yL, yR;
        step22k(inL >> 2, inR >> 2, yL, yR);
        yL = (yL * volL) >> 12;
        yR = (yR * volR) >> 12;
        out[i * 2] = (prevOutL_ + yL) >> 1;
        out[i * 2 + 1] = (prevOutR_ + yR) >> 1;
        out[i * 2 + 2] = yL;
        out[i * 2 + 3] = yR;
        prevOutL_ = yL;
        prevOutR_ = yR;
    }
}

}  // namespace vc2rt::sas
