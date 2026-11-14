// GDI glyph rasterizer used by the sceLibFont HLE (see font_render.h).
#include "font_render.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <climits>

#pragma comment(lib, "gdi32.lib")

namespace vc2rt::font {

HostFace::~HostFace() {
    HDC dc = (HDC)dc_;
    if (dc && oldFont_) SelectObject(dc, (HGDIOBJ)oldFont_);
    if (font_) DeleteObject((HGDIOBJ)font_);
    if (dc) DeleteDC(dc);
}

bool HostFace::open(const std::vector<std::wstring>& faces, int pixelSize, int weight, bool italic) {
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    dc_ = dc;
    HFONT chosen = nullptr;
    for (size_t i = 0; i < faces.size() && !chosen; i++) {
        LOGFONTW lf{};
        lf.lfHeight = -std::max(1, pixelSize);  // negative: character (em) height
        lf.lfWeight = weight;
        lf.lfItalic = italic ? TRUE : FALSE;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfOutPrecision = OUT_TT_ONLY_PRECIS;
        lf.lfQuality = ANTIALIASED_QUALITY;
        wcsncpy_s(lf.lfFaceName, faces[i].c_str(), _TRUNCATE);
        HFONT f = CreateFontIndirectW(&lf);
        if (!f) continue;
        HGDIOBJ old = SelectObject(dc, f);
        wchar_t got[LF_FACESIZE] = {};
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, old);
        // accept an exact family match, or the last candidate (GDI then substitutes something)
        if (_wcsicmp(got, faces[i].c_str()) == 0 || i + 1 == faces.size()) {
            chosen = f;
            faceName_ = got;
        } else {
            DeleteObject(f);
        }
    }
    if (!chosen) return false;
    font_ = chosen;
    oldFont_ = SelectObject(dc, chosen);

    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    metrics_.ascent = tm.tmAscent;
    metrics_.descent = tm.tmDescent;
    metrics_.height = tm.tmHeight;
    DWORD rsz = GetFontUnicodeRanges(dc, nullptr);
    if (rsz) {
        std::vector<uint8_t> buf(rsz);
        GLYPHSET* gs = (GLYPHSET*)buf.data();
        if (GetFontUnicodeRanges(dc, gs)) metrics_.numGlyphs = (int)gs->cGlyphsSupported;
    }
    // Font-wide maxima, measured over the Latin range the PSP's ltn fonts cover.
    int minCenter = INT_MAX;
    auto scan = [&](uint32_t from, uint32_t to) {
        for (uint32_t c = from; c <= to; c++) {
            const HostGlyph* g = glyph(c);
            if (!g) continue;
            metrics_.maxW = std::max(metrics_.maxW, g->w);
            metrics_.maxH = std::max(metrics_.maxH, g->h);
            metrics_.maxLeft = std::max(metrics_.maxLeft, g->left);
            metrics_.maxTop = std::max(metrics_.maxTop, g->top);
            if (g->h) metrics_.maxBelow = std::max(metrics_.maxBelow, g->h - g->top);
            metrics_.maxAdvance = std::max(metrics_.maxAdvance, g->advance);
            if (g->w) minCenter = std::min(minCenter, g->left + g->w / 2);
        }
    };
    scan(0x20, 0x7E);
    scan(0xA0, 0xFF);
    metrics_.minCenterX = minCenter == INT_MAX ? 0 : minCenter;
    return true;
}

const HostGlyph* HostFace::glyph(uint32_t code) {
    auto it = cache_.find(code);
    if (it != cache_.end()) return it->second.get();
    auto g = std::make_unique<HostGlyph>();
    if (!render(code, *g)) g.reset();
    const HostGlyph* p = g.get();
    cache_[code] = std::move(g);
    return p;
}

bool HostFace::render(uint32_t code, HostGlyph& out) {
    HDC dc = (HDC)dc_;
    if (!dc || code < 0x20 || code > 0xFFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
    wchar_t wc = (wchar_t)code;
    WORD idx = 0xFFFF;
    if (GetGlyphIndicesW(dc, &wc, 1, &idx, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR || idx == 0xFFFF) return false;
    const MAT2 identity = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    GLYPHMETRICS gm{};
    DWORD size = GetGlyphOutlineW(dc, code, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &identity);
    if (size == GDI_ERROR) return false;
    out.advance = gm.gmCellIncX;
    out.left = gm.gmptGlyphOrigin.x;
    out.top = gm.gmptGlyphOrigin.y;
    if (size == 0) {  // blank glyph (space)
        out.w = out.h = 0;
        return true;
    }
    std::vector<uint8_t> buf(size);
    if (GetGlyphOutlineW(dc, code, GGO_GRAY8_BITMAP, &gm, size, buf.data(), &identity) == GDI_ERROR) return false;
    out.w = (int)gm.gmBlackBoxX;
    out.h = (int)gm.gmBlackBoxY;
    const int pitch = (out.w + 3) & ~3;
    if ((size_t)pitch * out.h > buf.size()) return false;
    out.pixels.resize((size_t)out.w * out.h);
    for (int y = 0; y < out.h; y++)
        for (int x = 0; x < out.w; x++) {
            int v = std::min<int>(buf[(size_t)y * pitch + x], 64);  // 65 grey levels
            out.pixels[(size_t)y * out.w + x] = (uint8_t)((v * 15 + 32) / 64);
        }
    return true;
}

}  // namespace vc2rt::font
