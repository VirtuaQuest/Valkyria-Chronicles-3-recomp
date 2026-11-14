#pragma once
// sceSas reverb ("waveform effect"): the SPU-style reverb the PSP's SAS runs on its send bus.
#include <vector>

#include "../core/runtime.h"

namespace vc2rt::sas {

class Reverb {
public:
    Reverb();
    // -1 = off (the send bus is passed through, scaled by the effect volume), 0..8 = presets
    // Room, Studio S/M/L, Hall, Space, Echo, Delay, Pipe.
    void setType(int type);
    int type() const { return type_; }
    // sceSasRevParam: delay and feedback (0..127); only Echo and Delay derive registers from them.
    void setParams(int delay, int feedback);
    // send: interleaved stereo 44.1 kHz send-bus mix (grain frames). out: interleaved stereo wet
    // signal at 44.1 kHz. vol*: sceSasRevEVOL (0..0x1000).
    void process(const s32* send, s32* out, int frames, int volL, int volR);
    static const char* name(int type);

private:
    struct Regs {
        s32 size;  // work area length in bytes
        s16 dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL;
        s16 vAPF1, vAPF2, mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2;
        s16 dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4;
        s16 dLDIFF, dRDIFF, mLAPF1, mRAPF1, mLAPF2, mRAPF2;
    };
    void load();
    s16& at(int reg, int back = 0);
    void step22k(s32 inL, s32 inR, s32& outL, s32& outR);

    int type_ = -1;
    int delay_ = 0, feedback_ = 0;
    Regs r_{};
    std::vector<s16> ring_;
    int pos_ = 0;
    // resampler state (44.1k <-> 22.05k)
    s32 prevOddL_ = 0, prevOddR_ = 0;
    s32 prevOutL_ = 0, prevOutR_ = 0;
};

}  // namespace vc2rt::sas
