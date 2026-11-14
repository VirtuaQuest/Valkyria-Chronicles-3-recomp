#pragma once
// Internal interfaces between the ATRAC3plus implementation files.
#include "atrac3plus.h"
#include "bit_reader.h"

namespace vc2rt::media::at3p {

// ---- bitstream parsing (atrac3plus_parse.cpp)
// Parses one channel unit (after its 2-bit type) into `u`; numChannels = 1 (mono) or 2 (stereo).
bool parseChannelUnit(atrac::BitReader& br, ChannelUnit& u, int numChannels);

// ---- signal processing (atrac3plus_dsp.cpp)
void dspInit();
// Inverse quantisation, power compensation and stereo processing of the parsed spectrum.
void dequantizeSpectrum(ChannelUnit& u, float out[2][kFrameSamples], int numChannels);
// IMDCT + windowing of one subband: 128 coefficients -> 256 windowed samples.
// windowId bit 1: steep window on the first half, bit 0: steep window on the second half.
// `in` is modified (odd subbands are spectrally reversed in place).
void imdctSubband(float* in, float* out, int windowId, int sb);
// Gain compensation of one subband's IMDCT output and overlap-add with the previous frame.
void gainCompensate(const float* in, float* overlap, const GainInfo& now, const GainInfo& next, float* out);
// Adds the synthesised sine waves of channel `c`, subband `sb` to `out` (128 samples).
void addTones(ChannelUnit& u, int c, int sb, float* out);
// 16-band inverse PQF: 16 x 128 subband samples -> 2048 PCM samples.
void ipqfSynthesis(IpqfHistory& hist, const float* in, float* out);

}  // namespace vc2rt::media::at3p
