// ATRAC3plus synthesis: inverse quantisation with power-compensation noise, per-subband IMDCT
// with the two ATRAC3plus window shapes, gain compensation, sine-wave (tone) resynthesis and
// the 16-band inverse pseudo-QMF filter bank.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <mutex>

#include "atrac3plus_internal.h"
#include "atrac3plus_tables.h"

namespace vc2rt::media::at3p {
namespace {

namespace T = tables;
constexpr double kPi = 3.14159265358979323846;

// ------------------------------------------------------------------ derived tables
struct DspTables {
    float sine64[64], sine128[128];  // MDCT sine windows (rising halves)
    float waveTable[2048];           // one sine period for tone synthesis
    float hann[256];                 // Hann window used to cross-fade tones
    float ampSf[64];                 // tone amplitude scale factors
    float gainLevel[16];             // gain compensation: level code -> factor
    float gainInterp[31];            // per-sample interpolation step between two levels

    // 128-point DCT-IV via a 64-point complex FFT (for the 256-point IMDCT)
    std::complex<float> preTw[64], postTw[64], fftTw[32];
    uint8_t bitrev[64];
    // 16-point DCT-IV matrix of the IPQF, with the IPQF scale (1/1024) and sign folded in
    float ipqfDct[16][16];
};

DspTables g_t;

void buildTables() {
    for (int i = 0; i < 64; i++) g_t.sine64[i] = sinf((float)((i + 0.5) * (kPi / (2.0 * 64))));
    for (int i = 0; i < 128; i++) g_t.sine128[i] = sinf((float)((i + 0.5) * (kPi / (2.0 * 128))));
    for (int i = 0; i < 2048; i++) g_t.waveTable[i] = (float)sin(2 * kPi * i / 2048);
    for (int i = 0; i < 256; i++) g_t.hann[i] = (float)((1.0f - cos(2 * kPi * i / 256.0f)) * 0.5f);
    for (int i = 0; i < 64; i++) g_t.ampSf[i] = exp2f((i - 3) / 4.0f);
    for (int i = 0; i < 16; i++) g_t.gainLevel[i] = powf(2.0f, (float)(6 - i));
    for (int i = -15; i < 16; i++) g_t.gainInterp[i + 15] = powf(2.0f, -0.25f * (float)i);

    constexpr int M = 128;
    for (int k = 0; k < M / 2; k++) {
        g_t.preTw[k] = std::polar(1.0f, (float)(-kPi * (k + 0.125) / M));
        g_t.postTw[k] = std::polar(1.0f, (float)(-kPi * (k + 0.125) / M));
    }
    for (int k = 0; k < 32; k++) g_t.fftTw[k] = std::polar(1.0f, (float)(-2 * kPi * k / 64));
    for (int i = 0; i < 64; i++) {
        int r = 0;
        for (int b = 0; b < 6; b++) r |= ((i >> b) & 1) << (5 - b);
        g_t.bitrev[i] = (uint8_t)r;
    }
    // Middle half of a 32-point IMDCT: y[n] = s * sum_k in[k] cos(pi/16 (15 - n + 1/2)(k + 1/2)),
    // s = 32/32768 (the output scale of the filter bank).
    for (int n = 0; n < 16; n++)
        for (int k = 0; k < 16; k++)
            g_t.ipqfDct[n][k] = (float)((32.0 / 32768.0) * cos(kPi / 16 * (15 - n + 0.5) * (k + 0.5)));
}

// In-place 64-point complex FFT (forward, unnormalised), iterative radix 2.
void fft64(std::complex<float>* z) {
    for (int i = 0; i < 64; i++) {
        const int j = g_t.bitrev[i];
        if (j > i) std::swap(z[i], z[j]);
    }
    for (int len = 2; len <= 64; len <<= 1) {
        const int half = len >> 1, step = 64 / len;
        for (int base = 0; base < 64; base += len) {
            for (int j = 0; j < half; j++) {
                const std::complex<float> w = g_t.fftTw[j * step];
                const std::complex<float> a = z[base + j];
                const std::complex<float> b = z[base + j + half] * w;
                z[base + j] = a + b;
                z[base + j + half] = a - b;
            }
        }
    }
}

// u[n] = sum_k x[k] cos(pi/128 (n + 1/2)(k + 1/2)), n, k = 0..127
void dct4_128(const float* x, float* u) {
    constexpr int M = 128;
    std::complex<float> z[M / 2];
    for (int k = 0; k < M / 2; k++) z[k] = std::complex<float>(x[2 * k], x[M - 1 - 2 * k]) * g_t.preTw[k];
    fft64(z);
    for (int p = 0; p < M / 2; p++) {
        const std::complex<float> c = z[p] * g_t.postTw[p];
        u[2 * p] = c.real();
        u[M - 1 - 2 * p] = -c.imag();
    }
}

// ------------------------------------------------------------------ power compensation
// Adds shaped noise to quantisation units whose energy was lost to coarse quantisation.
void powerCompensation(ChannelUnit& u, int c, float* sp, int rngIndex, int sb) {
    const int swapCh = (u.unitType == kUnitStereo && u.swapChannels[sb]) ? 1 : 0;
    const int src = c ^ swapCh;
    const int level = u.ch[src].powerLevs[T::subband_to_powgrp[sb]];
    if (level == 15) return;  // off

    float noise[kSubbandSamples];
    for (int i = 0; i < kSubbandSamples; i++) noise[i] = T::noise_tab[(rngIndex + i) & 0x3FF];

    // Reduce the noise where gain control boosts the signal.
    const GainInfo& g1 = u.gain(src)[sb];
    const GainInfo& g2 = u.gainPrev(src)[sb];
    const int gainLev = g1.numPoints > 0 ? 6 - g1.lev[0] : 0;
    int gcv = 0;
    for (int i = 0; i < g2.numPoints; i++) gcv = std::max(gcv, gainLev - (g2.lev[i] - 6));
    for (int i = 0; i < g1.numPoints; i++) gcv = std::max(gcv, 6 - g1.lev[i]);
    const float groupLevel = T::pwc_levs[level] / (float)(1 << gcv);

    // the lowest two QUs (0..351 Hz) of subband 0 are left alone
    const ChannelState& ch = u.ch[c];
    for (int qu = T::subband_to_qu[sb] + (sb == 0 ? 2 : 0); qu < T::subband_to_qu[sb + 1]; qu++) {
        const int wl = ch.wordLen[qu];
        if (wl <= 0) continue;
        const float quLevel = T::sf_tab[ch.sfIdx[qu]] * T::mant_tab[wl] / (float)(1 << wl) * groupLevel;
        float* dst = sp + T::qu_to_spec_pos[qu];
        const int n = T::qu_to_spec_pos[qu + 1] - T::qu_to_spec_pos[qu];
        for (int i = 0; i < n; i++) dst[i] += noise[i] * quLevel;
    }
}

// ------------------------------------------------------------------ tone synthesis
void synthWaves(const WaveSynth& synth, const WaveGroup& group, const WaveEnvelope& env, bool invert, int regionOffset,
                float* out) {
    for (int w = 0; w < group.numWaves; w++) {
        const WaveParam& p = synth.waves[group.startIndex + w];
        const float ampF = g_t.ampSf[p.ampSf] * (!synth.amplitudeMode ? (p.ampIndex + 1) / 15.13f : 1.0f);
        const double amp = ampF;
        const int inc = p.freqIndex;
        int pos = (((p.phaseIndex & 0x1F) << 6) - (regionOffset ^ 128) * inc) & 2047;
        for (int i = 0; i < kSubbandSamples; i++) {
            out[i] = (float)(out[i] + g_t.waveTable[pos] * amp);
            pos = (pos + inc) & 2047;
        }
    }
    if (invert)
        for (int i = 0; i < kSubbandSamples; i++) out[i] *= -1.0f;

    if (env.hasStart) {  // fade in with a steep Hann slope
        const int pos = (env.startPos << 2) - regionOffset;
        if (pos > 0 && pos <= kSubbandSamples) {
            memset(out, 0, (size_t)pos * sizeof(float));
            if (!env.hasStop || env.startPos != env.stopPos) {
                for (int i = 0; i < 4 && pos + i < kSubbandSamples; i++) out[pos + i] *= g_t.hann[i * 32];
            }
        }
    }
    if (env.hasStop) {  // fade out
        const int pos = ((env.stopPos + 1) << 2) - regionOffset;
        if (pos > 0 && pos <= kSubbandSamples) {
            for (int i = 0; i < 4 && pos - 4 + i >= 0; i++) out[pos - 4 + i] *= g_t.hann[96 - i * 32];
            memset(out + pos, 0, (size_t)(kSubbandSamples - pos) * sizeof(float));
        }
    }
}

}  // namespace

void dspInit() {
    static std::once_flag once;
    std::call_once(once, buildTables);
}

void dequantizeSpectrum(ChannelUnit& u, float out[2][kFrameSamples], int numChannels) {
    if (u.muteFlag) {
        for (int c = 0; c < numChannels; c++) memset(out[c], 0, sizeof(float) * kFrameSamples);
        return;
    }
    // noise table start per subband, derived from the scale factors
    int rng = 0;
    for (int qu = 0; qu < u.usedQuantUnits; qu++) rng += u.ch[0].sfIdx[qu] + u.ch[1].sfIdx[qu];
    int sbRng[kSubbands] = {};
    for (int sb = 0; sb < u.numCodedSubbands; sb++, rng += 128) sbRng[sb] = rng & 0x3FC;

    for (int c = 0; c < numChannels; c++) {
        const ChannelState& ch = u.ch[c];
        memset(out[c], 0, sizeof(float) * kFrameSamples);
        for (int qu = 0; qu < u.usedQuantUnits; qu++) {
            const int wl = ch.wordLen[qu];
            if (wl <= 0) continue;
            const int begin = T::qu_to_spec_pos[qu], end = T::qu_to_spec_pos[qu + 1];
            const float q = T::sf_tab[ch.sfIdx[qu]] * T::mant_tab[wl];
            for (int i = begin; i < end; i++) out[c][i] = ch.spectrum[i] * q;
        }
        for (int sb = 0; sb < u.numCodedSubbands; sb++) powerCompensation(u, c, out[c], sbRng[sb], sb);
    }

    if (u.unitType == kUnitStereo) {
        for (int sb = 0; sb < u.numCodedSubbands; sb++) {
            float* l = out[0] + sb * kSubbandSamples;
            float* r = out[1] + sb * kSubbandSamples;
            if (u.swapChannels[sb])
                for (int i = 0; i < kSubbandSamples; i++) std::swap(l[i], r[i]);
            if (u.negateCoeffs[sb])
                for (int i = 0; i < kSubbandSamples; i++) r[i] = -r[i];
        }
    }
}

void imdctSubband(float* in, float* out, int windowId, int sb) {
    if (sb & 1)  // odd subbands are spectrally inverted by the QMF
        std::reverse(in, in + kSubbandSamples);

    // 256-point IMDCT y[n] = sum_k in[k] cos(pi/128 (n + 1/2 + 64)(k + 1/2)), unscaled, unfolded
    // from a 128-point DCT-IV by its symmetries.
    float u[128];
    dct4_128(in, u);
    for (int n = 0; n < 64; n++) out[n] = u[n + 64];
    for (int n = 64; n < 192; n++) out[n] = -u[191 - n];
    for (int n = 192; n < 256; n++) out[n] = -u[n - 192];

    if (windowId & 2) {  // steep first half: 32 zeros, 64-sample sine slope, 32 ones
        memset(out, 0, 32 * sizeof(float));
        for (int i = 0; i < 64; i++) out[32 + i] *= g_t.sine64[i];
    } else {
        for (int i = 0; i < 128; i++) out[i] *= g_t.sine128[i];
    }
    if (windowId & 1) {  // steep second half
        for (int i = 0; i < 64; i++) out[160 + i] *= g_t.sine64[63 - i];
        memset(out + 224, 0, 32 * sizeof(float));
    } else {
        for (int i = 0; i < 128; i++) out[128 + i] *= g_t.sine128[127 - i];
    }
}

void gainCompensate(const float* in, float* overlap, const GainInfo& now, const GainInfo& next, float* out) {
    constexpr int N = kSubbandSamples;
    const float scale = next.numPoints ? g_t.gainLevel[next.lev[0]] : 1.0f;
    int pos = 0;
    if (now.numPoints) {
        for (int i = 0; i < now.numPoints; i++) {
            const int lastPos = now.loc[i] << 2;
            float lev = g_t.gainLevel[now.lev[i]];
            const int nextLev = i + 1 < now.numPoints ? now.lev[i + 1] : 6;
            const float step = g_t.gainInterp[nextLev - now.lev[i] + 15];
            for (; pos < lastPos; pos++) out[pos] = (in[pos] * scale + overlap[pos]) * lev;
            for (; pos < lastPos + 4; pos++) {  // interpolate to the next level over 4 samples
                out[pos] = (in[pos] * scale + overlap[pos]) * lev;
                lev *= step;
            }
        }
    }
    for (; pos < N; pos++) out[pos] = in[pos] * scale + overlap[pos];
    memcpy(overlap, in + N, N * sizeof(float));
}

void addTones(ChannelUnit& u, int c, int sb, float* out) {
    float reg1[kSubbandSamples] = {}, reg2[kSubbandSamples] = {};
    WaveGroup& now = u.tonesPrev(c)[sb];
    WaveGroup& next = u.tones(c)[sb];

    // Rebuild the full envelope of this frame's waves from the truncated bitstream data.
    WaveEnvelope& e = next.current;
    if (next.pending.hasStart && next.pending.startPos < next.pending.stopPos) {
        e.hasStart = 1;
        e.startPos = next.pending.startPos + 32;
    } else if (now.pending.hasStart) {
        e.hasStart = 1;
        e.startPos = now.pending.startPos;
    } else {
        e.hasStart = 0;
        e.startPos = 0;
    }
    if (now.pending.hasStop && now.pending.stopPos >= e.startPos) {
        e.hasStop = 1;
        e.stopPos = now.pending.stopPos;
    } else if (next.pending.hasStop) {
        e.hasStop = 1;
        e.stopPos = next.pending.stopPos + 32;
    } else {
        e.hasStop = 0;
        e.stopPos = 64;
    }

    // which parts of the two overlapping windows are audible
    const bool reg1On = now.current.stopPos >= 32;
    const bool reg2On = next.current.startPos < 32;

    if (now.numWaves && reg1On)
        synthWaves(u.wavesPrev(), now, now.current, (u.wavesPrev().invertPhase[sb] & c) != 0, 128, reg1);
    if (next.numWaves && reg2On)
        synthWaves(u.wavesCur(), next, next.current, (u.wavesCur().invertPhase[sb] & c) != 0, 0, reg2);

    // Cross-fade with the Hann window: always when both frames have audible waves, otherwise only
    // the parts without an explicit envelope edge.
    bool fade1, fade2;
    if (now.numWaves && next.numWaves && reg1On && reg2On) {
        fade1 = fade2 = true;
    } else {
        fade1 = now.numWaves && !now.current.hasStop;
        fade2 = next.numWaves && !next.current.hasStart;
    }
    if (fade1)
        for (int i = 0; i < kSubbandSamples; i++) reg1[i] *= g_t.hann[128 + i];
    if (fade2)
        for (int i = 0; i < kSubbandSamples; i++) reg2[i] *= g_t.hann[i];

    for (int i = 0; i < kSubbandSamples; i++) out[i] += reg1[i] + reg2[i];
}

void ipqfSynthesis(IpqfHistory& h, const float* in, float* out) {
    memset(out, 0, sizeof(float) * kFrameSamples);
    for (int s = 0; s < kSubbandSamples; s++) {
        // one sample of each subband through the cosine modulation (16-point DCT-IV)
        float x[kSubbands];
        for (int sb = 0; sb < kSubbands; sb++) x[sb] = in[sb * kSubbandSamples + s];
        float y[kSubbands];
        for (int n = 0; n < kSubbands; n++) {
            float acc = 0.0f;
            for (int k = 0; k < kSubbands; k++) acc += g_t.ipqfDct[n][k] * x[k];
            y[n] = acc;
        }
        for (int i = 0; i < 8; i++) {
            h.buf1[h.pos][i] = y[i + 8];
            h.buf2[h.pos][i] = y[7 - i];
        }

        // 384-tap prototype filter over the 24-entry history ring
        float* o = out + s * kSubbands;
        int p = h.pos;
        for (int t = 0; t < 12; t++) {
            const float* b1 = h.buf1[p];
            const float* b2 = h.buf2[(p + 1) % 24];
            const float* c1 = T::ipqf_coeffs1[t];
            const float* c2 = T::ipqf_coeffs2[t];
            for (int i = 0; i < 8; i++) o[i] += b1[i] * c1[i] + b2[i] * c2[i];
            for (int i = 0; i < 8; i++) o[8 + i] += b1[7 - i] * c1[8 + i] + b2[7 - i] * c2[8 + i];
            p = (p + 2) % 24;
        }
        h.pos = (h.pos + 23) % 24;
    }
}

}  // namespace vc2rt::media::at3p
