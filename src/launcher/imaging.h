#pragma once
// Image helpers built on WIC: PNG decode/encode, high-quality scaling, multi-size .ico writing,
// plus the launcher's own procedurally generated emblem (original artwork, no game assets).
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vc2l {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // straight (non-premultiplied) RGBA8, top-down
    bool empty() const { return w <= 0 || h <= 0; }
};

// All functions initialize COM on the calling thread if needed.
bool decodeImage(const void* data, size_t size, Image& out, std::string* err = nullptr);
bool encodePng(const Image& img, std::vector<uint8_t>& out);
bool savePng(const Image& img, const std::filesystem::path& file);
Image scaleImage(const Image& src, int w, int h);  // high-quality (Fant) resampling

// Fits `src` into a transparent square of `side` pixels, keeping the aspect ratio, with `padFrac` margin.
Image fitSquare(const Image& src, int side, float padFrac);

// Writes a Vista-style .ico whose entries are PNG-compressed, one per entry in `sizes`.
bool writeIcoFromImage(const Image& src, const std::vector<int>& sizes, const std::filesystem::path& file,
                       std::string* err = nullptr);

// Original launcher emblem: an abstract gear ring with a compass star (procedural, anti-aliased).
Image renderEmblem(int size);

}  // namespace vc2l
