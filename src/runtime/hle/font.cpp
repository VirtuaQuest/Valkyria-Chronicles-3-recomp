// sceLibFont HLE (replaces the game's LIBFONT.PRX).
//
// The PSP renders text from PGF fonts in its firmware (flash0:/font). Those files are not ours to
// ship, so the 18 firmware fonts are presented with their real styles/sizes, but glyphs are
// rasterized with a similar font installed on Windows (Arial / Times New Roman / Yu Gothic /
// Malgun Gothic) at the size the PSP font would have (point size x library resolution / 72).
// Metrics are reported in the PSP's 26.6 fixed point formats; glyph images are blended into the
// guest's GlyphImage buffer exactly like the firmware library does (additive, saturating, with
// horizontal sub-pixel positioning). Memory is obtained through the game's alloc/free callbacks at
// the same points the real library calls them.
#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstring>
#include <map>

#include "../kernel/kernel.h"
#include "font_render.h"
#include "hle.h"

namespace vc2rt::utility {
void runFontSelfTest(PspContext* c, u32 fontHandle);
}

namespace vc2rt::font {

namespace {

enum : u32 {
    ERR_OUT_OF_MEMORY = 0x80460001,
    ERR_INVALID_LIBID = 0x80460002,
    ERR_INVALID_PARAMETER = 0x80460003,
    ERR_TOO_MANY_OPEN_FONTS = 0x80460009,
    ERR_KERNEL_ILLEGAL_ADDR = 0x800200d3,
    ERR_KERNEL_INVALID_ARGUMENT = 0x800200d2,
};

enum : u16 { FAMILY_SANS_SERIF = 1, FAMILY_SERIF = 2 };
enum : u16 { STYLE_REGULAR = 1, STYLE_ITALIC = 2, STYLE_BOLD = 5, STYLE_BOLD_ITALIC = 6, STYLE_DB = 103 };
enum : u16 { LANG_JAPANESE = 1, LANG_LATIN = 2, LANG_KOREAN = 3 };
enum : u32 { PIXFMT_4 = 0, PIXFMT_4_REV = 1, PIXFMT_8 = 2, PIXFMT_24 = 3, PIXFMT_32 = 4 };

// The firmware font set (flash0:/font), in the order the library enumerates it.
struct InternalFont {
    const char* file;
    const char* name;
    u16 family, style, language, country;
    u32 size64;      // point size in 26.6 (h and v)
    u32 fileSize;    // used for FULL opens
    u32 stingySize;  // memory the library asks for on a STINGY open
};
const InternalFont kFonts[] = {
    {"jpn0.pgf", "FTT-NewRodin Pro DB", FAMILY_SANS_SERIF, STYLE_DB, LANG_JAPANESE, 1, 0x288, 1581700, 145844},
    {"ltn0.pgf", "FTT-NewRodin Pro Latin", FAMILY_SANS_SERIF, STYLE_REGULAR, LANG_LATIN, 1, 0x288, 69108, 16680},
    {"ltn1.pgf", "FTT-Matisse Pro Latin", FAMILY_SERIF, STYLE_REGULAR, LANG_LATIN, 1, 0x288, 65124, 16920},
    {"ltn2.pgf", "FTT-NewRodin Pro Latin", FAMILY_SANS_SERIF, STYLE_ITALIC, LANG_LATIN, 1, 0x288, 72948, 16872},
    {"ltn3.pgf", "FTT-Matisse Pro Latin", FAMILY_SERIF, STYLE_ITALIC, LANG_LATIN, 1, 0x288, 67700, 17112},
    {"ltn4.pgf", "FTT-NewRodin Pro Latin", FAMILY_SANS_SERIF, STYLE_BOLD, LANG_LATIN, 1, 0x288, 72828, 16648},
    {"ltn5.pgf", "FTT-Matisse Pro Latin", FAMILY_SERIF, STYLE_BOLD, LANG_LATIN, 1, 0x288, 68220, 16928},
    {"ltn6.pgf", "FTT-NewRodin Pro Latin", FAMILY_SANS_SERIF, STYLE_BOLD_ITALIC, LANG_LATIN, 1, 0x288, 77032, 16792},
    {"ltn7.pgf", "FTT-Matisse Pro Latin", FAMILY_SERIF, STYLE_BOLD_ITALIC, LANG_LATIN, 1, 0x288, 71144, 17160},
    {"ltn8.pgf", "FTT-NewRodin Pro Latin", FAMILY_SANS_SERIF, STYLE_REGULAR, LANG_LATIN, 1, 0x1c0, 41000, 16192},
    {"ltn9.pgf", "FTT-Matisse Pro Latin", FAMILY_SERIF, STYLE_REGULAR, LANG_LATIN, 1, 0x1c0, 40164, 16476},
    {"ltn10.pgf", "FTT-NewRodin Pro Latin", FAMILY_SANS_SERIF, STYLE_ITALIC, LANG_LATIN, 1, 0x1c0, 42692, 16300},
    {"ltn11.pgf", "FTT-Matisse Pro Latin", FAMILY_SERIF, STYLE_ITALIC, LANG_LATIN, 1, 0x1c0, 41488, 16656},
    {"ltn12.pgf", "FTT-NewRodin Pro Latin", FAMILY_SANS_SERIF, STYLE_BOLD, LANG_LATIN, 1, 0x1c0, 43136, 16176},
    {"ltn13.pgf", "FTT-Matisse Pro Latin", FAMILY_SERIF, STYLE_BOLD, LANG_LATIN, 1, 0x1c0, 41772, 16436},
    {"ltn14.pgf", "FTT-NewRodin Pro Latin", FAMILY_SANS_SERIF, STYLE_BOLD_ITALIC, LANG_LATIN, 1, 0x1c0, 45184, 16272},
    {"ltn15.pgf", "FTT-Matisse Pro Latin", FAMILY_SERIF, STYLE_BOLD_ITALIC, LANG_LATIN, 1, 0x1c0, 43044, 16704},
    {"kr0.pgf", "AsiaNHH(512Johab)", FAMILY_SANS_SERIF, STYLE_REGULAR, LANG_KOREAN, 3, 0x288, 394192, 51856},
};
constexpr u32 kNumFonts = (u32)std::size(kFonts);
constexpr u32 kStyleSize = 0xA8;     // PGFFontStyle
constexpr u32 kFontInfoSize = 0x108; // PGFFontInfo
constexpr u32 kCharInfoSize = 0x3C;  // PGFCharInfo
constexpr int kMaxRefs = 4;

std::vector<std::wstring> hostFacesFor(const InternalFont& f) {
    if (f.language == LANG_JAPANESE) return {L"Yu Gothic UI", L"Meiryo UI", L"MS UI Gothic", L"MS Gothic", L"Arial"};
    if (f.language == LANG_KOREAN) return {L"Malgun Gothic", L"Gulim", L"Arial"};
    if (f.family == FAMILY_SERIF) return {L"Times New Roman", L"Georgia", L"Cambria"};
    return {L"Arial", L"Segoe UI", L"Tahoma"};
}

// PGFFontStyle (168 bytes)
void writeStyle(u32 p, const InternalFont& f) {
    memset(memPtr(p), 0, kStyleSize);
    float sz = f.size64 / 64.0f, res = 128.0f;  // firmware fonts are 128 dpi
    memcpy(memPtr(p + 0), &sz, 4);
    memcpy(memPtr(p + 4), &sz, 4);
    memcpy(memPtr(p + 8), &res, 4);
    memcpy(memPtr(p + 12), &res, 4);
    // fontWeight (float) @16 stays 0
    wr16(p + 20, f.family);
    wr16(p + 22, f.style);
    wr16(p + 24, 0);  // styleSub
    wr16(p + 26, f.language);
    wr16(p + 28, 0);  // region
    wr16(p + 30, f.country);
    writeCString(p + 32, f.name, 64);
    writeCString(p + 96, f.file, 64);
    // attributes @160, expire @164 = 0
}

float rdf(u32 a) {
    float f;
    memcpy(&f, memPtr(a), 4);
    return f;
}
void wrf(u32 a, float f) { memcpy(memPtr(a), &f, 4); }

// ------------------------------------------------------------------ library state
struct Slot {
    u32 handle = 0;     // guest address of the font's slot in the library block
    int fontIndex = -1; // internal font, -1 = never opened
    int refs = 0;
    u32 allocAddr = 0;  // memory from the game's allocator for the open font
};

struct FontLib {
    u32 handle = 0;  // guest address of the library block (returned by sceFontNewLib)
    u32 userData = 0, allocFunc = 0, freeFunc = 0;
    u32 numFonts = 0;
    float hRes = 128.0f, vRes = 128.0f;
    u16 altChar = 0x5F;
    std::vector<Slot> slots;
    u32 charBitmap = 0;  // the library's glyph work buffer (re-allocated per GetCharInfo)
};

std::map<u32, FontLib> g_libs;           // by library handle
std::map<u32, std::pair<u32, int>> g_fonts;  // font handle -> (library handle, slot)

// Rasterizers shared by every library: keyed by (internal font, pixel size)
std::map<std::pair<int, int>, std::unique_ptr<HostFace>> g_faces;

HostFace* faceFor(int fontIndex, float hRes) {
    const InternalFont& f = kFonts[fontIndex];
    int px = std::max(1, (int)lround(f.size64 / 64.0 * hRes / 72.0));
    auto key = std::make_pair(fontIndex, px);
    auto it = g_faces.find(key);
    if (it != g_faces.end()) return it->second.get();
    auto face = std::make_unique<HostFace>();
    bool bold = f.style == STYLE_BOLD || f.style == STYLE_BOLD_ITALIC;
    bool italic = f.style == STYLE_ITALIC || f.style == STYLE_BOLD_ITALIC;
    int weight = bold ? 700 : (f.style == STYLE_DB ? 600 : 400);
    if (!face->open(hostFacesFor(f), px, weight, italic)) {
        LOG_ERROR("FONT", "no host font for %s", f.file);
        g_faces[key] = nullptr;
        return nullptr;
    }
    std::string fn;
    for (wchar_t ch : face->faceName()) fn.push_back(ch < 0x80 ? (char)ch : '?');
    LOG_INFO("FONT", "%s (%s %.3fpt) -> host font \"%s\" %dpx (ascent %d, descent %d, max glyph %dx%d)", f.file, f.name,
             f.size64 / 64.0, fn.c_str(), px, face->metrics().ascent, face->metrics().descent, face->metrics().maxW,
             face->metrics().maxH);
    HostFace* p = face.get();
    g_faces[key] = std::move(face);
    return p;
}

// Guest allocator callbacks, called the way the firmware library calls them: alloc(userData, size)
// returns an address, free(userData, ptr).
u32 guestAlloc(PspContext* c, const FontLib& lib, u32 size) {
    if (!lib.allocFunc) return 0;
    return callGuestSimple(lib.allocFunc, (c->r[29] - 0x400) & ~0xFu, c->r[28], {lib.userData, size});
}
void guestFree(PspContext* c, const FontLib& lib, u32 ptr) {
    if (!lib.freeFunc || !ptr) return;
    callGuestSimple(lib.freeFunc, (c->r[29] - 0x400) & ~0xFu, c->r[28], {lib.userData, ptr});
}

FontLib* findLib(u32 h) {
    auto it = g_libs.find(h);
    return it == g_libs.end() ? nullptr : &it->second;
}
struct OpenFont {
    FontLib* lib;
    Slot* slot;
};
bool findFont(u32 h, OpenFont& out, bool allowClosed) {
    auto it = g_fonts.find(h);
    if (it == g_fonts.end()) return false;
    FontLib* lib = findLib(it->second.first);
    if (!lib || it->second.second >= (int)lib->slots.size()) return false;
    Slot& s = lib->slots[it->second.second];
    if (s.fontIndex < 0 || (!allowClosed && s.refs == 0)) return false;
    out = {lib, &s};
    return true;
}
bool validErr(u32 p) { return p && memValid(p) && memValid(p + 3); }
void setErr(u32 p, u32 v) {
    if (validErr(p)) wr32(p, v);
}

// ------------------------------------------------------------------ library
// FontNewLibParams: userData, numFonts, cacheData, alloc, free, open, close, read, seek, error, ioFinish
void sceFontNewLib(PspContext* c) {
    u32 params = A0, errPtr = A1;
    if (!memValid(params) || !memValid(params + 43) || !validErr(errPtr)) {
        RET(ERR_KERNEL_ILLEGAL_ADDR);
        return;
    }
    FontLib lib;
    lib.userData = rd32(params);
    lib.numFonts = std::min<u32>(rd32(params + 4), 9);
    lib.allocFunc = rd32(params + 12);
    lib.freeFunc = rd32(params + 16);
    if (!memValid(lib.allocFunc) || !memValid(lib.freeFunc)) {
        wr32(errPtr, ERR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    // library block, per-font blocks and the internal font table, as one allocation
    u32 n = lib.numFonts;
    u32 size = 0x4C + n * 0x4C + n * 0x230 + kNumFonts * kStyleSize;
    u32 addr = guestAlloc(c, lib, size);
    if (!addr || !memValid(addr) || !memValid(addr + size - 1)) {
        wr32(errPtr, ERR_OUT_OF_MEMORY);
        RET(0);
        return;
    }
    memset(memPtr(addr), 0, size);
    memcpy(memPtr(addr), memPtr(params), 44);
    wr32(addr + 0x04, n);
    wr32(addr + 0x2C, addr + 0x4C);              // per-font info
    wr32(addr + 0x30, addr + 0x4C + n * 0x4C);   // per-font work areas
    wrf(addr + 0x38, lib.hRes);
    wrf(addr + 0x3C, lib.vRes);
    wr32(addr + 0x40, kNumFonts);
    u32 table = addr + 0x4C + n * 0x4C + n * 0x230;
    wr32(addr + 0x44, table);
    wr16(addr + 0x48, lib.altChar);
    for (u32 i = 0; i < kNumFonts; i++) writeStyle(table + i * kStyleSize, kFonts[i]);
    lib.handle = addr;
    lib.slots.resize(n);
    for (u32 i = 0; i < n; i++) lib.slots[i].handle = addr + 0x4C + i * 0x4C;
    g_libs[addr] = lib;
    wr32(errPtr, 0);
    LOG_INFO("FONT", "sceFontNewLib: %u font slots, library block %08x (%u bytes)", n, addr, size);
    RET(addr);
}

void closeSlot(PspContext* c, FontLib& lib, Slot& s, bool all) {
    if (s.refs <= 0) return;
    s.refs = all ? 0 : s.refs - 1;
    if (s.refs == 0) {
        guestFree(c, lib, s.allocAddr);
        s.allocAddr = 0;
    }
    if (lib.charBitmap) {
        guestFree(c, lib, lib.charBitmap);
        lib.charBitmap = 0;
    }
}

void sceFontDoneLib(PspContext* c) {
    FontLib* lib = findLib(A0);
    if (!lib) {
        RET(0);
        return;
    }
    for (auto& s : lib->slots) closeSlot(c, *lib, s, true);
    for (auto it = g_fonts.begin(); it != g_fonts.end();)
        it = it->second.first == lib->handle ? g_fonts.erase(it) : std::next(it);
    guestFree(c, *lib, lib->handle);
    g_libs.erase(A0);
    LOG_INFO("FONT", "sceFontDoneLib(%08x)", A0);
    RET(0);
}

void sceFontGetNumFontList(PspContext* c) {
    if (!validErr(A1)) {
        RET(ERR_INVALID_PARAMETER);
        return;
    }
    if (!findLib(A0)) {
        wr32(A1, ERR_INVALID_LIBID);
        RET(0);
        return;
    }
    wr32(A1, 0);
    RET(kNumFonts);
}

void sceFontGetFontList(PspContext* c) {
    if (!findLib(A0)) {
        RET(ERR_INVALID_LIBID);
        return;
    }
    s32 num = std::min<s32>((s32)A2, (s32)kNumFonts);
    if (!memValid(A1) || (num > 0 && !memValid(A1 + num * kStyleSize - 1))) {
        RET(ERR_INVALID_PARAMETER);
        return;
    }
    for (s32 i = 0; i < num; i++) writeStyle(A1 + i * kStyleSize, kFonts[i]);
    RET(0);
}

// Style matching: every non-zero field of the request must match; among matches the nearest size
// (in pixels, i.e. points x resolution) wins.
struct StyleReq {
    float h, v, hRes, vRes;
    u16 family, style, language, country;
    std::string name, file;
};
StyleReq readStyle(u32 p) {
    StyleReq r;
    r.h = rdf(p);
    r.v = rdf(p + 4);
    r.hRes = rdf(p + 8);
    r.vRes = rdf(p + 12);
    r.family = rd16(p + 20);
    r.style = rd16(p + 22);
    r.language = rd16(p + 26);
    r.country = rd16(p + 30);
    r.name = readCString(p + 32, 64);
    r.file = readCString(p + 96, 64);
    return r;
}
bool styleMatches(const StyleReq& r, const InternalFont& f) {
    if (r.family && r.family != f.family) return false;
    if (r.style && r.style != f.style) return false;
    if (r.language && r.language != f.language) return false;
    if (r.country && r.country != f.country) return false;
    if (!r.name.empty() && r.name != f.name) return false;
    if (!r.file.empty() && r.file != f.file) return false;
    return true;
}

void sceFontFindOptimumFont(PspContext* c) {
    if (!validErr(A2)) {
        RET(ERR_KERNEL_INVALID_ARGUMENT);
        return;
    }
    FontLib* lib = findLib(A0);
    if (!lib) {
        wr32(A2, ERR_INVALID_LIBID);
        RET(0);
        return;
    }
    if (!memValid(A1) || !memValid(A1 + kStyleSize - 1)) {
        wr32(A2, ERR_INVALID_LIBID);  // sic: the firmware reports this code for a bad style pointer
        RET(0);
        return;
    }
    StyleReq r = readStyle(A1);
    float hRes = r.hRes > 0 ? r.hRes : lib->hRes, vRes = r.vRes > 0 ? r.vRes : lib->vRes;
    int best = -1, lastMatch = -1;
    float bestDist = INFINITY;
    for (u32 i = 0; i < kNumFonts; i++) {
        if (!styleMatches(r, kFonts[i])) continue;
        lastMatch = (int)i;
        float fontPx = kFonts[i].size64 / 64.0f * 128.0f;
        float dist = INFINITY;
        if (r.h > 0) dist = fabsf(fontPx - hRes * r.h);
        else if (r.v > 0) dist = fabsf(fontPx - vRes * r.v);
        if (dist < bestDist) {
            bestDist = dist;
            best = (int)i;
        }
    }
    if (best < 0) best = lastMatch < 0 ? 0 : lastMatch;
    wr32(A2, 0);
    LOG_INFO("FONT", "sceFontFindOptimumFont(%.3fpt @%.0fdpi family %u style %u lang %u \"%s\") -> %d (%s)", r.h, hRes,
             r.family, r.style, r.language, r.name.c_str(), best, kFonts[best].file);
    RET(best);
}

void sceFontFindFont(PspContext* c) {
    if (!validErr(A2)) {
        RET(ERR_KERNEL_INVALID_ARGUMENT);
        return;
    }
    FontLib* lib = findLib(A0);
    if (!lib) {
        wr32(A2, ERR_INVALID_LIBID);
        RET(0);
        return;
    }
    if (!memValid(A1) || !memValid(A1 + kStyleSize - 1)) {
        wr32(A2, ERR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    StyleReq r = readStyle(A1);
    float hRes = r.hRes > 0 ? r.hRes : lib->hRes;
    for (u32 i = 0; i < kNumFonts; i++) {
        if (!styleMatches(r, kFonts[i])) continue;
        if (r.h > 0) {
            if (fabsf(kFonts[i].size64 / 64.0f * 128.0f - hRes * r.h) > 0.001f) continue;
        } else if (r.v > 0) {
            continue;  // V alone never matches
        }
        wr32(A2, 0);
        RET(i);
        return;
    }
    wr32(A2, 0);
    RET(-1);
}

void sceFontGetFontInfoByIndexNumber(PspContext* c) {
    FontLib* lib = findLib(A0);
    if (!lib) {
        RET(ERR_INVALID_LIBID);
        return;
    }
    if (A2 >= kNumFonts || !memValid(A1) || !memValid(A1 + kStyleSize - 1)) {
        RET(ERR_INVALID_PARAMETER);
        return;
    }
    writeStyle(A1, kFonts[A2]);
    RET(0);
}

void sceFontOpen(PspContext* c) {
    u32 libH = A0, index = A1, mode = A2, errPtr = A3;
    if (!validErr(errPtr)) {
        RET(-1);
        return;
    }
    FontLib* lib = findLib(libH);
    if (!lib) {
        wr32(errPtr, ERR_INVALID_LIBID);
        RET(0);
        return;
    }
    if (index >= kNumFonts) {
        wr32(errPtr, ERR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    int slot = -1;
    for (size_t i = 0; i < lib->slots.size(); i++)
        if (lib->slots[i].refs > 0 && lib->slots[i].fontIndex == (int)index) slot = (int)i;
    if (slot < 0)
        for (size_t i = 0; i < lib->slots.size(); i++)
            if (lib->slots[i].refs == 0) {
                slot = (int)i;
                break;
            }
    if (slot < 0 || lib->slots[slot].refs >= kMaxRefs) {
        wr32(errPtr, ERR_TOO_MANY_OPEN_FONTS);
        RET(0);
        return;
    }
    Slot& s = lib->slots[slot];
    bool first = s.refs == 0;
    s.fontIndex = (int)index;
    s.refs++;
    g_fonts[s.handle] = {lib->handle, slot};
    if (first) {
        // FONT_OPEN_INTERNAL_FULL (1) loads the whole PGF, anything else is the "stingy" mode
        u32 size = mode == 1 ? 12 + kFonts[index].fileSize : kFonts[index].stingySize;
        s.allocAddr = guestAlloc(c, *lib, size);
        faceFor((int)index, lib->hRes);
        LOG_INFO("FONT", "sceFontOpen(%s, mode %u) -> %08x", kFonts[index].file, mode, s.handle);
        wr32(errPtr, 0);
        utility::runFontSelfTest(c, s.handle);  // no-op unless VC2_UTIL_SELFTEST is set
    }
    wr32(errPtr, 0);
    RET(s.handle);
}

void sceFontClose(PspContext* c) {
    OpenFont f;
    if (!findFont(A0, f, false)) {
        RET(0);
        return;
    }
    closeSlot(c, *f.lib, *f.slot, false);
    RET(0);
}

// PGFFontInfo (264 bytes)
void sceFontGetFontInfo(PspContext* c) {
    u32 p = A1;
    if (!memValid(p) || !memValid(p + kFontInfoSize - 1)) {
        RET(ERR_INVALID_PARAMETER);
        return;
    }
    OpenFont f;
    if (!findFont(A0, f, true)) {
        RET(ERR_INVALID_PARAMETER);
        return;
    }
    HostFace* face = faceFor(f.slot->fontIndex, f.lib->hRes);
    HostFaceMetrics m = face ? face->metrics() : HostFaceMetrics{};
    // The glyph box spans the tallest ascent and the deepest descent of the font. VC2 sizes its
    // glyph cells as maxGlyphWidth/Height + 1 and puts the baseline at
    // round((maxGlyphBaseY + maxGlyphDescender) / 64) + 2 rows from the top of the cell, so BaseY is
    // reported such that this lands exactly on the box's baseline (as it does with the PGF fonts).
    const int boxH = m.maxTop + m.maxBelow;
    const s32 v[10] = {
        m.maxW * 64,                            // maxGlyphWidthI
        boxH * 64,                              // maxGlyphHeightI
        m.maxTop * 64,                          // maxGlyphAscenderI
        -m.maxBelow * 64,                       // maxGlyphDescenderI
        m.maxLeft * 64,                         // maxGlyphLeftXI
        (m.maxTop + m.maxBelow - 2) * 64,       // maxGlyphBaseYI
        m.minCenterX * 64,                      // minGlyphCenterXI
        m.maxTop * 64,                          // maxGlyphTopYI
        m.maxAdvance * 64,                      // maxGlyphAdvanceXI
        std::max(m.height, boxH) * 64,          // maxGlyphAdvanceYI
    };
    for (int i = 0; i < 10; i++) {
        wr32(p + i * 4, (u32)v[i]);
        wrf(p + 40 + i * 4, v[i] / 64.0f);
    }
    wr16(p + 80, (u16)m.maxW);  // maxGlyphWidth (pixels)
    wr16(p + 82, (u16)boxH);    // maxGlyphHeight
    wr32(p + 84, (u32)m.numGlyphs);
    wr32(p + 88, 0);            // shadowMapLength
    writeStyle(p + 92, kFonts[f.slot->fontIndex]);
    wr8(p + 260, 4);            // BPP
    wr8(p + 261, 0);
    wr16(p + 262, 0);
    RET(0);
}

const HostGlyph* glyphFor(const OpenFont& f, u32 code) {
    HostFace* face = faceFor(f.slot->fontIndex, f.lib->hRes);
    if (!face) return nullptr;
    const HostGlyph* g = face->glyph(code);
    if (!g && code >= 0x20) g = face->glyph(f.lib->altChar);  // missing glyph -> alternate character
    return g;
}

// PGFCharInfo (60 bytes)
void writeCharInfo(u32 p, const HostGlyph* g, int lineHeight) {
    memset(memPtr(p), 0, kCharInfoSize);
    if (!g) return;  // zeroed, as the firmware does for characters the font doesn't have
    wr32(p + 0, (u32)g->w);
    wr32(p + 4, (u32)g->h);
    wr32(p + 8, (u32)g->left);
    wr32(p + 12, (u32)g->top);
    wr32(p + 16, (u32)(g->w * 64));                  // sfp26Width
    wr32(p + 20, (u32)(g->h * 64));                  // sfp26Height
    wr32(p + 24, (u32)(g->top * 64));                // sfp26Ascender
    wr32(p + 28, (u32)((g->top - g->h) * 64));       // sfp26Descender
    wr32(p + 32, (u32)(g->left * 64));               // sfp26BearingHX
    wr32(p + 36, (u32)(g->top * 64));                // sfp26BearingHY
    wr32(p + 40, (u32)(-(g->advance * 64) / 2));     // sfp26BearingVX (vertical layout: centred)
    wr32(p + 44, 0);                                 // sfp26BearingVY
    wr32(p + 48, (u32)(g->advance * 64));            // sfp26AdvanceH
    wr32(p + 52, (u32)(lineHeight * 64));            // sfp26AdvanceV
    wr16(p + 56, 0);                                 // shadowFlags
    wr16(p + 58, 0);                                 // shadowId
}

void sceFontGetCharInfo(PspContext* c) {
    u32 code = A1 & 0xFFFF, p = A2;
    if (!memValid(p) || !memValid(p + kCharInfoSize - 1)) {
        RET(ERR_INVALID_PARAMETER);
        return;
    }
    OpenFont f;
    if (!findFont(A0, f, true)) {
        RET(ERR_INVALID_PARAMETER);
        return;
    }
    const HostGlyph* g = glyphFor(f, code);
    HostFace* face = faceFor(f.slot->fontIndex, f.lib->hRes);
    writeCharInfo(p, g, face ? face->metrics().height : 0);
    // The firmware library keeps a decode buffer for the last glyph, re-allocated through the
    // game's allocator for each character it is asked about.
    if (g && g->advance != 0) {
        FontLib& lib = *f.lib;
        if (lib.charBitmap) {
            guestFree(c, lib, lib.charBitmap);
            lib.charBitmap = 0;
        }
        u32 size = (u32)(g->w * g->h);
        if (size) {
            lib.charBitmap = guestAlloc(c, lib, size);
            if (!lib.charBitmap) {
                RET(ERR_OUT_OF_MEMORY);
                return;
            }
        }
    }
    RET(0);
}

void sceFontGetCharImageRect(PspContext* c) {
    OpenFont f;
    if (!findFont(A0, f, true)) {
        RET(ERR_INVALID_PARAMETER);
        return;
    }
    if (!memValid(A2) || !memValid(A2 + 3)) {
        RET(ERR_INVALID_PARAMETER);
        return;
    }
    const HostGlyph* g = glyphFor(f, A1 & 0xFFFF);
    wr16(A2, (u16)(g ? g->w : 0));
    wr16(A2 + 2, (u16)(g ? g->h : 0));
    RET(0);
}

// GlyphImage {u32 pixelFormat; s32 xPos64, yPos64; u16 bufWidth, bufHeight, bytesPerLine, pad; u32 buffer}
void drawGlyph(u32 img, const HostGlyph* g, int clipX, int clipY, int clipW, int clipH) {
    if (!g || g->w <= 0 || g->h <= 0) return;
    const u32 fmt = rd32(img);
    const s32 xPos = (s32)rd32(img + 4), yPos = (s32)rd32(img + 8);
    const int bufW = rd16(img + 12), bufH = rd16(img + 14), bpl = rd16(img + 16);
    const u32 base = rd32(img + 20);
    if (fmt > PIXFMT_32) return;
    const int x0 = xPos >> 6, y0 = yPos >> 6, xFrac = xPos & 63;  // the vertical fraction is ignored
    if (clipX < 0) clipX = 0;
    if (clipY < 0) clipY = 0;
    if (clipW < 0) clipW = 8192;
    if (clipH < 0) clipH = 8192;
    auto sample = [&](int x, int y) -> int {
        return (x < 0 || y < 0 || x >= g->w || y >= g->h) ? 0 : g->pixels[(size_t)y * g->w + x];
    };
    const int scale = (fmt == PIXFMT_4 || fmt == PIXFMT_4_REV) ? 1 : 17;  // 4-bit vs 8-bit intensity
    const int rx1 = std::max(clipX, x0) - x0, ry1 = std::max(clipY, y0) - y0;
    const int rx2 = std::min(clipX + clipW - x0, g->w + (xFrac ? 1 : 0)), ry2 = std::min(clipY + clipH - y0, g->h);
    for (int yy = ry1; yy < ry2; yy++) {
        const int py = y0 + yy;
        if (py < 0 || py >= bufH) continue;
        for (int xx = rx1; xx < rx2; xx++) {
            const int px = x0 + xx;
            if (px < 0 || px >= bufW) continue;
            // horizontal sub-pixel position: blend with the left neighbour
            const int a = sample(xx - 1, yy) * scale, b = sample(xx, yy) * scale;
            const int v = (a * xFrac) / 64 + (b * (64 - xFrac) + 63) / 64;
            if (!v) continue;
            const u32 row = base + (u32)(py * bpl);
            switch (fmt) {
                case PIXFMT_4:
                case PIXFMT_4_REV: {
                    u32 a8 = row + px / 2;
                    if (!memValid(a8)) break;
                    int shift = ((px & 1) != (int)fmt) ? 4 : 0;
                    u8 old = rd8(a8);
                    int nv = std::min(((old >> shift) & 0xF) + v, 15);
                    wr8(a8, (u8)((old & ~(0xF << shift)) | (nv << shift)));
                    break;
                }
                case PIXFMT_8: {
                    u32 a8 = row + px;
                    if (memValid(a8)) wr8(a8, (u8)std::min(rd8(a8) + v, 255));
                    break;
                }
                case PIXFMT_24: {
                    u32 a8 = row + px * 3;
                    if (!memValid(a8 + 2)) break;
                    for (int i = 0; i < 3; i++) wr8(a8 + i, (u8)std::min(rd8(a8 + i) + v, 255));
                    break;
                }
                case PIXFMT_32: {
                    u32 a8 = row + px * 4;
                    if (!memValid(a8 + 3)) break;
                    for (int i = 0; i < 4; i++) wr8(a8 + i, (u8)std::min(rd8(a8 + i) + v, 255));
                    break;
                }
            }
        }
    }
}

u32 g_glyphLogCount = 0;

void glyphImage(PspContext* c, int clipX, int clipY, int clipW, int clipH) {
    u32 code = A1 & 0xFFFF, img = A2;
    if (!memValid(img) || !memValid(img + 23)) {
        RET(ERR_INVALID_PARAMETER);
        return;
    }
    OpenFont f;
    if (!findFont(A0, f, true)) {
        RET(ERR_INVALID_PARAMETER);
        return;
    }
    const HostGlyph* g = glyphFor(f, code);
    if (g_glyphLogCount < 64) {
        g_glyphLogCount++;
        LOG_DEBUG("FONT", "glyph U+%04X '%c' fmt %u pos (%.2f, %.2f) buf %ux%u bpl %u @%08x -> %dx%d", code,
                  code >= 0x20 && code < 0x7F ? (char)code : '?', rd32(img), (s32)rd32(img + 4) / 64.0,
                  (s32)rd32(img + 8) / 64.0, rd16(img + 12), rd16(img + 14), rd16(img + 16), rd32(img + 20),
                  g ? g->w : 0, g ? g->h : 0);
    }
    drawGlyph(img, g, clipX, clipY, clipW, clipH);
    RET(0);
}

void sceFontGetCharGlyphImage(PspContext* c) { glyphImage(c, -1, -1, -1, -1); }
void sceFontGetCharGlyphImage_Clip(PspContext* c) { glyphImage(c, (s32)A3, (s32)A4, (s32)A5, (s32)A6); }

void sceFontSetAltCharacterCode(PspContext* c) {
    FontLib* lib = findLib(A0);
    if (!lib) {
        RET(ERR_INVALID_LIBID);
        return;
    }
    lib->altChar = (u16)(A1 & 0xFFFF);
    wr16(lib->handle + 0x48, lib->altChar);
    RET(0);
}

void sceFontFlush(PspContext* c) {
    OpenFont f;
    if (!findFont(A0, f, true)) {
        RET(ERR_INVALID_PARAMETER);
        return;
    }
    if (f.lib->charBitmap) {
        guestFree(c, *f.lib, f.lib->charBitmap);
        f.lib->charBitmap = 0;
    }
    RET(0);
}

const HleFunction kLibFont[] = {
    {0x67F17ED7, "sceFontNewLib", sceFontNewLib},
    {0x574B6FBC, "sceFontDoneLib", sceFontDoneLib},
    {0x27F6E642, "sceFontGetNumFontList", sceFontGetNumFontList},
    {0xBC75D85B, "sceFontGetFontList", sceFontGetFontList},
    {0x099EF33C, "sceFontFindOptimumFont", sceFontFindOptimumFont},
    {0x681E61A7, "sceFontFindFont", sceFontFindFont},
    {0x5333322D, "sceFontGetFontInfoByIndexNumber", sceFontGetFontInfoByIndexNumber},
    {0xA834319D, "sceFontOpen", sceFontOpen},
    {0x3AEA8CB6, "sceFontClose", sceFontClose},
    {0x0DA7535E, "sceFontGetFontInfo", sceFontGetFontInfo},
    {0xDCC80C2F, "sceFontGetCharInfo", sceFontGetCharInfo},
    {0x5C3E4A9E, "sceFontGetCharImageRect", sceFontGetCharImageRect},
    {0x980F4895, "sceFontGetCharGlyphImage", sceFontGetCharGlyphImage},
    {0xCA1E6945, "sceFontGetCharGlyphImage_Clip", sceFontGetCharGlyphImage_Clip},
    {0xEE232411, "sceFontSetAltCharacterCode", sceFontSetAltCharacterCode},
    {0x02D7F94B, "sceFontFlush", sceFontFlush},
};

}  // namespace

HLE_MODULE("sceLibFont", kLibFont);

}  // namespace vc2rt::font
