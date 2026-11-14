// Dear ImGui front end for the PSP system dialogs: savedata picker / confirmations and the
// on-screen keyboard. Runs inside host::overlayFrame() on the host thread; communicates with the
// HLE side (savedata.cpp, utility.cpp) only through UiState (screen in, event out).
#include <SDL3/SDL.h>
#include <wincodec.h>
#include <windows.h>

#include <algorithm>
#include <map>

#include "../host/config.h"
#include "../host/gl.h"
#include "../host/host.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "utility.h"

namespace vc2rt::utility {

namespace {

UiState g_ui;
u64 g_nextIconId = 1;
bool g_registered = false;
ImFont* g_font = nullptr;
constexpr float kFontPx = 32.0f;

// ------------------------------------------------------------------ icons (PNG -> GL texture)
struct Tex {
    GLuint id = 0;
    int w = 0, h = 0;
};
std::map<u64, Tex> g_textures;

bool decodePng(const std::vector<u8>& png, std::vector<u8>& rgba, UINT& w, UINT& h) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* f = nullptr;
    IWICStream* st = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE*)png.data(), (DWORD)png.size())) &&
        SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(conv->GetSize(&w, &h)) && w && h && w <= 4096 && h <= 4096) {
        rgba.resize((size_t)w * h * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)rgba.size(), rgba.data()));
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (st) st->Release();
    if (f) f->Release();
    return ok;
}

const Tex* iconTexture(const SlotView& s) {
    if (!s.iconPng || s.iconPng->empty()) return nullptr;
    auto it = g_textures.find(s.iconId);
    if (it != g_textures.end()) return it->second.id ? &it->second : nullptr;
    Tex& t = g_textures[s.iconId];  // a failed decode is remembered as id 0
    std::vector<u8> rgba;
    UINT w = 0, h = 0;
    if (!decodePng(*s.iconPng, rgba, w, h)) return nullptr;
    GLint prevTex = 0, prevAlign = 4, prevRow = 0, prevPbo = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prevRow);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prevPbo);
    if (prevPbo) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, prevRow);
    if (prevPbo) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)prevPbo);
    t.w = (int)w;
    t.h = (int)h;
    return &t;
}

void freeTextures() {
    for (auto& [id, t] : g_textures)
        if (t.id) glDeleteTextures(1, &t.id);
    g_textures.clear();
}

// ------------------------------------------------------------------ input
bool keyPressed(ImGuiKey k, bool repeat = false) { return ImGui::IsKeyPressed(k, repeat); }

bool navUp() {
    return keyPressed(ImGuiKey_UpArrow, true) || keyPressed(ImGuiKey_GamepadDpadUp, true) ||
           keyPressed(ImGuiKey_GamepadLStickUp, true) || keyPressed(ImGuiKey_W, true);
}
bool navDown() {
    return keyPressed(ImGuiKey_DownArrow, true) || keyPressed(ImGuiKey_GamepadDpadDown, true) ||
           keyPressed(ImGuiKey_GamepadLStickDown, true) || keyPressed(ImGuiKey_S, true);
}
bool navLeft() {
    return keyPressed(ImGuiKey_LeftArrow, true) || keyPressed(ImGuiKey_GamepadDpadLeft, true) ||
           keyPressed(ImGuiKey_GamepadLStickLeft, true) || keyPressed(ImGuiKey_A, true);
}
bool navRight() {
    return keyPressed(ImGuiKey_RightArrow, true) || keyPressed(ImGuiKey_GamepadDpadRight, true) ||
           keyPressed(ImGuiKey_GamepadLStickRight, true) || keyPressed(ImGuiKey_D, true);
}
ImGuiKey padConfirm() { return config().swapConfirm ? ImGuiKey_GamepadFaceRight : ImGuiKey_GamepadFaceDown; }
ImGuiKey padCancel() { return config().swapConfirm ? ImGuiKey_GamepadFaceDown : ImGuiKey_GamepadFaceRight; }
// Cross / Enter / Space confirm, Circle / Esc / Backspace cancel (the PC defaults of the game input)
bool confirmPressed(bool allowSpace = true) {
    return keyPressed(ImGuiKey_Enter) || keyPressed(ImGuiKey_KeypadEnter) || (allowSpace && keyPressed(ImGuiKey_Space)) ||
           keyPressed(padConfirm());
}
bool cancelPressed(bool allowBackspace = true) {
    return keyPressed(ImGuiKey_Escape) || (allowBackspace && keyPressed(ImGuiKey_Backspace)) || keyPressed(padCancel());
}

// ------------------------------------------------------------------ drawing helpers
const ImVec4 kAccent(0.38f, 0.62f, 0.95f, 1.0f);
const ImVec4 kDim(0.70f, 0.72f, 0.76f, 1.0f);

void drawIcon(const SlotView& s, ImVec2 size) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (const Tex* t = iconTexture(s)) {
        // fit 144:80 icons (or anything else) into the box, centred
        float sc = std::min(size.x / t->w, size.y / t->h);
        ImVec2 d(t->w * sc, t->h * sc);
        ImVec2 o(p.x + (size.x - d.x) * 0.5f, p.y + (size.y - d.y) * 0.5f);
        dl->AddImage((ImTextureID)(intptr_t)t->id, o, ImVec2(o.x + d.x, o.y + d.y));
    } else {
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(40, 46, 58, 255), 4.0f);
        const char* label = s.exists ? (s.broken ? "?" : "No icon") : "New";
        ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f), IM_COL32(170, 176, 190, 255), label);
    }
    ImGui::Dummy(size);
}

void drawSlotDetails(const SlotView& s, float iconW) {
    drawIcon(s, ImVec2(iconW, iconW * 80.0f / 144.0f));
    ImGui::Spacing();
    if (!s.title.empty()) ImGui::TextColored(kDim, "%s", s.title.c_str());
    ImGui::TextWrapped("%s", s.savedataTitle.empty() ? (s.exists ? "(untitled)" : "New Saved Data") : s.savedataTitle.c_str());
    if (!s.detail.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(s.detail.c_str());
        ImGui::PopTextWrapPos();
    }
    if (s.exists) {
        ImGui::Spacing();
        ImGui::TextColored(kDim, "%s", s.dateText.c_str());
        ImGui::TextColored(kDim, "%s", s.sizeText.c_str());
    }
}

void footer(const char* text) {
    ImGui::Separator();
    ImGui::TextColored(kDim, "%s", text);
}

bool beginDialog(const char* id, ImVec2 frac, float scale) {
    ImGuiIO& io = ImGui::GetIO();
    // dim the game behind the dialog like the PSP does
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
    const float w = std::min(io.DisplaySize.x * frac.x, 1400.0f * scale * 2);
    ImGuiWindowFlags extra = 0;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (frac.y > 0) {
        ImGui::SetNextWindowSize(ImVec2(w, io.DisplaySize.y * frac.y), ImGuiCond_Always);
    } else {  // fixed width, height follows the content
        ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, io.DisplaySize.y * 0.9f));
        extra = ImGuiWindowFlags_AlwaysAutoResize;
    }
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * scale, 14 * scale));
    bool open = ImGui::Begin(id, nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | extra);
    ImGui::PopStyleVar(2);
    if (g_font) ImGui::PushFont(g_font);
    ImGui::SetWindowFontScale(scale);
    return open;
}
void endDialog() {
    if (g_font) ImGui::PopFont();
    ImGui::End();
}

void header(float scale) {
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::SetWindowFontScale(scale * 1.25f);
    ImGui::TextUnformatted(g_ui.heading.c_str());
    ImGui::SetWindowFontScale(scale);
    ImGui::PopStyleColor();
    if (!g_ui.gameTitle.empty()) {
        ImGui::SameLine();
        float w = ImGui::CalcTextSize(g_ui.gameTitle.c_str()).x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 20, ImGui::GetWindowContentRegionMax().x - w));
        ImGui::TextColored(kDim, "%s", g_ui.gameTitle.c_str());
    }
    ImGui::Separator();
    ImGui::Spacing();
}

// ------------------------------------------------------------------ savedata screens
int g_lastScrolledTo = -1;

void drawList(float scale) {
    UiState& u = g_ui;
    const int n = (int)u.slots.size();
    if (n == 0) {
        u.event = UiEvent::Cancel;
        return;
    }
    if (navUp()) u.selected = (u.selected + n - 1) % n;
    if (navDown()) u.selected = (u.selected + 1) % n;
    if (keyPressed(ImGuiKey_PageUp, true) || keyPressed(ImGuiKey_GamepadL1, true)) u.selected = std::max(0, u.selected - 5);
    if (keyPressed(ImGuiKey_PageDown, true) || keyPressed(ImGuiKey_GamepadR1, true)) u.selected = std::min(n - 1, u.selected + 5);
    u.selected = std::clamp(u.selected, 0, n - 1);

    if (!beginDialog("##savedata_list", ImVec2(0.86f, 0.84f), scale)) {
        endDialog();
        return;
    }
    header(scale);
    float footerH = ImGui::GetTextLineHeightWithSpacing() * 1.6f;
    float avail = ImGui::GetContentRegionAvail().y - footerH;
    float listW = ImGui::GetContentRegionAvail().x * 0.56f;
    ImGui::BeginChild("##slots", ImVec2(listW, avail), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    float lineH = ImGui::GetTextLineHeight();
    float rowH = std::max(lineH * 2.4f, 52.0f * scale);
    float iconW = rowH * 144.0f / 80.0f;
    for (int i = 0; i < n; i++) {
        const SlotView& s = u.slots[i];
        ImGui::PushID(i);
        ImVec2 start = ImGui::GetCursorPos();
        bool sel = i == u.selected;
        if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowH + 6 * scale))) {
            u.selected = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) u.event = UiEvent::Select;
        }
        if (sel && g_lastScrolledTo != i) {
            ImGui::SetScrollHereY(0.5f);
            g_lastScrolledTo = i;
        }
        ImGui::SetCursorPos(ImVec2(start.x + 4 * scale, start.y + 3 * scale));
        drawIcon(s, ImVec2(iconW, rowH));
        ImGui::SetCursorPos(ImVec2(start.x + iconW + 14 * scale, start.y + 3 * scale + (rowH - lineH * 2.2f) * 0.5f));
        ImGui::BeginGroup();
        if (!s.exists) ImGui::TextColored(kAccent, "New Saved Data");
        else ImGui::TextUnformatted(s.savedataTitle.empty() ? s.saveName.c_str() : s.savedataTitle.c_str());
        if (s.exists) ImGui::TextColored(kDim, "%s", s.dateText.c_str());
        ImGui::EndGroup();
        ImGui::SetCursorPos(ImVec2(start.x, start.y + rowH + 8 * scale));
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##detail", ImVec2(0, avail), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    drawSlotDetails(u.slots[u.selected], ImGui::GetContentRegionAvail().x);
    ImGui::EndChild();
    footer("Enter / Cross: Select      Esc / Circle: Back      Up / Down: Choose");
    endDialog();

    if (u.event == UiEvent::None) {
        if (confirmPressed()) u.event = UiEvent::Select;
        else if (cancelPressed()) u.event = UiEvent::Cancel;
    }
}

void drawConfirmOrMessage(float scale, bool confirm) {
    UiState& u = g_ui;
    if (confirm) {
        if (navLeft()) u.yes = true;  // [Yes] [No]
        else if (navRight()) u.yes = false;
        else if (navUp() || navDown()) u.yes = !u.yes;
    }
    if (!beginDialog(confirm ? "##savedata_confirm" : "##savedata_msg", ImVec2(0.70f, 0.62f), scale)) {
        endDialog();
        return;
    }
    header(scale);
    float footerH = ImGui::GetTextLineHeightWithSpacing() * 1.6f;
    float avail = ImGui::GetContentRegionAvail().y - footerH;
    bool hasSlot = u.focusSlot >= 0 && u.focusSlot < (int)u.slots.size();
    if (hasSlot) {
        ImGui::BeginChild("##slot", ImVec2(ImGui::GetContentRegionAvail().x * 0.45f, avail), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoNav);
        drawSlotDetails(u.slots[u.focusSlot], ImGui::GetContentRegionAvail().x);
        ImGui::EndChild();
        ImGui::SameLine();
    }
    ImGui::BeginChild("##msg", ImVec2(0, avail), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    float textH = ImGui::GetTextLineHeightWithSpacing() * 4;
    ImGui::SetCursorPosY(std::max(0.0f, (avail - textH) * 0.4f));
    ImGui::TextWrapped("%s", u.message.c_str());
    ImGui::Spacing();
    ImGui::Spacing();
    ImVec2 bsize(ImGui::GetFontSize() * 4.5f, ImGui::GetFontSize() * 1.7f);
    auto button = [&](const char* label, bool highlighted) {
        if (highlighted) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.26f, 0.48f, 0.86f, 1.0f));
        bool clicked = ImGui::Button(label, bsize);
        if (highlighted) ImGui::PopStyleColor();
        return clicked;
    };
    if (confirm) {
        if (button("Yes", u.yes)) u.event = UiEvent::Yes;
        ImGui::SameLine();
        if (button("No", !u.yes)) u.event = UiEvent::No;
    } else {
        if (button("OK", true)) u.event = UiEvent::Ok;
    }
    ImGui::EndChild();
    footer(confirm ? "Left / Right: Choose      Enter / Cross: OK      Esc / Circle: Back" : "Enter / Cross: OK");
    endDialog();

    if (u.event == UiEvent::None) {
        if (confirm) {
            if (confirmPressed()) u.event = u.yes ? UiEvent::Yes : UiEvent::No;
            else if (cancelPressed()) u.event = UiEvent::Cancel;
        } else if (confirmPressed() || cancelPressed()) {
            u.event = UiEvent::Ok;
        }
    }
}

// ------------------------------------------------------------------ on-screen keyboard
std::vector<char> g_oskBuf;
bool g_oskFocus = false;
bool g_oskCaretToEnd = false;

size_t utf16Units(const char* s, size_t n) {
    size_t units = 0;
    for (size_t i = 0; i < n;) {
        u8 c = (u8)s[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        units += len == 4 ? 2 : 1;
        i += len;
    }
    return units;
}

bool oskCharAllowed(ImWchar ch, u32 type) {
    // PSP_UTILITY_OSK_INPUTTYPE_*: 0x1/0x100 digits, 0x2/0x200 symbols, 0x4/0x400 lower, 0x8/0x800 upper
    const u32 latinMask = 0x0F0F;
    if (type == 0 || (type & ~latinMask)) return true;  // "all" or a non-latin keyboard: no filtering
    bool digit = ch >= '0' && ch <= '9', lower = ch >= 'a' && ch <= 'z', upper = ch >= 'A' && ch <= 'Z';
    if (digit) return type & 0x0101;
    if (lower) return type & 0x0404;
    if (upper) return type & 0x0808;
    if (ch == ' ') return (type & 0x0E0E) != 0;
    return (type & 0x0202) != 0;  // symbols and everything else
}

int oskCallback(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        if (g_oskCaretToEnd) {  // keep the initial text and append to it, like the PSP keyboard
            d->ClearSelection();
            d->CursorPos = d->BufTextLen;
            g_oskCaretToEnd = false;
        }
        return 0;
    }
    if (d->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        if (d->EventChar < 0x20 && d->EventChar != '\n') return 1;
        return oskCharAllowed(d->EventChar, g_ui.oskInputType) ? 0 : 1;
    }
    if (d->EventFlag == ImGuiInputTextFlags_CallbackEdit) {
        // enforce the field limit in UTF-16 units (what the game's buffer holds)
        while (d->BufTextLen > 0 && utf16Units(d->Buf, (size_t)d->BufTextLen) > g_ui.oskMaxChars) {
            int end = d->BufTextLen, start = end - 1;
            while (start > 0 && ((u8)d->Buf[start] & 0xC0) == 0x80) start--;
            d->DeleteChars(start, end - start);
        }
    }
    return 0;
}

void drawOsk(float scale) {
    UiState& u = g_ui;
    if (!beginDialog("##osk", ImVec2(0.70f, 0.0f), scale)) {
        endDialog();
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextWrapped("%s", u.heading.empty() ? "Enter text" : u.heading.c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();
    if (g_oskFocus) {
        ImGui::SetKeyboardFocusHere();
        g_oskFocus = false;
    }
    ImGuiInputTextFlags fl = ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_CallbackEdit |
                             ImGuiInputTextFlags_CallbackAlways |
                             ImGuiInputTextFlags_EnterReturnsTrue;
    bool entered;
    ImGui::PushItemWidth(-1);
    if (u.oskLines > 1) {
        float h = ImGui::GetTextLineHeight() * std::min(u.oskLines, 6) + ImGui::GetStyle().FramePadding.y * 2;
        entered = ImGui::InputTextMultiline("##text", g_oskBuf.data(), g_oskBuf.size(), ImVec2(-1, h),
                                            fl | ImGuiInputTextFlags_CtrlEnterForNewLine, oskCallback);
    } else {
        entered = ImGui::InputText("##text", g_oskBuf.data(), g_oskBuf.size(), fl, oskCallback);
    }
    ImGui::PopItemWidth();
    size_t units = utf16Units(g_oskBuf.data(), strlen(g_oskBuf.data()));
    ImGui::TextColored(kDim, "%zu / %u", units, u.oskMaxChars);
    ImGui::Spacing();
    ImVec2 bsize(ImGui::GetFontSize() * 5.0f, ImGui::GetFontSize() * 1.6f);
    bool ok = ImGui::Button("OK", bsize);
    ImGui::SameLine();
    bool cancel = ImGui::Button("Cancel", bsize);
    footer("Type with the keyboard.   Enter / Start: Done      Esc / Circle: Cancel");
    endDialog();

    if (entered || ok || keyPressed(ImGuiKey_GamepadStart) || keyPressed(padConfirm())) {
        u.oskText = g_oskBuf.data();
        u.event = UiEvent::Ok;
    } else if (cancel || keyPressed(ImGuiKey_Escape) || keyPressed(padCancel())) {
        u.event = UiEvent::Cancel;
    }
}

// ------------------------------------------------------------------ overlay hook
UiScreen g_prevScreen = UiScreen::Hidden;
// After a dialog closes, keep the game's input withheld until the keys that closed it are released;
// otherwise the game (whose pad read zero while the dialog was up) would see a fresh Cross press.
bool g_waitRelease = false;

bool anyDialogKeyDown() {
    static const ImGuiKey keys[] = {ImGuiKey_Enter,           ImGuiKey_KeypadEnter,       ImGuiKey_Space,
                                    ImGuiKey_Escape,          ImGuiKey_Backspace,         ImGuiKey_GamepadFaceDown,
                                    ImGuiKey_GamepadFaceRight, ImGuiKey_GamepadFaceLeft,   ImGuiKey_GamepadFaceUp,
                                    ImGuiKey_GamepadStart,    ImGuiKey_GamepadDpadUp,     ImGuiKey_GamepadDpadDown,
                                    ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,  ImGuiKey_UpArrow,
                                    ImGuiKey_DownArrow,       ImGuiKey_LeftArrow,         ImGuiKey_RightArrow};
    for (ImGuiKey k : keys)
        if (ImGui::IsKeyDown(k)) return true;
    return false;
}

bool wantsInput() {
    if (g_ui.screen != UiScreen::Hidden) return true;
    if (g_waitRelease && !anyDialogKeyDown()) g_waitRelease = false;
    return g_waitRelease;
}

void draw() {
    UiState& u = g_ui;
    if (u.screen != g_prevScreen) {
        if (u.screen == UiScreen::Osk) {
            g_oskBuf.assign(std::max<size_t>(u.oskMaxChars * 4 + 8, 64), 0);
            memcpy(g_oskBuf.data(), u.oskText.data(), std::min(u.oskText.size(), g_oskBuf.size() - 1));
            g_oskFocus = true;
            g_oskCaretToEnd = true;
            if (host::window()) SDL_StartTextInput(host::window());
        } else if (g_prevScreen == UiScreen::Osk && host::window()) {
            SDL_StopTextInput(host::window());
        }
        if (u.screen == UiScreen::Hidden) {
            freeTextures();
            g_waitRelease = true;
        }
        if (u.screen == UiScreen::List) g_lastScrolledTo = -1;
        g_prevScreen = u.screen;
    }
    if (u.screen == UiScreen::Hidden) return;
    // ~26px text at 1080p, never below 16px
    float px = std::clamp(ImGui::GetIO().DisplaySize.y * 26.0f / 1080.0f, 16.0f, 56.0f);
    float scale = g_font ? px / kFontPx : std::clamp(ImGui::GetIO().DisplaySize.y / 540.0f, 1.0f, 3.0f);
    // While a choice is waiting for the game's next Update call, keep drawing but keep that choice.
    UiEvent before = u.event;
    switch (u.screen) {
        case UiScreen::List: drawList(scale); break;
        case UiScreen::Confirm: drawConfirmOrMessage(scale, true); break;
        case UiScreen::Message: drawConfirmOrMessage(scale, false); break;
        case UiScreen::Osk: drawOsk(scale); break;
        default: break;
    }
    if (before != UiEvent::None) u.event = before;  // keep the first choice until consumed
    if (u.event != UiEvent::None) g_waitRelease = true;  // the dialog may close on this press
}

void loadUiFont() {
    // A proper TTF (Segoe UI) for the dialogs; the atlas may only change outside a frame, which is
    // the case here (called from an HLE call, not from inside overlayFrame()).
    ImGuiIO& io = ImGui::GetIO();
    const char* candidates[] = {"C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf"};
    wchar_t winDir[MAX_PATH];
    std::string fontsDir = "C:\\Windows\\Fonts\\";
    if (GetWindowsDirectoryW(winDir, MAX_PATH))
        fontsDir = utf16ToUtf8(std::u16string((char16_t*)winDir)) + "\\Fonts\\";
    static const ImWchar ranges[] = {0x0020, 0x024F, 0x2000, 0x206F, 0x20A0, 0x20CF, 0x2100, 0x214F, 0x2190, 0x21FF, 0};
    for (const char* c : candidates) {
        std::string path = fontsDir + (strrchr(c, '\\') + 1);
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        g_font = io.Fonts->AddFontFromFileTTF(path.c_str(), kFontPx, &cfg, ranges);
        if (g_font) break;
    }
    if (g_font) {
        ImGui_ImplOpenGL3_DestroyFontsTexture();
        ImGui_ImplOpenGL3_CreateFontsTexture();
    }
}

}  // namespace

UiState& ui() { return g_ui; }
u64 uiNewIconId() { return g_nextIconId++; }

void uiEnsureRegistered() {
    if (g_registered || !host::window() || !ImGui::GetCurrentContext()) return;
    g_registered = true;
    loadUiFont();
    // The game reads every connected pad; let the dialogs be driven from any of them too (the SDL3
    // backend defaults to the first pad only).
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);
    host::addOverlayDrawer(draw, wantsInput);
    LOG_INFO("UTIL", "system dialog overlay registered (font: %s)", g_font ? "TTF" : "default");
}

}  // namespace vc2rt::utility
