#pragma once
// Host glyph rasterizer for sceLibFont: renders characters with a font installed on Windows (GDI)
// into PSP PGF-like 4-bit glyph bitmaps with integer pixel metrics. No guest-memory dependencies.
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vc2rt::font {

struct HostGlyph {
    int w = 0, h = 0;      // bitmap size in pixels (0 for blank glyphs such as space)
    int left = 0, top = 0; // bitmap origin relative to the pen position / baseline (y up)
    int advance = 0;       // horizontal advance in pixels
    std::vector<uint8_t> pixels;  // w*h values 0..15, row-major
};

struct HostFaceMetrics {
    int ascent = 0, descent = 0, height = 0;  // TEXTMETRIC values in pixels (descent positive)
    int maxW = 0, maxH = 0;                   // largest glyph bitmap over the Latin range
    int maxLeft = 0, maxTop = 0, minCenterX = 0, maxAdvance = 0;
    int maxBelow = 0;                         // deepest descent of any glyph below the baseline
    int numGlyphs = 0;
};

class HostFace {
public:
    // faces: candidate family names tried in order. pixelSize: em height in pixels.
    bool open(const std::vector<std::wstring>& faces, int pixelSize, int weight, bool italic);
    ~HostFace();
    // Returns nullptr when the font has no glyph for `code`.
    const HostGlyph* glyph(uint32_t code);
    const HostFaceMetrics& metrics() const { return metrics_; }
    const std::wstring& faceName() const { return faceName_; }

private:
    bool render(uint32_t code, HostGlyph& out);
    void* dc_ = nullptr;
    void* font_ = nullptr;
    void* oldFont_ = nullptr;
    HostFaceMetrics metrics_;
    std::wstring faceName_;
    std::unordered_map<uint32_t, std::unique_ptr<HostGlyph>> cache_;  // null entry = missing glyph
};

}  // namespace vc2rt::font
