#include "settings_ui.h"

#include <SDL3/SDL_keyboard.h>
#include <imgui.h>

#include <algorithm>
#include <cstring>

#include "ui.h"
#include "util.h"

namespace vc2l {

using namespace ui;

namespace {

// Must match the binding names and defaults in src/runtime/host/host.cpp (kBinds / kStick).
struct BindInfo {
    const char* name;
    const char* label;
    const char* defaults;
};
const BindInfo kBindings[] = {
    {"Cross", "Cross (confirm)", "Space,Return"},
    {"Circle", "Circle (cancel)", "Escape,Backspace"},
    {"Square", "Square", "F"},
    {"Triangle", "Triangle", "R"},
    {"L", "L trigger", "Q"},
    {"R", "R trigger", "E"},
    {"Start", "Start", "Tab"},
    {"Select", "Select", "C"},
    {"Up", "D-pad up", "Up"},
    {"Down", "D-pad down", "Down"},
    {"Left", "D-pad left", "Left"},
    {"Right", "D-pad right", "Right"},
    {"StickUp", "Analog up", "W"},
    {"StickDown", "Analog down", "S"},
    {"StickLeft", "Analog left", "A"},
    {"StickRight", "Analog right", "D"},
};

void fillDefaultBindings(vc2rt::Config& c) {
    for (auto& b : kBindings)
        if (!c.keyBindings.count(b.name)) c.keyBindings[b.name] = b.defaults;
}

bool sameConfig(const vc2rt::Config& a, const vc2rt::Config& b) {
    return a.windowMode == b.windowMode && a.windowWidth == b.windowWidth && a.windowHeight == b.windowHeight &&
           a.vsync == b.vsync && a.internalScale == b.internalScale && a.aspect == b.aspect &&
           a.textureFilter == b.textureFilter && a.anisotropy == b.anisotropy && a.msaa == b.msaa &&
           a.textureUpscale == b.textureUpscale && a.fxaa == b.fxaa && a.showFps == b.showFps &&
           a.frameLimit == b.frameLimit && a.masterVolume == b.masterVolume && a.audioLatencyMs == b.audioLatencyMs &&
           a.stickDeadzone == b.stickDeadzone && a.swapConfirm == b.swapConfirm && a.keyBindings == b.keyBindings &&
           a.logHle == b.logHle;
}

std::vector<std::string> splitKeys(const std::string& s) {
    std::vector<std::string> v;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) v.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return v;
}

// label column + full-width widget
void rowLabel(const char* label, const char* help = nullptr) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (help) {
        ImGui::SameLine();
        helpMarker(help);
    }
    ImGui::SameLine(S(190));
    ImGui::SetNextItemWidth(-1);
}

bool comboInt(const char* id, int* v, const int* values, const char* const* names, int n) {
    int cur = 0;
    for (int i = 0; i < n; i++)
        if (values[i] == *v) cur = i;
    bool changed = false;
    if (ImGui::BeginCombo(id, names[cur])) {
        for (int i = 0; i < n; i++) {
            bool sel = i == cur;
            if (ImGui::Selectable(names[i], sel)) {
                *v = values[i];
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

}  // namespace

void SettingsState::load(const std::filesystem::path& iniFile) {
    file = iniFile;
    cfg = vc2rt::Config{};
    std::error_code ec;
    existsOnDisk = std::filesystem::is_regular_file(file, ec);
    if (existsOnDisk) cfg.load(file);
    fillDefaultBindings(cfg);
    saved = cfg;
    loaded = true;
    captureKey.clear();
}

bool SettingsState::save() {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    bool ok = cfg.save(file);
    if (ok) {
        saved = cfg;
        existsOnDisk = true;
    }
    return ok;
}

bool SettingsState::dirty() const { return !sameConfig(cfg, saved); }

bool settingsHandleKey(SettingsState& s, SDL_Scancode sc, float time) {
    if (s.captureKey.empty()) return false;
    const char* name = SDL_GetScancodeName(sc);
    if (!name || !*name) return true;
    std::string& v = s.cfg.keyBindings[s.captureKey];
    if (s.captureAppend) {
        auto keys = splitKeys(v);
        if (std::find(keys.begin(), keys.end(), name) == keys.end()) v = v.empty() ? name : v + "," + name;
    } else {
        v = name;
    }
    s.message = s.captureKey + " bound to " + v;
    s.messageError = false;
    s.messageTime = time;
    s.captureKey.clear();
    return true;
}

void drawSettingsTab(SettingsState& s, float time) {
    vc2rt::Config& c = s.cfg;
    const float gap = ImGui::GetStyle().ItemSpacing.x * 1.6f;
    const float barH = S(64);
    ImVec2 avail = ImGui::GetContentRegionAvail();

    ImGui::BeginChild("settingsScroll", ImVec2(0, avail.y - barH - S(8)), 0, 0);
    float colW = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
    float topH = S(266);

    // ---------------------------------------------------------------- display
    if (beginCard("display", ImVec2(colW, topH), col::cyan, "Display", "\xe2\x96\xa3")) {
        rowLabel("Window mode");
        int wm = (int)c.windowMode;
        const char* modes[] = {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"};
        if (ImGui::Combo("##wm", &wm, modes, 3)) c.windowMode = (vc2rt::WindowMode)wm;

        rowLabel("Window size", "Initial size of the game window in windowed mode.");
        static const int presets[][2] = {{960, 544}, {1280, 720}, {1440, 816}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
        char cur[64];
        snprintf(cur, sizeof cur, "%d x %d", c.windowWidth, c.windowHeight);
        float half = (ImGui::GetContentRegionAvail().x);
        ImGui::SetNextItemWidth(half * 0.46f);
        if (ImGui::BeginCombo("##res", cur)) {
            for (auto& p : presets) {
                char b[64];
                snprintf(b, sizeof b, "%d x %d", p[0], p[1]);
                if (ImGui::Selectable(b, p[0] == c.windowWidth && p[1] == c.windowHeight)) {
                    c.windowWidth = p[0];
                    c.windowHeight = p[1];
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(half * 0.25f);
        if (ImGui::InputInt("##ww", &c.windowWidth, 0)) c.windowWidth = std::clamp(c.windowWidth, 480, 7680);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputInt("##wh", &c.windowHeight, 0)) c.windowHeight = std::clamp(c.windowHeight, 272, 4320);

        rowLabel("Aspect ratio");
        int asp = (int)c.aspect;
        const char* aspects[] = {"Keep 16:9 (letterbox)", "Stretch to window", "Integer scale (pixel perfect)"};
        if (ImGui::Combo("##asp", &asp, aspects, 3)) c.aspect = (vc2rt::AspectMode)asp;

        rowLabel("Frame limit", "0 = unlimited. The game logic is designed for 60 FPS.");
        ImGui::SliderInt("##fl", &c.frameLimit, 0, 240, c.frameLimit == 0 ? "Unlimited" : "%d FPS");
        rowLabel("Vertical sync");
        ImGui::Checkbox("##vs", &c.vsync);
        ImGui::SameLine(S(190) + S(130));
        ImGui::TextUnformatted("Show FPS");
        ImGui::SameLine();
        ImGui::Checkbox("##fps", &c.showFps);
    }
    endCard();
    ImGui::SameLine(0, gap);

    // ---------------------------------------------------------------- graphics
    if (beginCard("graphics", ImVec2(colW, topH), col::magenta, "Graphics", "\xe2\x9c\xa6")) {
        rowLabel("Internal resolution", "Renders the 3D scene at a multiple of the PSP's native 480x272.");
        ImGui::SliderInt("##scale", &c.internalScale, 1, 8, "%dx");
        ImGui::Dummy({S(180), 0});
        ImGui::SameLine(S(190));
        char rb[96];
        int rw = 480 * c.internalScale, rh = 272 * c.internalScale;
        snprintf(rb, sizeof rb, "%d x %d", rw, rh);
        badge(rb, c.internalScale >= 6 ? col::amber : col::cyan);
        ImGui::SameLine();
        const char* hint = c.internalScale == 1 ? "native PSP" : c.internalScale <= 2 ? "~qHD" : c.internalScale <= 3 ? "~HD+" :
                           c.internalScale <= 4 ? "~1080p" : c.internalScale <= 6 ? "~1440p+" : "~4K+, needs a fast GPU";
        ImGui::AlignTextToFramePadding();
        textColored(col::muted, "%s", hint);

        rowLabel("Texture filtering");
        int tf = (int)c.textureFilter;
        const char* filters[] = {"Nearest (sharp pixels)", "Linear (smooth)"};
        if (ImGui::Combo("##tf", &tf, filters, 2)) c.textureFilter = (vc2rt::TexFilter)tf;

        static const int anisoV[] = {1, 2, 4, 8, 16};
        static const char* anisoN[] = {"Off", "2x", "4x", "8x", "16x"};
        rowLabel("Anisotropic filtering");
        comboInt("##an", &c.anisotropy, anisoV, anisoN, 5);

        static const int msaaV[] = {0, 2, 4, 8};
        static const char* msaaN[] = {"Off", "2x MSAA", "4x MSAA", "8x MSAA"};
        rowLabel("Anti-aliasing");
        float hw = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(hw * 0.55f);
        comboInt("##msaa", &c.msaa, msaaV, msaaN, 4);
        ImGui::SameLine();
        ImGui::Checkbox("FXAA", &c.fxaa);

        static const int upV[] = {1, 2, 3, 4};
        static const char* upN[] = {"Off", "2x smooth", "3x smooth", "4x smooth"};
        rowLabel("Texture upscaling", "Smooths the PSP's low-resolution textures. Costs VRAM and load time.");
        comboInt("##up", &c.textureUpscale, upV, upN, 4);
    }
    endCard();

    ImGui::Dummy({0, S(4)});
    // fill the remaining height (scrolls only when the window is too small for the minimum layout)
    float lowH = std::max(S(360), ImGui::GetContentRegionAvail().y - S(2));
    // ---------------------------------------------------------------- audio + input
    ImGui::BeginGroup();
    if (beginCard("audio", ImVec2(colW, S(150)), col::amber, "Audio", "\xe2\x99\xab")) {
        rowLabel("Master volume");
        ImGui::SliderInt("##vol", &c.masterVolume, 0, 100, c.masterVolume == 0 ? "Muted" : "%d%%");
        rowLabel("Latency", "Lower is more responsive; raise it if you hear crackling.");
        ImGui::SliderInt("##lat", &c.audioLatencyMs, 16, 500, "%d ms");
    }
    endCard();
    ImGui::Dummy({0, S(4)});
    if (beginCard("input", ImVec2(colW, lowH - S(150) - S(12)), col::green, "Controller", "\xe2\x97\x8e")) {
        rowLabel("Stick deadzone", "Applies to the left analog stick of game controllers.");
        ImGui::SliderInt("##dz", &c.stickDeadzone, 0, 90, "%d%%");
        rowLabel("Swap confirm / cancel", "Use the right face button (Circle position) to confirm, like Japanese releases.");
        ImGui::Checkbox("##swap", &c.swapConfirm);
        ImGui::Dummy({0, S(4)});
        textWrappedColored(col::muted,
                           "Game controllers are detected automatically (Xbox, PlayStation and SDL-compatible pads). "
                           "Hold Shift while moving with the keyboard to walk slowly; hold ` (grave) to fast-forward.");
        ImGui::Dummy({0, S(2)});
        ImGui::Checkbox("Verbose HLE log (debug)", &c.logHle);
    }
    endCard();
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);

    // ---------------------------------------------------------------- key bindings
    if (beginCard("keys", ImVec2(colW, lowH), col::violet, "Keyboard bindings", "\xe2\x8c\xa8")) {
        if (ImGui::BeginTable("binds", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH,
                              ImVec2(0, ImGui::GetContentRegionAvail().y))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("PSP control", ImGuiTableColumnFlags_WidthFixed, S(130));
            ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, S(150));
            ImGui::TableHeadersRow();
            for (auto& b : kBindings) {
                ImGui::PushID(b.name);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(b.label);
                ImGui::TableNextColumn();
                auto keys = splitKeys(c.keyBindings[b.name]);
                if (s.captureKey == b.name) {
                    ImGui::AlignTextToFramePadding();
                    float pulse = 0.6f + 0.4f * sinf(time * 6);
                    textColored(withAlpha(col::cyan, pulse), s.captureAppend ? "press a key to add..." : "press a key...");
                } else if (keys.empty()) {
                    ImGui::AlignTextToFramePadding();
                    textColored(col::red, "unbound");
                } else {
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(3));
                    for (size_t i = 0; i < keys.size(); i++) {
                        if (i) ImGui::SameLine(0, S(4));
                        keycap(keys[i].c_str());
                    }
                }
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Set")) {
                    s.captureKey = b.name;
                    s.captureAppend = false;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Add")) {
                    s.captureKey = b.name;
                    s.captureAppend = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset")) c.keyBindings[b.name] = b.defaults;
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    endCard();
    ImGui::EndChild();

    // ---------------------------------------------------------------- action bar
    ImGui::Dummy({0, S(2)});
    if (beginCard("settingsBar", ImVec2(0, barH), col::violet, nullptr)) {
        ImGui::AlignTextToFramePadding();
        bool dirty = s.dirty();
        if (dirty) {
            statusDot(col::amber);
            ImGui::SameLine();
            textColored(col::amber, "Unsaved changes");
        } else {
            statusDot(s.existsOnDisk ? col::green : col::muted);
            ImGui::SameLine();
            textColored(col::muted, s.existsOnDisk ? "Saved" : "Defaults (file not created yet)");
        }
        float bw = S(120), bh = S(34);
        float buttonsX = ImGui::GetContentRegionMax().x - bw * 3 - S(20);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (!s.message.empty() && time - s.messageTime < 4.0) {
            textColored(s.messageError ? col::red : col::green, "  %s", s.message.c_str());
        } else {
            // the settings file path, shortened from the left so it never runs into the buttons
            ImGui::PushFont(fonts().caption);
            std::string p = pathUtf8(s.file);
            float room = buttonsX - ImGui::GetCursorPosX() - S(24);
            bool cut = false;
            while (p.size() > 8 && ImGui::CalcTextSize(p.c_str()).x > room) {
                p.erase(0, 4);
                cut = true;
            }
            textColored(col::muted, "  %s%s", cut ? "..." : "", p.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pathUtf8(s.file).c_str());
            ImGui::PopFont();
        }
        ImGui::SameLine(buttonsX);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(2));
        if (ImGui::Button("Defaults", ImVec2(bw, bh))) {
            c = vc2rt::Config{};
            fillDefaultBindings(c);
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert", ImVec2(bw, bh))) c = s.saved;
        ImGui::SameLine();
        if (gradientButton("Save", ImVec2(bw, bh), col::violet, col::magenta, dirty || !s.existsOnDisk)) {
            bool ok = s.save();
            s.message = ok ? "Settings saved" : "Could not write the settings file";
            s.messageError = !ok;
            s.messageTime = time;
        }
    }
    endCard();

    if (!s.captureKey.empty()) {
        ImGui::OpenPopup("Bind key");
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(26), S(20)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(14));
    if (ImGui::BeginPopupModal("Bind key", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        if (s.captureKey.empty()) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::PushFont(fonts().heading);
            ImGui::Text("%s a key for %s", s.captureAppend ? "Add" : "Press", s.captureKey.c_str());
            ImGui::PopFont();
            textColored(col::muted, "Any key works, including Escape. Current: %s", c.keyBindings[s.captureKey].c_str());
            ImGui::Dummy({0, S(6)});
            spinner(S(8), S(2.5f), col::cyan, time);
            ImGui::SameLine();
            ImGui::TextUnformatted("Waiting for input...");
            ImGui::SameLine(ImGui::GetContentRegionMax().x - S(100));
            if (ImGui::Button("Cancel", ImVec2(S(100), 0))) s.captureKey.clear();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

}  // namespace vc2l
