#pragma once
// ATRAC3plus decoder (Sony's PSP / MiniDisc / OpenMG codec, "at3+").
//
// One frame = 2048 samples per channel, coded as a sequence of channel units (mono or jointly
// coded stereo). Each unit carries, per quantisation unit (QU), word lengths, scale factors and
// Huffman code-table selectors, then the Huffman-coded MDCT spectrum, followed by per-subband
// window shapes, gain-control envelopes, optional sinusoidal "tones" (GHA) and noise-fill
// parameters. Synthesis: inverse quantisation + power-compensation noise -> 16 x 256-point IMDCT
// (one per PQF subband) -> gain compensation / overlap-add -> added tones -> 16-band inverse
// PQF filter bank -> PCM.
//
// The bitstream syntax and constant tables follow FFmpeg's ATRAC3+ decoder (the tables in
// atrac3plus_tables.cpp are LGPL-2.1+, see LICENSE-NOTICE.md); this implementation is the
// project's own.
#include <cstddef>
#include <cstdint>
#include <memory>

namespace vc2rt::media::at3p {

constexpr int kSubbands = 16;
constexpr int kSubbandSamples = 128;
constexpr int kFrameSamples = kSubbands * kSubbandSamples;  // 2048
constexpr int kMaxQuantUnits = 32;
constexpr int kMaxTones = 48;

enum UnitType : int { kUnitMono = 0, kUnitStereo = 1, kUnitExtension = 2, kUnitTerminator = 3 };

struct GainInfo {
    int numPoints = 0;
    int lev[7] = {};  // gain level code at each control point
    int loc[7] = {};  // location code (units of 4 samples) of each control point
};

struct WaveEnvelope {
    int hasStart = 0, hasStop = 0;
    int startPos = 0, stopPos = 0;  // in units of 4 samples
};

struct WaveGroup {            // the sine waves of one subband
    WaveEnvelope pending;     // envelope as transmitted in this frame
    WaveEnvelope current;     // full envelope reconstructed for synthesis
    int numWaves = 0;
    int startIndex = 0;       // first entry in WaveSynth::waves
};

struct WaveParam {
    int freqIndex = 0, ampSf = 0, ampIndex = 0, phaseIndex = 0;
};

struct WaveSynth {            // tone parameters of a whole channel unit for one frame
    int tonesPresent = 0;
    int amplitudeMode = 0;
    int numToneBands = 0;
    uint8_t toneSharing[kSubbands] = {};
    uint8_t toneMaster[kSubbands] = {};
    uint8_t invertPhase[kSubbands] = {};
    int tonesIndex = 0;
    WaveParam waves[kMaxTones];
};

struct ChannelState {
    int chNum = 0;
    int numCodedVals = 0, fillMode = 0, splitPoint = 0, tableType = 0;
    int wordLen[kMaxQuantUnits] = {};
    int sfIdx[kMaxQuantUnits] = {};
    int tabIdx[kMaxQuantUnits] = {};
    int16_t spectrum[kFrameSamples] = {};
    uint8_t powerLevs[5] = {};
    int numGainSubbands = 0;
    // two-frame histories, indexed through ChannelUnit::cur (current) / cur ^ 1 (previous)
    uint8_t wndShape[2][kSubbands] = {};
    GainInfo gain[2][kSubbands];
    WaveGroup tones[2][kSubbands];
};

struct IpqfHistory {
    float buf1[24][8] = {};
    float buf2[24][8] = {};
    int pos = 0;
};

struct ChannelUnit {
    int unitType = kUnitMono;
    int numQuantUnits = 0;
    int numSubbands = 0;
    int usedQuantUnits = 0;
    int numCodedSubbands = 0;
    int muteFlag = 0;
    int useFullTable = 0;
    int noisePresent = 0, noiseLevelIndex = 0, noiseTableIndex = 0;
    uint8_t swapChannels[kSubbands] = {};
    uint8_t negateCoeffs[kSubbands] = {};
    ChannelState ch[2];
    WaveSynth waves[2];
    int cur = 0;  // which half of the histories is the current frame

    IpqfHistory ipqf[2];
    float overlap[2][kFrameSamples] = {};  // second IMDCT halves awaiting overlap-add

    // accessors for the double-buffered state
    uint8_t* wnd(int c) { return ch[c].wndShape[cur]; }
    uint8_t* wndPrev(int c) { return ch[c].wndShape[cur ^ 1]; }
    GainInfo* gain(int c) { return ch[c].gain[cur]; }
    GainInfo* gainPrev(int c) { return ch[c].gain[cur ^ 1]; }
    WaveGroup* tones(int c) { return ch[c].tones[cur]; }
    WaveGroup* tonesPrev(int c) { return ch[c].tones[cur ^ 1]; }
    WaveSynth& wavesCur() { return waves[cur]; }
    WaveSynth& wavesPrev() { return waves[cur ^ 1]; }
};

class Decoder {
public:
    Decoder();
    ~Decoder();
    // channels: 1..8 (the PSP uses 1 or 2). Returns false for an unsupported layout.
    bool init(int channels);
    int channels() const { return channels_; }
    // Decodes one frame into planar float output (nominal range +-1.0), out[c] for each of
    // channels() channels, kFrameSamples each. On failure the output is silence and false is
    // returned (the inter-frame state is kept, as the PSP and the reference decoder do).
    bool decodeFrame(const uint8_t* data, size_t size, float* const* out);
    // Clears all inter-frame state (after a seek or loop).
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int channels_ = 0;
};

}  // namespace vc2rt::media::at3p
