// ATRAC3plus frame decoding: channel-unit sequencing and per-unit reconstruction.
#include <cstring>
#include <new>

#include "atrac3plus_internal.h"

namespace vc2rt::media::at3p {

struct Decoder::Impl {
    int numUnits = 0;
    int unitTypes[5] = {};
    ChannelUnit units[5];
    atrac::FrameBuffer frame;

    // per-unit work buffers
    float spectrum[2][kFrameSamples];
    float imdctOut[256];
    float timeSignal[2][kFrameSamples];
    float pcm[2][kFrameSamples];

    void reconstruct(ChannelUnit& u, int numChannels);
};

namespace {

// Channel-unit layouts by channel count (as the ATRAC3plus "channel configuration" defines them).
bool layoutFor(int channels, int& count, int* types) {
    static const int kLayouts[9][5] = {
        {}, {kUnitMono}, {kUnitStereo}, {kUnitStereo, kUnitMono}, {kUnitStereo, kUnitMono, kUnitMono}, {},
        {kUnitStereo, kUnitMono, kUnitStereo, kUnitMono},
        {kUnitStereo, kUnitMono, kUnitStereo, kUnitMono, kUnitMono},
        {kUnitStereo, kUnitMono, kUnitStereo, kUnitStereo, kUnitMono},
    };
    static const int kCounts[9] = {0, 1, 1, 2, 3, 0, 4, 5, 5};
    if (channels < 1 || channels > 8 || !kCounts[channels]) return false;
    count = kCounts[channels];
    for (int i = 0; i < count; i++) types[i] = kLayouts[channels][i];
    return true;
}

}  // namespace

void Decoder::Impl::reconstruct(ChannelUnit& u, int numChannels) {
    for (int c = 0; c < numChannels; c++) {
        float* time = timeSignal[c];
        const uint8_t* wnd = u.wnd(c);
        const uint8_t* wndPrev = u.wndPrev(c);
        for (int sb = 0; sb < u.numSubbands; sb++) {
            const int off = sb * kSubbandSamples;
            imdctSubband(spectrum[c] + off, imdctOut, (wndPrev[sb] << 1) | wnd[sb], sb);
            gainCompensate(imdctOut, u.overlap[c] + off, u.gainPrev(c)[sb], u.gain(c)[sb], time + off);
        }
        // subbands beyond this frame's bandwidth are silent and leave no overlap behind
        const size_t tail = (size_t)(kSubbands - u.numSubbands) * kSubbandSamples * sizeof(float);
        memset(u.overlap[c] + u.numSubbands * kSubbandSamples, 0, tail);
        memset(time + u.numSubbands * kSubbandSamples, 0, tail);

        if (u.wavesCur().tonesPresent || u.wavesPrev().tonesPresent) {
            for (int sb = 0; sb < u.numSubbands; sb++)
                if (u.tones(c)[sb].numWaves || u.tonesPrev(c)[sb].numWaves) addTones(u, c, sb, time + sb * kSubbandSamples);
        }
        ipqfSynthesis(u.ipqf[c], time, pcm[c]);
    }
    u.cur ^= 1;  // this frame's window shapes, gain data and tones become the history
}

Decoder::Decoder() : impl_(std::make_unique<Impl>()) { dspInit(); }
Decoder::~Decoder() = default;

bool Decoder::init(int channels) {
    reset();
    int count = 0, types[5] = {};
    if (!layoutFor(channels, count, types)) {
        channels_ = 0;
        impl_->numUnits = 0;
        return false;
    }
    channels_ = channels;
    impl_->numUnits = count;
    for (int i = 0; i < count; i++) impl_->unitTypes[i] = types[i];
    return true;
}

void Decoder::reset() {
    for (ChannelUnit& u : impl_->units) {
        u.~ChannelUnit();
        new (&u) ChannelUnit();
        u.ch[0].chNum = 0;
        u.ch[1].chNum = 1;
    }
}

bool Decoder::decodeFrame(const uint8_t* data, size_t size, float* const* out) {
    Impl& d = *impl_;
    auto silence = [&] {
        for (int c = 0; c < channels_; c++) memset(out[c], 0, sizeof(float) * kFrameSamples);
        return false;
    };
    if (!d.numUnits) return silence();

    atrac::BitReader br;
    br.init(d.frame.load(data, size), size);
    if (br.read1()) return silence();  // the start bit must be 0

    int unit = 0, outCh = 0;
    while (br.bitsLeft() >= 2) {
        const int type = (int)br.read(2);
        if (type == kUnitTerminator) break;
        if (type == kUnitExtension || unit >= d.numUnits || d.unitTypes[unit] != type) return silence();

        ChannelUnit& u = d.units[unit];
        u.unitType = type;
        const int nch = type + 1;
        if (!parseChannelUnit(br, u, nch)) return silence();
        dequantizeSpectrum(u, d.spectrum, nch);
        d.reconstruct(u, nch);
        for (int c = 0; c < nch && outCh + c < channels_; c++) memcpy(out[outCh + c], d.pcm[c], sizeof(float) * kFrameSamples);
        outCh += nch;
        unit++;
    }
    // channels of units missing from a short frame stay silent
    for (int c = outCh; c < channels_; c++) memset(out[c], 0, sizeof(float) * kFrameSamples);
    return true;
}

}  // namespace vc2rt::media::at3p
