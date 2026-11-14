#include "imaging.h"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace vc2l {

namespace {

template <class T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
};

IWICImagingFactory* wicFactory() {
    // One factory per thread (COM apartment rules); launcher threads that use WIC call this.
    thread_local ComPtr<IWICImagingFactory> f;
    thread_local bool init = false;
    if (!init) {
        init = true;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // ok if already initialized differently
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    }
    return f.p;
}

std::vector<uint8_t> toPBGRA(const Image& img) {
    std::vector<uint8_t> o(img.rgba.size());
    for (size_t i = 0; i < o.size(); i += 4) {
        unsigned a = img.rgba[i + 3];
        o[i + 0] = (uint8_t)((img.rgba[i + 2] * a + 127) / 255);
        o[i + 1] = (uint8_t)((img.rgba[i + 1] * a + 127) / 255);
        o[i + 2] = (uint8_t)((img.rgba[i + 0] * a + 127) / 255);
        o[i + 3] = (uint8_t)a;
    }
    return o;
}

void fromPBGRA(const std::vector<uint8_t>& p, Image& img) {
    img.rgba.resize(p.size());
    for (size_t i = 0; i < p.size(); i += 4) {
        unsigned a = p[i + 3];
        auto un = [&](unsigned c) { return (uint8_t)(a ? std::min(255u, (c * 255 + a / 2) / a) : 0); };
        img.rgba[i + 0] = un(p[i + 2]);
        img.rgba[i + 1] = un(p[i + 1]);
        img.rgba[i + 2] = un(p[i + 0]);
        img.rgba[i + 3] = (uint8_t)a;
    }
}

}  // namespace

bool decodeImage(const void* data, size_t size, Image& out, std::string* err) {
    IWICImagingFactory* f = wicFactory();
    if (!f) {
        if (err) *err = "Windows Imaging Component is unavailable";
        return false;
    }
    ComPtr<IWICStream> st;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateStream(&st)) || FAILED(st->InitializeFromMemory((BYTE*)data, (DWORD)size)) ||
        FAILED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom))) {
        if (err) *err = "the image could not be decoded";
        return false;
    }
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (!w || !h || w > 8192 || h > 8192) {
        if (err) *err = "invalid image size";
        return false;
    }
    out.w = (int)w;
    out.h = (int)h;
    out.rgba.resize((size_t)w * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, (UINT)out.rgba.size(), out.rgba.data()))) {
        if (err) *err = "the image could not be decoded";
        return false;
    }
    return true;
}

bool encodePng(const Image& img, std::vector<uint8_t>& out) {
    IWICImagingFactory* f = wicFactory();
    if (!f || img.empty()) return false;
    ComPtr<IStream> mem;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &mem))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(mem, WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&frame, &props)) || FAILED(frame->Initialize(props)) ||
        FAILED(frame->SetSize((UINT)img.w, (UINT)img.h)))
        return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&fmt)) || fmt != GUID_WICPixelFormat32bppBGRA) return false;
    std::vector<uint8_t> bgra(img.rgba);
    for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    if (FAILED(frame->WritePixels((UINT)img.h, (UINT)img.w * 4, (UINT)bgra.size(), bgra.data())) || FAILED(frame->Commit()) ||
        FAILED(enc->Commit()))
        return false;
    HGLOBAL hg = nullptr;
    if (FAILED(GetHGlobalFromStream(mem, &hg))) return false;
    STATSTG stat{};
    mem->Stat(&stat, STATFLAG_NONAME);
    size_t n = (size_t)stat.cbSize.QuadPart;
    void* p = GlobalLock(hg);
    if (!p) return false;
    out.assign((uint8_t*)p, (uint8_t*)p + n);
    GlobalUnlock(hg);
    return true;
}

bool savePng(const Image& img, const std::filesystem::path& file) {
    std::vector<uint8_t> png;
    if (!encodePng(img, png)) return false;
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f.write((const char*)png.data(), (std::streamsize)png.size());
    return (bool)f;
}

Image scaleImage(const Image& src, int w, int h) {
    Image out;
    if (src.empty() || w <= 0 || h <= 0) return out;
    if (w == src.w && h == src.h) return src;
    IWICImagingFactory* f = wicFactory();
    if (!f) return out;
    std::vector<uint8_t> pm = toPBGRA(src);
    ComPtr<IWICBitmap> bmp;
    ComPtr<IWICBitmapScaler> sc;
    if (FAILED(f->CreateBitmapFromMemory((UINT)src.w, (UINT)src.h, GUID_WICPixelFormat32bppPBGRA, (UINT)src.w * 4,
                                         (UINT)pm.size(), pm.data(), &bmp)) ||
        FAILED(f->CreateBitmapScaler(&sc)))
        return out;
    // HighQualityCubic needs Windows 10; fall back to Fant.
    if (FAILED(sc->Initialize(bmp, (UINT)w, (UINT)h, WICBitmapInterpolationModeHighQualityCubic)) &&
        FAILED(sc->Initialize(bmp, (UINT)w, (UINT)h, WICBitmapInterpolationModeFant)))
        return out;
    std::vector<uint8_t> dst((size_t)w * h * 4);
    if (FAILED(sc->CopyPixels(nullptr, (UINT)w * 4, (UINT)dst.size(), dst.data()))) return out;
    out.w = w;
    out.h = h;
    fromPBGRA(dst, out);
    return out;
}

Image fitSquare(const Image& src, int side, float padFrac) {
    Image out;
    out.w = out.h = side;
    out.rgba.assign((size_t)side * side * 4, 0);
    if (src.empty()) return out;
    float avail = side * (1.0f - 2.0f * padFrac);
    float s = std::min(avail / src.w, avail / src.h);
    int w = std::max(1, (int)std::lround(src.w * s)), h = std::max(1, (int)std::lround(src.h * s));
    Image sc = scaleImage(src, w, h);
    if (sc.empty()) return out;
    int ox = (side - w) / 2, oy = (side - h) / 2;
    for (int y = 0; y < h; y++)
        memcpy(&out.rgba[((size_t)(oy + y) * side + ox) * 4], &sc.rgba[(size_t)y * w * 4], (size_t)w * 4);
    return out;
}

bool writeIcoFromImage(const Image& src, const std::vector<int>& sizes, const std::filesystem::path& file, std::string* err) {
    std::vector<std::vector<uint8_t>> pngs;
    for (int s : sizes) {
        // a little less padding at small sizes, where every pixel counts
        Image sq = fitSquare(src, s, s <= 32 ? 0.0f : 0.03f);
        std::vector<uint8_t> png;
        if (!encodePng(sq, png)) {
            if (err) *err = "PNG encoding failed";
            return false;
        }
        pngs.push_back(std::move(png));
    }
    std::vector<uint8_t> ico;
    auto w16 = [&](unsigned v) {
        ico.push_back((uint8_t)v);
        ico.push_back((uint8_t)(v >> 8));
    };
    auto w32 = [&](uint32_t v) {
        w16(v & 0xFFFF);
        w16(v >> 16);
    };
    w16(0);
    w16(1);  // type: icon
    w16((unsigned)sizes.size());
    uint32_t offset = 6 + 16 * (uint32_t)sizes.size();
    for (size_t i = 0; i < sizes.size(); i++) {
        int s = sizes[i];
        ico.push_back((uint8_t)(s >= 256 ? 0 : s));
        ico.push_back((uint8_t)(s >= 256 ? 0 : s));
        ico.push_back(0);  // palette
        ico.push_back(0);
        w16(1);   // planes
        w16(32);  // bpp
        w32((uint32_t)pngs[i].size());
        w32(offset);
        offset += (uint32_t)pngs[i].size();
    }
    for (auto& p : pngs) ico.insert(ico.end(), p.begin(), p.end());
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f.write((const char*)ico.data(), (std::streamsize)ico.size());
    if (!f) {
        if (err) *err = "could not write the icon file";
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ procedural emblem
namespace {

struct Col {
    float r, g, b, a;
};

Col lerp(Col a, Col b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

Col hex(uint32_t c, float a = 1.0f) { return {((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, a}; }

// premultiplied "over"
void over(Col& dst, Col src) {
    float a = src.a;
    dst.r = src.r * a + dst.r * (1 - a);
    dst.g = src.g * a + dst.g * (1 - a);
    dst.b = src.b * a + dst.b * (1 - a);
    dst.a = a + dst.a * (1 - a);
}

float cross2(float ax, float ay, float bx, float by) { return ax * by - ay * bx; }

// point inside triangle (either winding)
bool inTri(float x, float y, float ax, float ay, float bx, float by, float cx, float cy) {
    float c1 = cross2(bx - ax, by - ay, x - ax, y - ay);
    float c2 = cross2(cx - bx, cy - by, x - bx, y - by);
    float c3 = cross2(ax - cx, ay - cy, x - cx, y - cy);
    return (c1 >= 0 && c2 >= 0 && c3 >= 0) || (c1 <= 0 && c2 <= 0 && c3 <= 0);
}

// Returns which half-arm of an n-pointed star contains (x,y): -1 if none, else 2*arm + side.
// Each arm is split along its axis into two triangles so it can be shaded like a bevelled facet.
int starArm(float x, float y, int points, float outer, float inner, float rot) {
    const float pi = 3.14159265f;
    for (int k = 0; k < points; k++) {
        float a = rot + k * 2 * pi / points;
        float tx = cosf(a) * outer, ty = sinf(a) * outer;
        float la = a - pi / points, ra = a + pi / points;
        float lx = cosf(la) * inner, ly = sinf(la) * inner, rx = cosf(ra) * inner, ry = sinf(ra) * inner;
        if (inTri(x, y, 0, 0, lx, ly, tx, ty)) return k * 2;
        if (inTri(x, y, 0, 0, tx, ty, rx, ry)) return k * 2 + 1;
    }
    return -1;
}

Col emblemSample(float x, float y) {
    const float pi = 3.14159265f;
    Col c{0, 0, 0, 0};
    float r = sqrtf(x * x + y * y);
    float th = atan2f(y, x);
    // backdrop disc with a radial gradient
    if (r < 0.97f) over(c, lerp(hex(0x2a2f6e), hex(0x0e1131), std::min(1.0f, r / 0.97f)));
    // gear: ring + 12 trapezoidal teeth, angular gradient amber -> coral -> magenta
    {
        const int teeth = 12;
        float seg = fmodf((th + pi) / (2 * pi) * teeth + 0.25f, 1.0f);  // 0..1 within a tooth period
        float d = fabsf(seg - 0.5f);                                    // 0 at tooth centre
        float outer = 0.80f;
        if (d < 0.22f) outer = 0.93f;
        else if (d < 0.30f) outer = 0.93f - (d - 0.22f) / 0.08f * 0.13f;  // sloped tooth flanks
        if (r > 0.64f && r < outer) {
            float t = (y + 1) * 0.5f;
            Col g = t < 0.5f ? lerp(hex(0xffc24a), hex(0xff6f61), t * 2) : lerp(hex(0xff6f61), hex(0xe0429a), (t - 0.5f) * 2);
            // inner bevel highlight
            if (r < 0.68f) g = lerp(g, hex(0xffffff), 0.35f);
            over(c, g);
        }
    }
    // thin violet ring
    if (r > 0.53f && r < 0.565f) over(c, hex(0x9d8cff, 0.95f));
    // secondary diagonal star
    if (int a = starArm(x, y, 4, 0.44f, 0.09f, pi / 4); a >= 0) over(c, (a & 1) ? hex(0x6b5cff) : hex(0x8f84ff));
    // main compass star (north arm points up)
    if (int a = starArm(x, y, 4, 0.60f, 0.15f, -pi / 2); a >= 0) {
        int arm = a / 2;
        bool lit = (a & 1) == 0;
        Col col = (arm % 2 == 0) ? (lit ? hex(0x7fe6ff) : hex(0x22a6e0)) : (lit ? hex(0xf2f7ff) : hex(0xb7c4e8));
        if (arm == 0) col = lit ? hex(0xffe08a) : hex(0xffb03b);  // north point in gold
        over(c, col);
    }
    // hub
    if (r < 0.085f) over(c, hex(0x151a44));
    if (r < 0.045f) over(c, hex(0xffffff));
    return c;
}

}  // namespace

Image renderEmblem(int size) {
    Image img;
    img.w = img.h = size;
    img.rgba.resize((size_t)size * size * 4);
    const int ss = size <= 64 ? 6 : 4;
    for (int py = 0; py < size; py++)
        for (int px = 0; px < size; px++) {
            Col acc{0, 0, 0, 0};
            for (int sy = 0; sy < ss; sy++)
                for (int sx = 0; sx < ss; sx++) {
                    float x = ((px + (sx + 0.5f) / ss) / size) * 2 - 1;
                    float y = ((py + (sy + 0.5f) / ss) / size) * 2 - 1;
                    Col s = emblemSample(x, y);  // premultiplied
                    acc.r += s.r, acc.g += s.g, acc.b += s.b, acc.a += s.a;
                }
            float n = (float)(ss * ss);
            float a = acc.a / n;
            uint8_t* o = &img.rgba[((size_t)py * size + px) * 4];
            o[0] = (uint8_t)std::clamp(a > 0 ? acc.r / n / a * 255 : 0.f, 0.f, 255.f);
            o[1] = (uint8_t)std::clamp(a > 0 ? acc.g / n / a * 255 : 0.f, 0.f, 255.f);
            o[2] = (uint8_t)std::clamp(a > 0 ? acc.b / n / a * 255 : 0.f, 0.f, 255.f);
            o[3] = (uint8_t)std::clamp(a * 255, 0.f, 255.f);
        }
    return img;
}

}  // namespace vc2l
