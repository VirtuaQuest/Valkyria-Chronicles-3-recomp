// ATRAC3plus channel-unit bitstream parser.
//
// Every parameter group (word lengths, scale factors, code tables, gain control, tones) has up
// to four coding modes, selected by a 2-bit field. The first ("master") channel of a stereo unit
// is coded on its own; the second ("slave") channel may be coded relative to the master.
#include <algorithm>
#include <cstring>
#include <mutex>

#include "atrac3plus_internal.h"
#include "atrac3plus_tables.h"

namespace vc2rt::media::at3p {
namespace {

using atrac::BitReader;
using atrac::signExtend;
using atrac::Vlc;
namespace T = tables;

// ------------------------------------------------------------------ VLC set (built once)
struct VlcSet {
    Vlc wl[4], ct[4], sf[8], spec[112], gain[11], tone[7];
};

const VlcSet& vlcs() {
    static VlcSet s;
    static std::once_flag once;
    std::call_once(once, [] {
        auto expl = [](Vlc& v, const T::ExplicitVlc& d) { v.buildExplicit(d.num_codes, d.bits, d.codes8, d.codes16, d.xlat); };
        for (int i = 0; i < 4; i++) expl(s.wl[i], T::wl_vlcs[i]);
        for (int i = 0; i < 4; i++) expl(s.ct[i], T::ct_vlcs[i]);
        for (int i = 0; i < 8; i++) expl(s.sf[i], T::sf_vlcs[i]);
        for (int i = 0; i < 112; i++)
            if (T::spectra_tabs[i].cb) s.spec[i].buildCanonical(T::spectra_tabs[i].cb, T::spectra_tabs[i].xlat);
        for (int i = 0; i < 11; i++) s.gain[i].buildCanonical(T::gain_vlcs[i].cb, T::gain_vlcs[i].xlat);
        for (int i = 0; i < 7; i++) s.tone[i].buildCanonical(T::tone_vlcs[i].cb, T::tone_vlcs[i].xlat);
    });
    return s;
}

inline int ilog2(unsigned v) {
    int n = 0;
    while (v >>= 1) n++;
    return n;
}

// Reads `n` subband flags: '0' = all clear, '10' = all set, '11' + n explicit bits.
int readSubbandFlags(BitReader& br, uint8_t* out, int n) {
    memset(out, 0, (size_t)n);
    const int any = br.read1();
    if (any) {
        if (br.read1()) {
            for (int i = 0; i < n; i++) out[i] = (uint8_t)br.read1();
        } else {
            memset(out, 1, (size_t)n);
        }
    }
    return any;
}

// Expands a vector-quantised shape: the first three values are the start value, the rest are
// offset per segment.
void applyShape(int start, const int8_t* shape, int* dst, int count) {
    if (!count) return;
    dst[0] = dst[1] = dst[2] = start;
    for (int i = 3; i < count; i++) dst[i] = start - shape[T::qu_num_to_seg[i] - 1];
}

class UnitParser {
public:
    UnitParser(BitReader& br, ChannelUnit& u, int numChannels) : br_(br), u_(u), nch_(numChannels), v_(vlcs()) {}

    bool parse() {
        u_.numQuantUnits = (int)br_.read(5) + 1;
        if (u_.numQuantUnits > 28 && u_.numQuantUnits < 32) return false;
        u_.muteFlag = br_.read1();

        for (int c = 0; c < nch_; c++) {
            memset(u_.ch[c].wordLen, 0, sizeof u_.ch[c].wordLen);
            if (!wordLengths(c)) return false;
        }
        int used = u_.numQuantUnits - 1;
        while (used >= 0 && !u_.ch[0].wordLen[used] && !(nch_ == 2 && u_.ch[1].wordLen[used])) used--;
        u_.usedQuantUnits = used + 1;
        u_.numSubbands = T::qu_to_subband[u_.numQuantUnits - 1] + 1;
        u_.numCodedSubbands = u_.usedQuantUnits ? T::qu_to_subband[u_.usedQuantUnits - 1] + 1 : 0;

        if (u_.usedQuantUnits) {
            for (int c = 0; c < nch_; c++) {
                memset(u_.ch[c].sfIdx, 0, sizeof u_.ch[c].sfIdx);
                if (!scaleFactors(c)) return false;
            }
            u_.useFullTable = br_.read1();
            for (int c = 0; c < nch_; c++) {
                memset(u_.ch[c].tabIdx, 0, sizeof u_.ch[c].tabIdx);
                if (!codeTables(c)) return false;
            }
        }

        for (int c = 0; c < nch_; c++)
            if (!spectrum(c)) return false;

        if (nch_ == 2) {
            readSubbandFlags(br_, u_.swapChannels, u_.numCodedSubbands);
            readSubbandFlags(br_, u_.negateCoeffs, u_.numCodedSubbands);
        }
        for (int c = 0; c < nch_; c++) readSubbandFlags(br_, u_.wnd(c), u_.numSubbands);

        for (int c = 0; c < nch_; c++)
            if (!gainControl(c)) return false;

        if (!tones()) return false;

        u_.noisePresent = br_.read1();
        if (u_.noisePresent) {
            u_.noiseLevelIndex = (int)br_.read(4);
            u_.noiseTableIndex = (int)br_.read(4);
        }
        return true;
    }

private:
    uint32_t bitsOrZero(int n) { return n ? br_.read(n) : 0; }

    // ---------------------------------------------------------------- word lengths
    bool codedUnitCount(ChannelState& ch) {
        ch.fillMode = (int)br_.read(2);
        if (!ch.fillMode) {
            ch.numCodedVals = u_.numQuantUnits;
        } else {
            ch.numCodedVals = (int)br_.read(5);
            if (ch.numCodedVals > u_.numQuantUnits) return false;
            if (ch.fillMode == 3) ch.splitPoint = (int)br_.read(2) + (ch.chNum << 1) + 1;
        }
        return true;
    }

    bool wordLengths(int c) {
        ChannelState& ch = u_.ch[c];
        const ChannelState& ref = u_.ch[0];
        int* wl = ch.wordLen;
        int weights = 0;
        ch.fillMode = 0;

        switch (br_.read(2)) {
        case 0:  // fixed 3-bit values
            for (int i = 0; i < u_.numQuantUnits; i++) wl[i] = (int)br_.read(3);
            break;
        case 1:
            if (c) {  // VLC delta to the master channel
                if (!codedUnitCount(ch)) return false;
                if (ch.numCodedVals) {
                    const Vlc& vlc = v_.wl[br_.read(2)];
                    for (int i = 0; i < ch.numCodedVals; i++) wl[i] = (ref.wordLen[i] + vlc.decode(br_)) & 7;
                }
            } else {  // some direct values, the rest as minimum + small delta
                weights = (int)br_.read(2);
                if (!codedUnitCount(ch)) return false;
                if (ch.numCodedVals) {
                    const int direct = (int)br_.read(5);
                    if (direct > ch.numCodedVals) return false;
                    const int deltaBits = (int)br_.read(2);
                    const int minVal = (int)br_.read(3);
                    for (int i = 0; i < direct; i++) wl[i] = (int)br_.read(3);
                    for (int i = direct; i < ch.numCodedVals; i++) wl[i] = (minVal + (int)bitsOrZero(deltaBits)) & 7;
                }
            }
            break;
        case 2:
            if (!codedUnitCount(ch)) return false;
            if (c && ch.numCodedVals) {  // VLC delta to the master's contour
                const Vlc& vlc = v_.wl[br_.read(2)];
                wl[0] = (ref.wordLen[0] + vlc.decode(br_)) & 7;
                for (int i = 1; i < ch.numCodedVals; i++) {
                    const int diff = ref.wordLen[i] - ref.wordLen[i - 1];
                    wl[i] = (wl[i - 1] + diff + vlc.decode(br_)) & 7;
                }
            } else if (ch.numCodedVals) {  // shape + VLC corrections
                const int pairwise = br_.read1();
                const Vlc& vlc = v_.wl[br_.read1()];
                const int start = (int)br_.read(3);
                applyShape(start, T::wl_shapes[start][br_.read(4)], wl, ch.numCodedVals);
                if (!pairwise) {
                    for (int i = 0; i < ch.numCodedVals; i++) wl[i] = (wl[i] + vlc.decode(br_)) & 7;
                } else {
                    int i = 0;
                    for (; i < (ch.numCodedVals & ~1); i += 2) {
                        if (!br_.read1()) {
                            wl[i] = (wl[i] + vlc.decode(br_)) & 7;
                            wl[i + 1] = (wl[i + 1] + vlc.decode(br_)) & 7;
                        }
                    }
                    if (ch.numCodedVals & 1) wl[i] = (wl[i] + vlc.decode(br_)) & 7;
                }
            }
            break;
        case 3:  // first value direct, then VLC delta to the previous one
            weights = (int)br_.read(2);
            if (!codedUnitCount(ch)) return false;
            if (ch.numCodedVals) {
                const Vlc& vlc = v_.wl[br_.read(2)];
                wl[0] = (int)br_.read(3);
                for (int i = 1; i < ch.numCodedVals; i++) wl[i] = (wl[i - 1] + vlc.decode(br_)) & 7;
            }
            break;
        }

        if (ch.fillMode == 2) {
            for (int i = ch.numCodedVals; i < u_.numQuantUnits; i++) wl[i] = c ? br_.read1() : 1;
        } else if (ch.fillMode == 3) {
            int end = c ? ch.numCodedVals + ch.splitPoint : u_.numQuantUnits - ch.splitPoint;
            end = std::min(end, kMaxQuantUnits);
            for (int i = ch.numCodedVals; i < end; i++) wl[i] = 1;
        }

        if (weights) {
            const int8_t* w = T::wl_weights[c * 3 + weights - 1];
            for (int i = 0; i < u_.numQuantUnits; i++) {
                wl[i] += w[i];
                if (wl[i] < 0 || wl[i] > 7) return false;
            }
        }
        return true;
    }

    // ---------------------------------------------------------------- scale factors
    bool scaleFactors(int c) {
        ChannelState& ch = u_.ch[c];
        const ChannelState& ref = u_.ch[0];
        int* sf = ch.sfIdx;
        const int n = u_.usedQuantUnits;
        int weights = 0;

        auto shape = [&] {
            const int start = (int)br_.read(6);
            applyShape(start, T::sf_shapes[br_.read(6)], sf, n);
        };

        switch (br_.read(2)) {
        case 0:  // fixed 6-bit values
            for (int i = 0; i < n; i++) sf[i] = (int)br_.read(6);
            break;
        case 1:
            if (c) {  // VLC delta to the master
                const Vlc& vlc = v_.sf[br_.read(2)];
                for (int i = 0; i < n; i++) sf[i] = (ref.sfIdx[i] + vlc.decode(br_)) & 63;
            } else {
                weights = (int)br_.read(2);
                if (weights == 3) {  // shape, then direct corrections and minimum + delta
                    shape();
                    const int numLong = (int)br_.read(5);
                    const int deltaBits = (int)br_.read(2);
                    const int minVal = (int)br_.read(4) - 7;
                    for (int i = 0; i < numLong; i++) sf[i] = (sf[i] + (int)br_.read(4) - 7) & 63;
                    for (int i = numLong; i < n; i++) sf[i] = (sf[i] + minVal + (int)bitsOrZero(deltaBits)) & 63;
                } else {  // direct values, then minimum + delta
                    const int numLong = (int)br_.read(5);
                    const int deltaBits = (int)br_.read(3);
                    const int minVal = (int)br_.read(6);
                    if (numLong > n || deltaBits == 7) return false;
                    for (int i = 0; i < numLong; i++) sf[i] = (int)br_.read(6);
                    for (int i = numLong; i < n; i++) sf[i] = (minVal + (int)bitsOrZero(deltaBits)) & 63;
                }
            }
            break;
        case 2:
            if (c) {  // VLC delta to the master's contour
                const Vlc& vlc = v_.sf[br_.read(2)];
                sf[0] = (ref.sfIdx[0] + vlc.decode(br_)) & 63;
                for (int i = 1; i < n; i++) {
                    const int diff = ref.sfIdx[i] - ref.sfIdx[i - 1];
                    sf[i] = (sf[i - 1] + diff + vlc.decode(br_)) & 63;
                }
            } else {  // shape + signed VLC corrections
                const Vlc& vlc = v_.sf[br_.read(2) + 4];
                shape();
                for (int i = 0; i < n; i++) sf[i] = (sf[i] + signExtend(vlc.decode(br_), 4)) & 63;
            }
            break;
        case 3:
            if (c) {  // copy of the master
                for (int i = 0; i < n; i++) sf[i] = ref.sfIdx[i];
            } else {
                weights = (int)br_.read(2);
                const int sel = (int)br_.read(2);
                if (weights == 3) {  // shape + accumulated signed deltas
                    const Vlc& vlc = v_.sf[sel + 4];
                    shape();
                    int diff = ((int)br_.read(4) + 56) & 63;
                    sf[0] = (sf[0] + diff) & 63;
                    for (int i = 1; i < n; i++) {
                        diff = (diff + signExtend(vlc.decode(br_), 4)) & 63;
                        sf[i] = (diff + sf[i]) & 63;
                    }
                } else {  // first direct, then VLC delta to the previous one
                    const Vlc& vlc = v_.sf[sel];
                    sf[0] = (int)br_.read(6);
                    for (int i = 1; i < n; i++) sf[i] = (sf[i - 1] + vlc.decode(br_)) & 63;
                }
            }
            break;
        }

        if (weights == 1 || weights == 2) {
            const int8_t* w = T::sf_weights[weights - 1];
            for (int i = 0; i < n; i++) {
                sf[i] -= w[i];
                if (sf[i] < 0 || sf[i] > 63) return false;
            }
        }
        return true;
    }

    // ---------------------------------------------------------------- code table selectors
    bool codeTables(int c) {
        ChannelState& ch = u_.ch[c];
        const ChannelState& ref = u_.ch[0];
        const bool full = u_.useFullTable != 0;
        const int mask = full ? 7 : 3;
        ch.tableType = br_.read1();
        const int mode = (int)br_.read(2);
        if (mode == 3 && !c) return true;  // "difference to master" is meaningless for the master

        int count = u_.usedQuantUnits;
        if (br_.read1()) {
            count = (int)br_.read(5);
            if (count > u_.usedQuantUnits) return false;
        }

        const Vlc* vlc = &v_.ct[0];
        const Vlc* deltaVlc = &v_.ct[0];
        if (mode == 1 || mode == 2) vlc = full ? &v_.ct[1] : &v_.ct[0];
        if (mode == 2) deltaVlc = full ? &v_.ct[2] : &v_.ct[0];
        if (mode == 3) vlc = full ? &v_.ct[3] : &v_.ct[0];
        const int directBits = (int)full + 2;

        int pred = 0;
        for (int i = 0; i < count; i++) {
            if (ch.wordLen[i]) {
                int v = 0;
                switch (mode) {
                case 0: v = (int)br_.read(directBits); break;
                case 1: v = vlc->decode(br_); break;
                case 2: v = i == 0 ? vlc->decode(br_) : (pred + deltaVlc->decode(br_)) & mask; break;
                case 3: v = (ref.tabIdx[i] + vlc->decode(br_)) & mask; break;
                }
                ch.tabIdx[i] = v;
                pred = v;
            } else if (c && ref.wordLen[i]) {
                ch.tabIdx[i] = br_.read1();  // "clone master" flag
            }
        }
        return true;
    }

    // ---------------------------------------------------------------- spectrum
    void quSpectrum(const T::SpecCodeTab& tab, const Vlc& vlc, int16_t* out, int numSpecs) {
        const int groupSize = tab.group_size, numCoeffs = tab.num_coeffs, bits = tab.bits;
        const unsigned mask = (1u << bits) - 1;
        for (int pos = 0; pos < numSpecs;) {
            if (groupSize == 1 || br_.read1()) {
                for (int j = 0; j < groupSize; j++) {
                    unsigned val = (unsigned)vlc.decode(br_);
                    for (int i = 0; i < numCoeffs; i++) {
                        int cf = (int)(val & mask);
                        if (tab.is_signed) cf = signExtend(cf, bits);
                        else if (cf && br_.read1()) cf = -cf;
                        out[pos++] = (int16_t)cf;
                        val >>= bits;
                    }
                }
            } else {  // all-zero group
                pos += groupSize * numCoeffs;
            }
        }
    }

    bool spectrum(int c) {
        ChannelState& ch = u_.ch[c];
        const ChannelState& master = u_.ch[0];
        memset(ch.spectrum, 0, sizeof ch.spectrum);
        memset(ch.powerLevs, 15, sizeof ch.powerLevs);  // 15 = power compensation off

        for (int qu = 0; qu < u_.usedQuantUnits; qu++) {
            const int begin = T::qu_to_spec_pos[qu];
            const int numSpecs = T::qu_to_spec_pos[qu + 1] - begin;
            const int wl = ch.wordLen[qu];
            int ct = ch.tabIdx[qu];
            if (wl) {
                if (ct < 0 || ct > 7) return false;
                if (!u_.useFullTable) ct = T::ct_restricted_to_full[ch.tableType][wl - 1][ct & 3];
                int index = (ch.tableType * 8 + ct) * 7 + wl - 1;
                const T::SpecCodeTab& tab = T::spectra_tabs[index];
                if (tab.redirect >= 0) index = tab.redirect;
                if (!v_.spec[index].valid()) return false;
                quSpectrum(tab, v_.spec[index], ch.spectrum + begin, numSpecs);
            } else if (c && master.wordLen[qu] && !ct) {  // copy of the master's coefficients
                memcpy(ch.spectrum + begin, master.spectrum + begin, (size_t)numSpecs * sizeof(int16_t));
                ch.wordLen[qu] = master.wordLen[qu];
            }
        }

        // Power compensation levels (only with more than the lowest two QUs, 0..351 Hz).
        if (u_.usedQuantUnits > 2) {
            const int groups = T::subband_to_num_powgrps[u_.numCodedSubbands - 1];
            for (int i = 0; i < groups; i++) ch.powerLevs[i] = (uint8_t)br_.read(4);
        }
        return true;
    }

    // ---------------------------------------------------------------- gain control
    bool gainPoints(int c, int coded) {
        GainInfo* g = u_.gain(c);
        const GainInfo* ref = u_.gain(0);
        switch (br_.read(2)) {
        case 0:
            for (int i = 0; i < coded; i++) g[i].numPoints = (int)br_.read(3);
            break;
        case 1:
            for (int i = 0; i < coded; i++) g[i].numPoints = v_.gain[0].decode(br_);
            break;
        case 2:
            if (c) {
                for (int i = 0; i < coded; i++) g[i].numPoints = (ref[i].numPoints + v_.gain[1].decode(br_)) & 7;
            } else {
                g[0].numPoints = v_.gain[0].decode(br_);
                for (int i = 1; i < coded; i++) g[i].numPoints = (g[i - 1].numPoints + v_.gain[1].decode(br_)) & 7;
            }
            break;
        case 3:
            if (c) {
                for (int i = 0; i < coded; i++) g[i].numPoints = ref[i].numPoints;
            } else {
                const int deltaBits = (int)br_.read(2);
                const int minVal = (int)br_.read(3);
                for (int i = 0; i < coded; i++) {
                    g[i].numPoints = minVal + (int)bitsOrZero(deltaBits);
                    if (g[i].numPoints > 7) return false;
                }
            }
            break;
        }
        for (int i = 0; i < coded; i++)
            if (g[i].numPoints < 0) return false;  // invalid code
        return true;
    }

    void levelsDeltaChain(GainInfo& g) {  // first via VLC, the rest as VLC delta to the previous
        if (g.numPoints > 0) g.lev[0] = v_.gain[2].decode(br_);
        for (int i = 1; i < g.numPoints; i++) g.lev[i] = (g.lev[i - 1] + v_.gain[3].decode(br_)) & 15;
    }
    static void levelsCopy(GainInfo& g, const GainInfo& ref) {
        for (int i = 0; i < g.numPoints; i++) g.lev[i] = i >= ref.numPoints ? 7 : ref.lev[i];
    }

    bool gainLevels(int c, int coded) {
        GainInfo* g = u_.gain(c);
        const GainInfo* ref = u_.gain(0);
        switch (br_.read(2)) {
        case 0:
            for (int sb = 0; sb < coded; sb++)
                for (int i = 0; i < g[sb].numPoints; i++) g[sb].lev[i] = (int)br_.read(4);
            break;
        case 1:
            if (c) {
                for (int sb = 0; sb < coded; sb++)
                    for (int i = 0; i < g[sb].numPoints; i++) {
                        const int delta = v_.gain[5].decode(br_);
                        const int pred = i >= ref[sb].numPoints ? 7 : ref[sb].lev[i];
                        g[sb].lev[i] = (pred + delta) & 15;
                    }
            } else {
                for (int sb = 0; sb < coded; sb++) levelsDeltaChain(g[sb]);
            }
            break;
        case 2:
            if (c) {
                for (int sb = 0; sb < coded; sb++)
                    if (g[sb].numPoints > 0) {
                        if (br_.read1()) levelsDeltaChain(g[sb]);
                        else levelsCopy(g[sb], ref[sb]);
                    }
            } else {
                if (g[0].numPoints > 0) levelsDeltaChain(g[0]);
                for (int sb = 1; sb < coded; sb++)
                    for (int i = 0; i < g[sb].numPoints; i++) {
                        const int delta = v_.gain[4].decode(br_);
                        const int pred = i >= g[sb - 1].numPoints ? 7 : g[sb - 1].lev[i];
                        g[sb].lev[i] = (pred + delta) & 15;
                    }
            }
            break;
        case 3:
            if (c) {
                for (int sb = 0; sb < coded; sb++) levelsCopy(g[sb], ref[sb]);
            } else {
                const int deltaBits = (int)br_.read(2);
                const int minVal = (int)br_.read(4);
                for (int sb = 0; sb < coded; sb++)
                    for (int i = 0; i < g[sb].numPoints; i++) {
                        g[sb].lev[i] = minVal + (int)bitsOrZero(deltaBits);
                        if (g[sb].lev[i] > 15) return false;
                    }
            }
            break;
        }
        return true;
    }

    void locDirect(GainInfo& g, int i) {  // ascending sequence, coded with as few bits as possible
        if (!i || g.loc[i - 1] < 15) {
            g.loc[i] = (int)br_.read(5);
        } else if (g.loc[i - 1] >= 30) {
            g.loc[i] = 31;
        } else {
            const int bits = ilog2((unsigned)(30 - g.loc[i - 1])) + 1;
            g.loc[i] = g.loc[i - 1] + (int)br_.read(bits) + 1;
        }
    }
    void locDeltaChain(GainInfo& g) {  // first direct, the rest VLC delta by curve direction
        if (g.numPoints <= 0) return;
        g.loc[0] = (int)br_.read(5);
        for (int i = 1; i < g.numPoints; i++) {
            const Vlc& vlc = g.lev[i] <= g.lev[i - 1] ? v_.gain[7] : v_.gain[9];
            g.loc[i] = g.loc[i - 1] + vlc.decode(br_);
        }
    }

    bool gainLocations(int c, int coded) {
        GainInfo* g = u_.gain(c);
        const GainInfo* ref = u_.gain(0);
        switch (br_.read(2)) {
        case 0:
            for (int sb = 0; sb < coded; sb++)
                for (int i = 0; i < g[sb].numPoints; i++) locDirect(g[sb], i);
            break;
        case 1:
            if (c) {
                for (int sb = 0; sb < coded; sb++) {
                    GainInfo& d = g[sb];
                    const GainInfo& r = ref[sb];
                    if (d.numPoints <= 0) continue;
                    const int pred = r.numPoints > 0 ? r.loc[0] : 0;
                    d.loc[0] = (pred + v_.gain[10].decode(br_)) & 31;
                    for (int i = 1; i < d.numPoints; i++) {
                        const bool beyondRef = i >= r.numPoints;
                        if (d.lev[i] > d.lev[i - 1]) {  // ascending
                            if (beyondRef) {
                                d.loc[i] = d.loc[i - 1] + v_.gain[9].decode(br_);
                            } else if (br_.read1()) {
                                locDirect(d, i);
                            } else {
                                d.loc[i] = r.loc[i];
                            }
                        } else {  // descending
                            const int delta = (beyondRef ? v_.gain[7] : v_.gain[10]).decode(br_);
                            d.loc[i] = beyondRef ? d.loc[i - 1] + delta : (r.loc[i] + delta) & 31;
                        }
                    }
                }
            } else {
                for (int sb = 0; sb < coded; sb++) locDeltaChain(g[sb]);
            }
            break;
        case 2:
            if (c) {
                for (int sb = 0; sb < coded; sb++) {
                    GainInfo& d = g[sb];
                    if (d.numPoints <= 0) continue;
                    if (d.numPoints > ref[sb].numPoints || br_.read1()) {
                        locDeltaChain(d);
                    } else {
                        for (int i = 0; i < d.numPoints; i++) d.loc[i] = ref[sb].loc[i];
                    }
                }
            } else {
                for (int i = 0; i < g[0].numPoints; i++) locDirect(g[0], i);
                for (int sb = 1; sb < coded; sb++) {
                    GainInfo& d = g[sb];
                    const GainInfo& p = g[sb - 1];
                    if (d.numPoints <= 0) continue;
                    const int pred = p.numPoints > 0 ? p.loc[0] : 0;
                    d.loc[0] = (pred + v_.gain[6].decode(br_)) & 31;
                    for (int i = 1; i < d.numPoints; i++) {
                        const bool beyondPrev = i >= p.numPoints;
                        const Vlc& vlc = v_.gain[(d.lev[i] > d.lev[i - 1]) * 2 + beyondPrev + 6];
                        const int delta = vlc.decode(br_);
                        d.loc[i] = beyondPrev ? d.loc[i - 1] + delta : (p.loc[i] + delta) & 31;
                    }
                }
            }
            break;
        case 3:
            if (c) {
                for (int sb = 0; sb < coded; sb++)
                    for (int i = 0; i < g[sb].numPoints; i++) {
                        if (i >= ref[sb].numPoints) locDirect(g[sb], i);
                        else g[sb].loc[i] = ref[sb].loc[i];
                    }
            } else {
                const int deltaBits = (int)br_.read(2) + 1;
                const int minVal = (int)br_.read(5);
                for (int sb = 0; sb < coded; sb++)
                    for (int i = 0; i < g[sb].numPoints; i++) g[sb].loc[i] = minVal + i + (int)br_.read(deltaBits);
            }
            break;
        }
        for (int sb = 0; sb < coded; sb++)
            for (int i = 0; i < g[sb].numPoints; i++)
                if (g[sb].loc[i] < 0 || g[sb].loc[i] > 31 || (i && g[sb].loc[i] <= g[sb].loc[i - 1])) return false;
        return true;
    }

    bool gainControl(int c) {
        ChannelState& ch = u_.ch[c];
        GainInfo* g = u_.gain(c);
        for (int sb = 0; sb < kSubbands; sb++) g[sb] = GainInfo{};
        if (!br_.read1()) {
            ch.numGainSubbands = 0;
            return true;
        }
        const int coded = (int)br_.read(4) + 1;
        ch.numGainSubbands = br_.read1() ? (int)br_.read(4) + 1 : coded;  // high-band replication
        if (!gainPoints(c, coded) || !gainLevels(c, coded) || !gainLocations(c, coded)) return false;
        for (int sb = coded; sb < ch.numGainSubbands; sb++) g[sb] = g[sb - 1];
        return true;
    }

    // ---------------------------------------------------------------- tones (GHA sine waves)
    void toneEnvelopes(int c, const int* has) {
        WaveGroup* dst = u_.tones(c);
        const WaveGroup* ref = u_.tones(0);
        const int bands = u_.wavesCur().numToneBands;
        if (!c || !br_.read1()) {
            for (int sb = 0; sb < bands; sb++) {
                if (!has[sb]) continue;
                WaveEnvelope& e = dst[sb].pending;
                e.hasStart = br_.read1();
                e.startPos = e.hasStart ? (int)br_.read(5) : -1;
                e.hasStop = br_.read1();
                e.stopPos = e.hasStop ? (int)br_.read(5) : 32;
            }
        } else {
            for (int sb = 0; sb < bands; sb++)
                if (has[sb]) dst[sb].pending = ref[sb].pending;
        }
    }

    bool toneCounts(int c, const int* has) {
        WaveSynth& ws = u_.wavesCur();
        WaveGroup* dst = u_.tones(c);
        const WaveGroup* ref = u_.tones(0);
        const int bands = ws.numToneBands;
        switch (br_.read(c + 1)) {
        case 0:
            for (int sb = 0; sb < bands; sb++)
                if (has[sb]) dst[sb].numWaves = (int)br_.read(4);
            break;
        case 1:
            for (int sb = 0; sb < bands; sb++)
                if (has[sb]) dst[sb].numWaves = v_.tone[1].decode(br_);
            break;
        case 2:
            for (int sb = 0; sb < bands; sb++)
                if (has[sb]) dst[sb].numWaves = (ref[sb].numWaves + signExtend(v_.tone[2].decode(br_), 3)) & 15;
            break;
        case 3:
            for (int sb = 0; sb < bands; sb++)
                if (has[sb]) dst[sb].numWaves = ref[sb].numWaves;
            break;
        }
        for (int sb = 0; sb < bands; sb++) {
            if (!has[sb]) continue;
            if (dst[sb].numWaves < 0 || ws.tonesIndex + dst[sb].numWaves > kMaxTones) return false;
            dst[sb].startIndex = ws.tonesIndex;
            ws.tonesIndex += dst[sb].numWaves;
        }
        return true;
    }

    void toneFrequencies(int c, const int* has) {
        WaveSynth& ws = u_.wavesCur();
        WaveGroup* dst = u_.tones(c);
        const WaveGroup* ref = u_.tones(0);
        const int bands = ws.numToneBands;
        if (!c || !br_.read1()) {
            for (int sb = 0; sb < bands; sb++) {
                const int n = dst[sb].numWaves;
                if (!has[sb] || !n) continue;
                WaveParam* w = ws.waves + dst[sb].startIndex;
                const int descending = n > 1 ? br_.read1() : 0;
                if (descending) {
                    w[n - 1].freqIndex = (int)br_.read(10);
                    for (int i = n - 2; i >= 0; i--) w[i].freqIndex = (int)br_.read(ilog2((unsigned)w[i + 1].freqIndex) + 1);
                } else {
                    for (int i = 0; i < n; i++) {
                        if (!i || w[i - 1].freqIndex < 512) {
                            w[i].freqIndex = (int)br_.read(10);
                        } else {
                            const int bits = ilog2((unsigned)(1023 - w[i - 1].freqIndex)) + 1;
                            w[i].freqIndex = (int)br_.read(bits) + 1024 - (1 << bits);
                        }
                    }
                }
            }
        } else {  // VLC delta to the master's frequencies
            for (int sb = 0; sb < bands; sb++) {
                const int n = dst[sb].numWaves;
                if (!has[sb] || !n) continue;
                const WaveParam* in = ws.waves + ref[sb].startIndex;
                WaveParam* out = ws.waves + dst[sb].startIndex;
                const int refN = ref[sb].numWaves;
                for (int i = 0; i < n; i++) {
                    const int delta = signExtend(v_.tone[6].decode(br_), 8);
                    const int pred = i < refN ? in[i].freqIndex : (refN ? in[refN - 1].freqIndex : 0);
                    out[i].freqIndex = (pred + delta) & 1023;
                }
            }
        }
    }

    void toneAmplitudes(int c, const int* has) {
        WaveSynth& ws = u_.wavesCur();
        WaveGroup* dst = u_.tones(c);
        const WaveGroup* ref = u_.tones(0);
        const int bands = ws.numToneBands;
        int refWave[kMaxTones] = {};  // master wave closest in frequency, -1 = none

        if (c) {
            for (int sb = 0; sb < bands; sb++) {
                if (!has[sb] || !dst[sb].numWaves) continue;
                const WaveParam* ws_ = ws.waves + dst[sb].startIndex;
                const WaveParam* wr = ws.waves + ref[sb].startIndex;
                for (int j = 0; j < dst[sb].numWaves; j++) {
                    int best = 0, bestDiff = 1024;
                    for (int i = 0; i < ref[sb].numWaves; i++) {
                        const int diff = std::abs(ws_[j].freqIndex - wr[i].freqIndex);
                        if (diff < bestDiff) {
                            bestDiff = diff;
                            best = i;
                        }
                    }
                    int& r = refWave[dst[sb].startIndex + j];
                    if (bestDiff < 8) r = best + ref[sb].startIndex;
                    else if (j < ref[sb].numWaves) r = j + ref[sb].startIndex;
                    else r = -1;
                }
            }
        }

        switch (br_.read(c + 1)) {
        case 0:
            for (int sb = 0; sb < bands; sb++) {
                if (!has[sb] || !dst[sb].numWaves) continue;
                WaveParam* w = ws.waves + dst[sb].startIndex;
                if (ws.amplitudeMode) {
                    for (int i = 0; i < dst[sb].numWaves; i++) w[i].ampSf = (int)br_.read(6);
                } else {
                    w[0].ampSf = (int)br_.read(6);
                }
            }
            break;
        case 1:
            for (int sb = 0; sb < bands; sb++) {
                if (!has[sb] || !dst[sb].numWaves) continue;
                WaveParam* w = ws.waves + dst[sb].startIndex;
                if (ws.amplitudeMode) {
                    for (int i = 0; i < dst[sb].numWaves; i++) w[i].ampSf = v_.tone[3].decode(br_) + 20;
                } else {
                    w[0].ampSf = v_.tone[4].decode(br_) + 24;
                }
            }
            break;
        case 2:
            for (int sb = 0; sb < bands; sb++) {
                if (!has[sb] || !dst[sb].numWaves) continue;
                for (int i = 0; i < dst[sb].numWaves; i++) {
                    const int k = dst[sb].startIndex + i;
                    const int delta = signExtend(v_.tone[5].decode(br_), 5);
                    const int pred = refWave[k] >= 0 ? ws.waves[refWave[k]].ampSf : 34;
                    ws.waves[k].ampSf = (pred + delta) & 63;
                }
            }
            break;
        case 3:
            for (int sb = 0; sb < bands; sb++) {
                if (!has[sb]) continue;
                for (int i = 0; i < dst[sb].numWaves; i++) {
                    const int k = dst[sb].startIndex + i;
                    ws.waves[k].ampSf = refWave[k] >= 0 ? ws.waves[refWave[k]].ampSf : 32;
                }
            }
            break;
        }
    }

    void tonePhases(int c, const int* has) {
        WaveSynth& ws = u_.wavesCur();
        const WaveGroup* dst = u_.tones(c);
        for (int sb = 0; sb < ws.numToneBands; sb++) {
            if (!has[sb]) continue;
            for (int i = 0; i < dst[sb].numWaves; i++) ws.waves[dst[sb].startIndex + i].phaseIndex = (int)br_.read(5);
        }
    }

    bool tones() {
        for (int c = 0; c < nch_; c++)
            for (int sb = 0; sb < kSubbands; sb++) u_.tones(c)[sb] = WaveGroup{};

        WaveSynth& ws = u_.wavesCur();
        ws.tonesPresent = br_.read1();
        if (!ws.tonesPresent) return true;
        for (WaveParam& w : ws.waves) w = WaveParam{};

        ws.amplitudeMode = br_.read1();
        if (!ws.amplitudeMode) return false;  // "GHA amplitude mode 0" is not known to be used
        ws.numToneBands = v_.tone[0].decode(br_) + 1;
        if (ws.numToneBands < 1) return false;

        if (nch_ == 2) {
            readSubbandFlags(br_, ws.toneSharing, ws.numToneBands);
            readSubbandFlags(br_, ws.toneMaster, ws.numToneBands);
            readSubbandFlags(br_, ws.invertPhase, ws.numToneBands);
        }
        ws.tonesIndex = 0;

        for (int c = 0; c < nch_; c++) {
            int has[kSubbands] = {};
            for (int i = 0; i < ws.numToneBands; i++) has[i] = !c ? 1 : !ws.toneSharing[i];
            toneEnvelopes(c, has);
            if (!toneCounts(c, has)) return false;
            toneFrequencies(c, has);
            toneAmplitudes(c, has);
            tonePhases(c, has);
        }

        if (nch_ == 2) {
            for (int i = 0; i < ws.numToneBands; i++) {
                if (ws.toneSharing[i]) u_.tones(1)[i] = u_.tones(0)[i];
                if (ws.toneMaster[i]) std::swap(u_.tones(0)[i], u_.tones(1)[i]);
            }
        }
        return true;
    }

    BitReader& br_;
    ChannelUnit& u_;
    const int nch_;
    const VlcSet& v_;
};

}  // namespace

bool parseChannelUnit(BitReader& br, ChannelUnit& u, int numChannels) {
    return UnitParser(br, u, numChannels).parse();
}

}  // namespace vc2rt::media::at3p
