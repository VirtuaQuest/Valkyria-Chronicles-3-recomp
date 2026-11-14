#pragma once
// ATRAC3plus constant tables (VLC code books, quantisation / scale-factor tables, window, noise
// and filter-bank coefficients). See atrac3plus_tables.cpp for provenance and licence:
// derived from FFmpeg's ATRAC3+ decoder, LGPL-2.1-or-later.
#include <cstdint>

namespace vc2rt::media::at3p::tables {

// Spectrum code table descriptor (one per table set x code table x word length).
struct SpecCodeTab {
    uint8_t group_size;   // number of coefficients grouped together (1 = no grouping)
    uint8_t num_coeffs;   // coefficients packed into one VLC symbol
    uint8_t bits;         // bits per packed coefficient
    uint8_t is_signed;    // 1: coefficients carry their own sign; 0: magnitude + sign bit
    int redirect;         // >= 0: reuse the code book of that entry
    const uint8_t* cb;    // canonical code book descriptor: min_len, max_len, count per length
    const uint8_t* xlat;  // symbol translation table (nullptr: symbol = code index)
};

// Explicitly coded (non-canonical) VLC: per-symbol code lengths and code words.
struct ExplicitVlc {
    int num_codes;
    const uint8_t* bits;
    const uint8_t* codes8;    // one of codes8 / codes16 is set
    const uint16_t* codes16;
    const uint8_t* xlat;      // nullptr: symbol = index
};

// Canonical VLC: code book descriptor + symbol translation.
struct CanonicalVlc {
    const uint8_t* cb;
    const uint8_t* xlat;
};


// ---- bitstream parameter tables (atrac3plus_data.h)
extern const int8_t wl_weights[6][32];
extern const int8_t sf_weights[2][32];
extern const uint8_t qu_num_to_seg[32];
extern const uint8_t qu_to_subband[32];
extern const int subband_to_num_powgrps[16];
extern const int8_t wl_shapes[8][16][9];
extern const int8_t sf_shapes[64][9];
extern const uint8_t ct_restricted_to_full[2][7][4];
extern const SpecCodeTab spectra_tabs[112];

// ---- DSP tables (atrac3plusdsp.c)
extern const uint16_t qu_to_spec_pos[33];
extern const float sf_tab[64];
extern const float mant_tab[8];
extern const int subband_to_powgrp[16];
extern const float noise_tab[1024];
extern const float pwc_levs[16];
extern const int subband_to_qu[17];
extern const float ipqf_coeffs1[12][16];
extern const float ipqf_coeffs2[12][16];

// ---- VLC set descriptors
extern const ExplicitVlc wl_vlcs[4];   // word length deltas
extern const ExplicitVlc ct_vlcs[4];   // code table indexes
extern const ExplicitVlc sf_vlcs[8];   // scale factor deltas
extern const CanonicalVlc gain_vlcs[11];
extern const CanonicalVlc tone_vlcs[7];

}  // namespace vc2rt::media::at3p::tables
