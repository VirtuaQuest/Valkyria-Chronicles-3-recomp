#pragma once
// Launcher look & feel: theme, fonts, procedural background art and custom widgets (Dear ImGui draw lists).
#include <imgui.h>

#include <string>

namespace vc2l::ui {

struct Fonts {
    ImFont* body = nullptr;
    ImFont* bold = nullptr;
    ImFont* title = nullptr;
    ImFont* heading = nullptr;
    ImFont* caption = nullptr;
    ImFont* mono = nullptr;
};
Fonts& fonts();
float scale();  // current DPI scale

// Loads system UI fonts (Segoe UI family) at the given DPI scale and applies the theme.
void setup(float dpiScale);
inline float S(float v) { return v * scale(); }

namespace col {
constexpr ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
inline constexpr ImU32 bgTop = IM_COL32(16, 19, 44, 255);
inline constexpr ImU32 bgBottom = IM_COL32(30, 18, 58, 255);
inline constexpr ImU32 card = IM_COL32(26, 30, 66, 214);
inline constexpr ImU32 cardBorder = IM_COL32(255, 255, 255, 18);
inline constexpr ImU32 text = IM_COL32(233, 236, 255, 255);
inline constexpr ImU32 muted = IM_COL32(150, 160, 200, 255);
inline constexpr ImU32 cyan = IM_COL32(54, 209, 255, 255);
inline constexpr ImU32 violet = IM_COL32(139, 108, 255, 255);
inline constexpr ImU32 magenta = IM_COL32(255, 79, 163, 255);
inline constexpr ImU32 amber = IM_COL32(255, 181, 71, 255);
inline constexpr ImU32 coral = IM_COL32(255, 111, 97, 255);
inline constexpr ImU32 green = IM_COL32(61, 220, 151, 255);
inline constexpr ImU32 red = IM_COL32(255, 93, 108, 255);
}  // namespace col

ImU32 withAlpha(ImU32 c, float a);
ImU32 lerpColor(ImU32 a, ImU32 b, float t);
ImVec4 toVec4(ImU32 c);

// Fills a rounded rectangle with a linear gradient from c0 (at p0) to c1 (at p1).
void gradientRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c0, ImU32 c1, float rounding, bool horizontal = true,
                  ImDrawFlags flags = 0);
void glow(ImDrawList* dl, ImVec2 center, float radius, ImU32 color, int rings = 28);
void drawGear(ImDrawList* dl, ImVec2 c, float r, int teeth, float rot, ImU32 color, float thickness);

void drawBackground(float time);

// Card = rounded translucent panel with a coloured accent strip and an optional title.
bool beginCard(const char* id, ImVec2 size, ImU32 accent, const char* title, const char* icon = nullptr);
void endCard();

bool gradientButton(const char* label, ImVec2 size, ImU32 c0, ImU32 c1, bool enabled = true, ImFont* font = nullptr);
bool pillButton(const char* label, bool selected, ImVec2 size);
void progressBar(float frac, ImVec2 size, bool animate, float time, const char* overlay);
void badge(const char* text, ImU32 color);
void keycap(const char* text);
void spinner(float radius, float thickness, ImU32 color, float time);
void statusDot(ImU32 color);
void helpMarker(const char* text);
void textColored(ImU32 c, const char* fmt, ...);
void textWrappedColored(ImU32 c, const char* text);

}  // namespace vc2l::ui
