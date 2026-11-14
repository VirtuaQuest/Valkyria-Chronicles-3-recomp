#include "ui.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>

namespace vc2l::ui {

namespace {
Fonts g_fonts;
float g_scale = 1.0f;
constexpr float kPi = 3.14159265f;

ImVec2 operator+(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }
ImVec2 operator-(ImVec2 a, ImVec2 b) { return {a.x - b.x, a.y - b.y}; }
ImVec2 operator*(ImVec2 a, float s) { return {a.x * s, a.y * s}; }

// Glyphs: Latin-1, general punctuation (dashes, bullets, ellipsis), arrows, math, geometric shapes.
const ImWchar kTextRanges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2010, 0x2027, 0x2030, 0x205E, 0x20AC, 0x20AC, 0x2100, 0x214F, 0x2190,
                               0x21FF, 0x2200, 0x22FF, 0x25A0, 0x25FF, 0x2600, 0x26FF, 0x2700, 0x27BF, 0};
const ImWchar kSymbolRanges[] = {0x2100, 0x214F, 0x2190, 0x21FF, 0x2200, 0x23FF, 0x25A0, 0x25FF, 0x2600, 0x26FF, 0x2700, 0x27BF, 0};

ImFont* addFont(const char* file, float size, bool mergeSymbols) {
    ImGuiIO& io = ImGui::GetIO();
    std::string dir = "C:\\Windows\\Fonts\\";
    if (const char* windir = getenv("WINDIR")) dir = std::string(windir) + "\\Fonts\\";
    std::error_code ec;
    ImFont* f = nullptr;
    if (std::filesystem::exists(dir + file, ec)) {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        cfg.PixelSnapH = false;
        f = io.Fonts->AddFontFromFileTTF((dir + file).c_str(), size, &cfg, kTextRanges);
        if (f && mergeSymbols && std::filesystem::exists(dir + "seguisym.ttf", ec)) {
            ImFontConfig m;
            m.MergeMode = true;
            m.OversampleH = 2;
            m.GlyphOffset.y = size * 0.05f;
            io.Fonts->AddFontFromFileTTF((dir + "seguisym.ttf").c_str(), size, &m, kSymbolRanges);
        }
    }
    if (!f) {
        ImFontConfig cfg;
        cfg.SizePixels = size;
        f = io.Fonts->AddFontDefault(&cfg);
    }
    return f;
}

void applyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 0;
    s.ChildRounding = 14;
    s.FrameRounding = 8;
    s.PopupRounding = 10;
    s.GrabRounding = 8;
    s.ScrollbarRounding = 10;
    s.TabRounding = 8;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.FrameBorderSize = 0;
    s.PopupBorderSize = 1;
    s.WindowPadding = {18, 14};
    s.FramePadding = {10, 6};
    s.ItemSpacing = {10, 8};
    s.ItemInnerSpacing = {8, 6};
    s.ScrollbarSize = 12;
    s.GrabMinSize = 14;
    s.CellPadding = {8, 5};
    s.SeparatorTextBorderSize = 2;
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = toVec4(col::text);
    c[ImGuiCol_TextDisabled] = toVec4(col::muted);
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.12f, 0.27f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.55f, 0.42f, 1.0f, 0.22f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.55f, 0.42f, 1.0f, 0.34f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.11f, 0.25f, 1);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.20f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.55f, 0.42f, 1.0f, 0.6f);
    c[ImGuiCol_CheckMark] = toVec4(col::cyan);
    c[ImGuiCol_SliderGrab] = toVec4(col::violet);
    c[ImGuiCol_SliderGrabActive] = toVec4(col::magenta);
    c[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.07f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.55f, 0.42f, 1.0f, 0.35f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.55f, 0.42f, 1.0f, 0.55f);
    c[ImGuiCol_Header] = ImVec4(0.55f, 0.42f, 1.0f, 0.20f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.55f, 0.42f, 1.0f, 0.32f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.55f, 0.42f, 1.0f, 0.45f);
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_SeparatorHovered] = toVec4(col::violet);
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Tab] = ImVec4(1, 1, 1, 0.05f);
    c[ImGuiCol_TabHovered] = ImVec4(0.55f, 0.42f, 1.0f, 0.4f);
    c[ImGuiCol_TabSelected] = ImVec4(0.55f, 0.42f, 1.0f, 0.3f);
    c[ImGuiCol_TableHeaderBg] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_TableBorderLight] = ImVec4(1, 1, 1, 0.05f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.21f, 0.82f, 1.0f, 0.30f);
    c[ImGuiCol_NavCursor] = toVec4(col::cyan);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.03f, 0.03f, 0.10f, 0.65f);
    c[ImGuiCol_PlotHistogram] = toVec4(col::cyan);
}
}  // namespace

Fonts& fonts() { return g_fonts; }
float scale() { return g_scale; }

void setup(float dpiScale) {
    g_scale = std::clamp(dpiScale, 0.75f, 4.0f);
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_fonts.body = addFont("segoeui.ttf", 17.0f * g_scale, true);
    g_fonts.bold = addFont("seguisb.ttf", 17.0f * g_scale, true);
    g_fonts.heading = addFont("seguisb.ttf", 20.0f * g_scale, true);
    g_fonts.caption = addFont("segoeui.ttf", 14.5f * g_scale, true);
    g_fonts.title = addFont("seguibl.ttf", 40.0f * g_scale, false);
    g_fonts.mono = addFont("consola.ttf", 14.5f * g_scale, false);
    io.FontDefault = g_fonts.body;
    applyTheme();
    ImGui::GetStyle().ScaleAllSizes(g_scale);
}

ImU32 withAlpha(ImU32 c, float a) {
    unsigned al = (unsigned)std::clamp(((c >> IM_COL32_A_SHIFT) & 0xFF) * a, 0.0f, 255.0f);
    return (c & ~IM_COL32_A_MASK) | (al << IM_COL32_A_SHIFT);
}

ImU32 lerpColor(ImU32 a, ImU32 b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto ch = [&](int sh) {
        float x = (float)((a >> sh) & 0xFF), y = (float)((b >> sh) & 0xFF);
        return (ImU32)(x + (y - x) * t + 0.5f) << sh;
    };
    return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | ch(IM_COL32_A_SHIFT);
}

ImVec4 toVec4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

void gradientRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c0, ImU32 c1, float rounding, bool horizontal, ImDrawFlags flags) {
    if (b.x - a.x < 1 || b.y - a.y < 1) return;
    // Draw opaque white geometry, then recolour its vertices: keeps rounded corners, which
    // AddRectFilledMultiColor cannot do. Alpha is interpolated separately below.
    int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, IM_COL32_WHITE, rounding, flags);
    int v1 = dl->VtxBuffer.Size;
    ImVec2 p0 = a, p1 = horizontal ? ImVec2(b.x, a.y) : ImVec2(a.x, b.y);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, v1, p0, p1, c0, c1);
    float a0 = ((c0 >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f, a1 = ((c1 >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    float len = horizontal ? (b.x - a.x) : (b.y - a.y);
    for (int i = v0; i < v1; i++) {
        ImDrawVert& v = dl->VtxBuffer[i];
        float t = std::clamp(((horizontal ? v.pos.x - a.x : v.pos.y - a.y) / len), 0.0f, 1.0f);
        float al = (a0 + (a1 - a0) * t) * (((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f);
        v.col = (v.col & ~IM_COL32_A_MASK) | ((ImU32)(al * 255.0f + 0.5f) << IM_COL32_A_SHIFT);
    }
}

void glow(ImDrawList* dl, ImVec2 c, float radius, ImU32 color, int rings) {
    float baseA = ((color >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    for (int i = 0; i < rings; i++) {
        float t = (float)i / rings;
        float r = radius * (1.0f - t);
        float a = baseA / rings * (0.6f + t);
        dl->AddCircleFilled(c, r, withAlpha(color | IM_COL32_A_MASK, a), 64);
    }
}

void drawGear(ImDrawList* dl, ImVec2 c, float r, int teeth, float rot, ImU32 color, float thickness) {
    std::vector<ImVec2> pts;
    float rin = r * 0.84f;
    for (int i = 0; i < teeth; i++) {
        float a0 = rot + i * 2 * kPi / teeth, step = 2 * kPi / teeth;
        float as[4] = {a0, a0 + step * 0.18f, a0 + step * 0.48f, a0 + step * 0.66f};
        float rs[4] = {rin, r, r, rin};
        for (int k = 0; k < 4; k++) pts.push_back({c.x + cosf(as[k]) * rs[k], c.y + sinf(as[k]) * rs[k]});
    }
    dl->AddPolyline(pts.data(), (int)pts.size(), color, ImDrawFlags_Closed, thickness);
    dl->AddCircle(c, r * 0.62f, color, 96, thickness);
    dl->AddCircle(c, r * 0.20f, color, 48, thickness);
    for (int i = 0; i < 6; i++) {
        float a = rot + i * kPi / 3;
        dl->AddLine({c.x + cosf(a) * r * 0.20f, c.y + sinf(a) * r * 0.20f}, {c.x + cosf(a) * r * 0.62f, c.y + sinf(a) * r * 0.62f},
                    color, thickness);
    }
}

void drawBackground(float time) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 sz = ImGui::GetIO().DisplaySize;
    float w = sz.x, h = sz.y;
    dl->AddRectFilledMultiColor({0, 0}, sz, IM_COL32(16, 19, 46, 255), IM_COL32(28, 18, 62, 255), IM_COL32(40, 16, 66, 255),
                                IM_COL32(12, 24, 56, 255));
    // soft drifting colour glows
    glow(dl, {w * (0.86f + 0.03f * sinf(time * 0.21f)), h * (0.10f + 0.03f * cosf(time * 0.17f))}, h * 0.62f,
         withAlpha(col::magenta, 0.20f));
    glow(dl, {w * (0.06f + 0.02f * cosf(time * 0.19f)), h * (0.94f + 0.02f * sinf(time * 0.23f))}, h * 0.70f,
         withAlpha(col::cyan, 0.16f));
    glow(dl, {w * 0.45f, h * (0.55f + 0.04f * sinf(time * 0.13f))}, h * 0.55f, withAlpha(col::violet, 0.10f));
    // diagonal hatching in the top-right corner
    float sp = S(16);
    for (float x = w * 0.55f; x < w + h; x += sp) {
        float a = std::clamp((x - w * 0.55f) / (w * 0.45f), 0.0f, 1.0f);
        dl->AddLine({x, 0}, {x - S(170), S(170)}, IM_COL32(255, 255, 255, (int)(9 * a)), S(1.0f));
    }
    // dot grid fading out from the left edge
    for (float y = S(130); y < h; y += S(24))
        for (float x = S(14); x < w * 0.3f; x += S(24)) {
            float a = (1.0f - x / (w * 0.3f)) * 0.5f;
            dl->AddCircleFilled({x, y}, S(1.3f), IM_COL32(170, 190, 255, (int)(40 * a)), 6);
        }
    // big slow gears in the bottom-right corner
    drawGear(dl, {w * 0.93f, h * 1.02f}, h * 0.40f, 18, time * 0.04f, IM_COL32(200, 190, 255, 16), S(2.0f));
    drawGear(dl, {w * 0.93f - h * 0.66f, h * 1.10f}, h * 0.27f, 12, -time * 0.06f + 0.13f, IM_COL32(120, 220, 255, 14),
             S(2.0f));
    // header separator light beam
    gradientRect(dl, {0, S(112)}, {w, S(113.5f)}, withAlpha(col::cyan, 0.0f), withAlpha(col::magenta, 0.55f), 0);
    gradientRect(dl, {w * 0.5f, S(112)}, {w, S(113.5f)}, withAlpha(col::magenta, 0.55f), withAlpha(col::amber, 0.0f), 0);
}

bool beginCard(const char* id, ImVec2 size, ImU32 accent, const char* title, const char* icon) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 sz = size;
    if (sz.x <= 0) sz.x = avail.x + sz.x;
    if (sz.y <= 0) sz.y = avail.y + sz.y;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = S(14);
    dl->AddRectFilled(p + ImVec2(0, S(4)), p + sz + ImVec2(0, S(6)), IM_COL32(0, 0, 0, 50), r);  // drop shadow
    gradientRect(dl, p, p + sz, IM_COL32(34, 38, 84, 220), IM_COL32(22, 24, 58, 220), r, false);
    dl->AddRect(p, p + sz, col::cardBorder, r, 0, S(1));
    // accent strip
    gradientRect(dl, p + ImVec2(S(16), 0), ImVec2(p.x + sz.x - S(16), p.y + S(3)), accent, withAlpha(accent, 0.0f), S(2));
    // corner glow
    dl->PushClipRect(p, p + sz, true);
    glow(dl, p + ImVec2(sz.x, 0), S(120), withAlpha(accent, 0.10f), 16);
    dl->PopClipRect();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18), S(14)));
    bool open = ImGui::BeginChild(id, sz, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    if (title) {
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        ImVec2 tp = ImGui::GetCursorScreenPos();
        ImGui::PushFont(fonts().heading);
        float lh = ImGui::GetTextLineHeight();
        if (icon) {
            // numbered/icon disc
            float rad = lh * 0.5f;
            ImVec2 cc = tp + ImVec2(rad, lh * 0.5f);
            cdl->AddCircleFilled(cc, rad, withAlpha(accent, 0.22f), 32);
            cdl->AddCircle(cc, rad, accent, 32, S(1.5f));
            ImGui::PushFont(fonts().bold);
            ImVec2 ts = ImGui::CalcTextSize(icon);
            cdl->AddText(cc - ts * 0.5f, accent, icon);
            ImGui::PopFont();
            ImGui::SetCursorScreenPos(tp + ImVec2(lh + S(10), 0));
        }
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        ImGui::Dummy({0, S(2)});
    }
    return open;
}

void endCard() { ImGui::EndChild(); }

bool gradientButton(const char* label, ImVec2 size, ImU32 c0, ImU32 c1, bool enabled, ImFont* font) {
    ImGui::PushID(label);
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (!enabled) ImGui::BeginDisabled();
    bool pressed = ImGui::InvisibleButton("##btn", size);
    if (!enabled) ImGui::EndDisabled();
    bool hov = enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    bool act = enabled && ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = size.y * 0.5f;
    ImVec2 q = p + size;
    if (enabled) {
        if (hov) glow(dl, p + size * 0.5f, size.x * 0.62f, withAlpha(lerpColor(c0, c1, 0.5f), 0.22f), 14);
        ImU32 a = c0, b = c1;
        if (hov) a = lerpColor(a, IM_COL32_WHITE, 0.15f), b = lerpColor(b, IM_COL32_WHITE, 0.15f);
        if (act) a = lerpColor(a, IM_COL32_BLACK, 0.12f), b = lerpColor(b, IM_COL32_BLACK, 0.12f);
        dl->AddRectFilled(p + ImVec2(0, S(3)), q + ImVec2(0, S(3)), withAlpha(lerpColor(c0, c1, 0.5f), 0.25f), r);
        gradientRect(dl, p, q, a, b, r);
        // glossy top highlight
        gradientRect(dl, p + ImVec2(S(4), S(2)), ImVec2(q.x - S(4), p.y + size.y * 0.48f), IM_COL32(255, 255, 255, 46),
                     IM_COL32(255, 255, 255, 0), r, false);
    } else {
        gradientRect(dl, p, q, IM_COL32(70, 74, 110, 160), IM_COL32(60, 62, 96, 160), r);
    }
    ImGui::PushFont(font ? font : fonts().bold);
    const char* end = ImGui::FindRenderedTextEnd(label);
    ImVec2 ts = ImGui::CalcTextSize(label, end);
    ImVec2 tp = p + (size - ts) * 0.5f;
    if (enabled) dl->AddText(tp + ImVec2(0, S(1)), IM_COL32(0, 0, 0, 70), label, end);
    dl->AddText(tp, enabled ? IM_COL32_WHITE : IM_COL32(170, 175, 205, 200), label, end);
    ImGui::PopFont();
    ImGui::PopID();
    return pressed && enabled;
}

bool pillButton(const char* label, bool selected, ImVec2 size) {
    ImGui::PushID(label);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton("##pill", size);
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = size.y * 0.5f;
    if (selected) {
        glow(dl, p + size * 0.5f, size.x * 0.6f, withAlpha(col::violet, 0.25f), 12);
        gradientRect(dl, p, p + size, col::violet, col::magenta, r);
    } else {
        dl->AddRectFilled(p, p + size, hov ? IM_COL32(255, 255, 255, 30) : IM_COL32(255, 255, 255, 12), r);
        dl->AddRect(p, p + size, IM_COL32(255, 255, 255, 30), r, 0, S(1));
    }
    ImGui::PushFont(fonts().bold);
    const char* end = ImGui::FindRenderedTextEnd(label);
    ImVec2 ts = ImGui::CalcTextSize(label, end);
    dl->AddText(p + (size - ts) * 0.5f, selected || hov ? IM_COL32_WHITE : col::muted, label, end);
    ImGui::PopFont();
    ImGui::PopID();
    return pressed;
}

void progressBar(float frac, ImVec2 size, bool animate, float time, const char* overlay) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (size.x <= 0) size.x = ImGui::GetContentRegionAvail().x + size.x;
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = size.y * 0.5f;
    ImVec2 q = p + size;
    dl->AddRectFilled(p, q, IM_COL32(8, 10, 30, 200), r);
    dl->AddRect(p, q, IM_COL32(255, 255, 255, 22), r, 0, S(1));
    frac = std::clamp(frac, 0.0f, 1.0f);
    float fw = size.x * frac;
    if (fw >= 1.0f) {
        ImVec2 fq(p.x + std::max(fw, size.y), q.y);
        dl->PushClipRect(p, ImVec2(p.x + fw, q.y), true);
        // three-stop gradient cyan -> violet -> magenta across the whole track
        ImVec2 mid(p.x + size.x * 0.5f, q.y);
        gradientRect(dl, p, ImVec2(mid.x + r, q.y), col::cyan, col::violet, r, true, ImDrawFlags_RoundCornersLeft);
        gradientRect(dl, ImVec2(mid.x, p.y), q, col::violet, col::magenta, r, true, ImDrawFlags_RoundCornersRight);
        if (animate) {
            float sp = size.y * 1.1f;
            float off = fmodf(time * S(40), sp);
            for (float x = p.x - size.y * 2 + off; x < p.x + fw + size.y; x += sp) {
                ImVec2 pts[4] = {{x, q.y}, {x + size.y * 0.5f, q.y}, {x + size.y, p.y}, {x + size.y * 0.5f, p.y}};
                dl->AddConvexPolyFilled(pts, 4, IM_COL32(255, 255, 255, 30));
            }
        }
        gradientRect(dl, p + ImVec2(0, S(2)), ImVec2(q.x, p.y + size.y * 0.45f), IM_COL32(255, 255, 255, 60),
                     IM_COL32(255, 255, 255, 0), r, false);
        dl->PopClipRect();
        (void)fq;
        // leading-edge glow
        if (animate && frac < 1.0f) glow(dl, ImVec2(p.x + fw, p.y + size.y * 0.5f), size.y * 1.4f, withAlpha(col::magenta, 0.45f), 12);
    }
    if (overlay) {
        ImGui::PushFont(fonts().bold);
        ImVec2 ts = ImGui::CalcTextSize(overlay);
        ImVec2 tp = p + (size - ts) * 0.5f;
        dl->AddText(tp + ImVec2(0, S(1)), IM_COL32(0, 0, 0, 140), overlay);
        dl->AddText(tp, IM_COL32_WHITE, overlay);
        ImGui::PopFont();
    }
}

void badge(const char* text, ImU32 color) {
    ImGui::PushFont(fonts().caption);
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 pad(S(9), S(3));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 sz = ts + pad * 2;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, p + sz, withAlpha(color, 0.18f), sz.y * 0.5f);
    dl->AddRect(p, p + sz, withAlpha(color, 0.65f), sz.y * 0.5f, 0, S(1));
    dl->AddText(p + pad, color, text);
    ImGui::Dummy(sz);
    ImGui::PopFont();
}

void keycap(const char* text) {
    ImGui::PushFont(fonts().caption);
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 pad(S(8), S(2));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 sz = ts + pad * 2;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p + ImVec2(0, S(2)), p + sz + ImVec2(0, S(2)), IM_COL32(0, 0, 0, 90), S(5));
    gradientRect(dl, p, p + sz, IM_COL32(74, 80, 140, 255), IM_COL32(50, 54, 104, 255), S(5), false);
    dl->AddRect(p, p + sz, IM_COL32(255, 255, 255, 40), S(5), 0, S(1));
    dl->AddText(p + pad, col::text, text);
    ImGui::Dummy(sz + ImVec2(0, S(2)));
    ImGui::PopFont();
}

void spinner(float radius, float thickness, ImU32 color, float time) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float lh = ImGui::GetTextLineHeight();
    ImGui::Dummy({radius * 2, lh});
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(p.x + radius, p.y + lh * 0.5f);
    float start = time * 6.0f;
    dl->PathClear();
    for (int i = 0; i <= 24; i++) {
        float a = start + i / 24.0f * kPi * 1.4f;
        dl->PathLineTo({c.x + cosf(a) * radius, c.y + sinf(a) * radius});
    }
    dl->PathStroke(color, 0, thickness);
}

void statusDot(ImU32 color) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float lh = ImGui::GetTextLineHeight();
    float r = S(5);
    ImGui::Dummy({r * 2 + S(2), lh});
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(p.x + r, p.y + lh * 0.5f);
    glow(dl, c, r * 2.6f, withAlpha(color, 0.5f), 8);
    dl->AddCircleFilled(c, r, color, 16);
}

void helpMarker(const char* text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void textColored(ImU32 c, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextUnformatted(buf);
    ImGui::PopStyleColor();
}

void textWrappedColored(ImU32 c, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

}  // namespace vc2l::ui
