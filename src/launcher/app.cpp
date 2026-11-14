// The launcher GUI: SDL3 window + OpenGL3 + Dear ImGui.
#include "app.h"

#include <windows.h>
#include <shobjidl.h>
#include <GL/gl.h>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

#include "builder.h"
#include "common/build_info.h"
#include "disc.h"
#include "imaging.h"
#include "settings_ui.h"
#include "toolchain.h"
#include "ui.h"
#include "util.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5

namespace vc2l {

using namespace ui;

// ------------------------------------------------------------------ prefs
static fs::path prefsFileExe() { return launcherDir() / "launcher.ini"; }
static fs::path prefsFileFallback() { return defaultInstallDir() / "launcher.ini"; }

LauncherPrefs loadPrefs() {
    LauncherPrefs p;
    std::error_code ec;
    fs::path f = fs::is_regular_file(prefsFileExe(), ec) ? prefsFileExe() : prefsFileFallback();
    auto kv = readKeyValueFile(f);
    if (kv.count("IsoPath")) p.iso = pathFromUtf8(kv["IsoPath"]);
    if (kv.count("OutDir")) p.outDir = pathFromUtf8(kv["OutDir"]);
    if (kv.count("LogOpen")) p.logOpen = kv["LogOpen"] != "0";
    if (kv.count("Jobs")) p.jobs = atoi(kv["Jobs"].c_str());
    return p;
}

bool savePrefs(const LauncherPrefs& p) {
    std::map<std::string, std::string> kv;
    kv["IsoPath"] = pathUtf8(p.iso);
    kv["OutDir"] = pathUtf8(p.outDir);
    kv["LogOpen"] = p.logOpen ? "1" : "0";
    kv["Jobs"] = std::to_string(p.jobs);
    const char* hdr = "VC3 Recompiled launcher - your choices (UTF-8)";
    if (writeKeyValueFile(prefsFileExe(), kv, hdr)) return true;
    return writeKeyValueFile(prefsFileFallback(), kv, hdr);
}

namespace {

// ------------------------------------------------------------------ GL helpers
using PFN_GenFramebuffers = void(APIENTRY*)(GLsizei, GLuint*);
using PFN_BindFramebuffer = void(APIENTRY*)(GLenum, GLuint);
using PFN_FramebufferTexture2D = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFN_CheckFramebufferStatus = GLenum(APIENTRY*)(GLenum);
using PFN_DeleteFramebuffers = void(APIENTRY*)(GLsizei, const GLuint*);

GLuint makeTexture(const Image& img, bool smooth) {
    if (img.empty()) return 0;
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.w, img.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
    return t;
}

ImTextureID texId(GLuint t) { return (ImTextureID)(intptr_t)t; }

// ------------------------------------------------------------------ native dialogs
bool pickPath(HWND owner, bool folder, const fs::path& initial, fs::path& out) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return false;
    DWORD opts = 0;
    d->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (folder) {
        opts |= FOS_PICKFOLDERS;
        d->SetTitle(L"Choose the install folder for the recompiled game");
        d->SetOkButtonLabel(L"Use this folder");
    } else {
        opts |= FOS_FILEMUSTEXIST;
        COMDLG_FILTERSPEC f[] = {{L"PSP disc images (*.iso)", L"*.iso"}, {L"All files (*.*)", L"*.*"}};
        d->SetFileTypes(2, f);
        d->SetFileTypeIndex(1);
        d->SetDefaultExtension(L"iso");
        d->SetTitle(L"Select your game disc image (.iso)");
    }
    d->SetOptions(opts);
    std::error_code ec;
    fs::path start = initial;
    if (!start.empty() && !fs::is_directory(start, ec)) start = start.parent_path();
    if (!start.empty() && fs::is_directory(start, ec)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            d->SetFolder(item);
            item->Release();
        }
    }
    bool ok = false;
    if (SUCCEEDED(d->Show(owner))) {
        IShellItem* r = nullptr;
        if (SUCCEEDED(d->GetResult(&r))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(r->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                out = p;
                ok = true;
                CoTaskMemFree(p);
            }
            r->Release();
        }
    }
    d->Release();
    return ok;
}

std::string freeSpaceText(const fs::path& dir) {
    fs::path p = dir;
    std::error_code ec;
    while (!p.empty() && !fs::exists(p, ec) && p != p.parent_path()) p = p.parent_path();
    ULARGE_INTEGER freeBytes{};
    if (p.empty() || !GetDiskFreeSpaceExW(p.c_str(), &freeBytes, nullptr, nullptr)) return {};
    return formatBytes(freeBytes.QuadPart) + " free";
}

ImVec2 operator+(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }
ImVec2 operator-(ImVec2 a, ImVec2 b) { return {a.x - b.x, a.y - b.y}; }
ImVec2 operator*(ImVec2 a, float s) { return {a.x * s, a.y * s}; }

const Stage kChipStages[] = {Stage::Toolchain, Stage::Validate, Stage::Decrypt, Stage::Translate,
                             Stage::Compile,   Stage::Resource, Stage::Link,    Stage::Install};
const char* kChipNames[] = {"MSVC", "Validate", "Load", "Translate", "Compile", "Icon", "Link", "Install"};

// ------------------------------------------------------------------ the app
class App {
public:
    explicit App(const GuiOptions& o) : opt_(o) {}
    int run();

private:
    void setIso(const fs::path& p);
    void setOut(const fs::path& p);
    void persist();
    void startBuild();
    void rebuildFontsIfNeeded();
    void frame();
    void drawHeader();
    void drawInstallTab();
    void drawDiscCard(ImVec2 size);
    void drawToolchainCard(ImVec2 size);
    void drawBuildCard(ImVec2 size);
    void drawAboutTab();
    void drawStageChips(const BuildStatus& st);
    void drawLog(float height);
    bool saveScreenshot(const fs::path& file, ImDrawData* dd);
    HWND hwnd() const;

    GuiOptions opt_;
    SDL_Window* win_ = nullptr;
    SDL_GLContext gl_ = nullptr;
    float time_ = 0;
    float dpi_ = 1.0f, pendingDpi_ = 0;
    int tab_ = 0;

    LauncherPrefs prefs_;
    char isoBuf_[2048] = {}, outBuf_[2048] = {};
    DiscInfo disc_;
    GLuint discTex_ = 0;
    int discW_ = 0, discH_ = 0;
    GLuint emblemTex_ = 0;

    Builder builder_;
    std::vector<LogLine> log_;
    size_t logEnd_ = 0;
    bool forceRebuild_ = false;
    bool wasRunning_ = false;

    SettingsState settings_;
    std::string toast_;
    bool toastError_ = false;
    float toastTime_ = -100;
};

HWND App::hwnd() const {
    return (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(win_), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
}

void App::persist() {
    prefs_.iso = pathFromUtf8(isoBuf_);
    prefs_.outDir = pathFromUtf8(outBuf_);
    savePrefs(prefs_);
}

void App::setIso(const fs::path& p) {
    snprintf(isoBuf_, sizeof isoBuf_, "%s", pathUtf8(p).c_str());
    disc_ = p.empty() ? DiscInfo{} : probeDisc(p);
    setGameExeName(disc_.exeName);
    if (p.empty()) disc_.error.clear();
    if (discTex_) glDeleteTextures(1, &discTex_);
    discTex_ = 0;
    Image icon;
    if (!disc_.iconPng.empty() && decodeImage(disc_.iconPng.data(), disc_.iconPng.size(), icon)) {
        discTex_ = makeTexture(icon, true);
        discW_ = icon.w;
        discH_ = icon.h;
    }
    persist();
}

void App::setOut(const fs::path& p) {
    snprintf(outBuf_, sizeof outBuf_, "%s", pathUtf8(p).c_str());
    settings_.load(p / "vc3recomp.ini");
    persist();
}

void App::startBuild() {
    BuildOptions bo;
    bo.iso = pathFromUtf8(isoBuf_);
    bo.outDir = pathFromUtf8(outBuf_);
    bo.jobs = prefs_.jobs;
    bo.forceRebuild = forceRebuild_;
    persist();
    if (builder_.start(bo)) {
        log_.clear();
        logEnd_ = 0;
        wasRunning_ = true;
    }
}

void App::rebuildFontsIfNeeded() {
    if (pendingDpi_ <= 0) return;
    dpi_ = pendingDpi_;
    pendingDpi_ = 0;
    ui::setup(dpi_);
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    ImGui_ImplOpenGL3_CreateFontsTexture();
}

// ------------------------------------------------------------------ header
void App::drawHeader() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    float w = ImGui::GetWindowWidth();
    float hh = S(112);
    // emblem with a soft halo; the emblem is drawn procedurally at start-up (original artwork)
    ImVec2 ec = o + ImVec2(S(62), hh * 0.5f);
    glow(dl, ec, S(70), withAlpha(col::violet, 0.45f), 20);
    float er = S(40) * (1.0f + 0.015f * sinf(time_ * 2.0f));
    dl->AddImage(texId(emblemTex_), ec - ImVec2(er, er), ec + ImVec2(er, er));
    // title: per-glyph gradient amber -> coral -> magenta -> violet
    const char* title = "VC3 RECOMPILED";
    ImFont* tf = fonts().title;
    float fs = tf->FontSize;
    ImVec2 tp = o + ImVec2(S(120), S(20));
    ImVec2 tsz = tf->CalcTextSizeA(fs, FLT_MAX, 0, title);
    float x = tp.x;
    const ImU32 stops[4] = {col::amber, col::coral, col::magenta, col::violet};
    for (const char* c = title; *c; c++) {
        float t = (x - tp.x) / std::max(1.0f, tsz.x);
        float seg = t * 3;
        int i = std::min(2, (int)seg);
        ImU32 cc = lerpColor(stops[i], stops[i + 1], seg - i);
        dl->AddText(tf, fs, ImVec2(x + S(2), tp.y + S(3)), IM_COL32(0, 0, 0, 90), c, c + 1);
        dl->AddText(tf, fs, ImVec2(x, tp.y), cc, c, c + 1);
        x += tf->CalcTextSizeA(fs, FLT_MAX, 0, c, c + 1).x;
    }
    // subtitle
    ImGui::PushFont(fonts().body);
    dl->AddText(tp + ImVec2(S(2), tsz.y + S(2)), col::muted,
                "Valkyria Chronicles III, recompiled from your own PSP disc into a native Windows game");
    ImGui::PopFont();
    // build information (full details on the About tab)
    ImGui::PushFont(fonts().caption);
    dl->AddText(tp + ImVec2(S(2), tsz.y + S(26)), withAlpha(col::muted, 0.75f), vc2::buildInfoLine().c_str());
    ImGui::PopFont();

    // tabs
    const char* tabs[] = {"Install", "Settings", "About"};
    float pw = S(112), ph = S(36), gap = S(8);
    float tx = w - (pw * 3 + gap * 2) - S(28);
    for (int i = 0; i < 3; i++) {
        ImGui::SetCursorScreenPos(o + ImVec2(tx + i * (pw + gap), (hh - ph) * 0.5f));
        if (pillButton(tabs[i], tab_ == i, ImVec2(pw, ph))) tab_ = i;
    }
    ImGui::SetCursorScreenPos(o + ImVec2(S(24), hh + S(14)));
}

// ------------------------------------------------------------------ install tab
void App::drawDiscCard(ImVec2 size) {
    if (beginCard("discinfo", size, col::magenta, "Detected disc")) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float iw = std::min(avail.x, S(288)), ih = iw * 80.0f / 144.0f;
        ImVec2 p = ImGui::GetCursorScreenPos() + ImVec2((avail.x - iw) * 0.5f, S(4));
        if (discTex_) {
            glow(dl, p + ImVec2(iw, ih) * 0.5f, iw * 0.7f, withAlpha(disc_.ok ? col::cyan : col::red, 0.30f), 18);
            dl->AddRectFilled(p - ImVec2(S(5), S(5)), p + ImVec2(iw, ih) + ImVec2(S(5), S(5)), IM_COL32(8, 10, 28, 255), S(12));
            float aspect = discW_ > 0 ? (float)discH_ / discW_ : 80.0f / 144.0f;
            float h2 = iw * aspect;
            dl->AddImageRounded(texId(discTex_), p, p + ImVec2(iw, h2), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, S(8));
            dl->AddRect(p - ImVec2(S(5), S(5)), p + ImVec2(iw, ih) + ImVec2(S(5), S(5)),
                        withAlpha(disc_.ok ? col::cyan : col::red, 0.8f), S(12), 0, S(1.5f));
        } else {
            // procedural placeholder: an abstract optical disc
            ImVec2 c = p + ImVec2(iw, ih) * 0.5f;
            float r = ih * 0.48f;
            dl->AddRectFilled(p, p + ImVec2(iw, ih), IM_COL32(255, 255, 255, 8), S(12));
            dl->AddRect(p, p + ImVec2(iw, ih), IM_COL32(255, 255, 255, 30), S(12), 0, S(1));
            for (int i = 0; i < 6; i++)
                dl->AddCircle(c, r * (1.0f - i * 0.13f), withAlpha(lerpColor(col::cyan, col::magenta, i / 5.0f), 0.35f), 64, S(1.5f));
            float a = time_ * 0.8f;
            dl->PathArcTo(c, r * 0.8f, a, a + 1.2f, 24);
            dl->PathStroke(withAlpha(col::cyan, 0.8f), 0, S(3));
            dl->AddCircleFilled(c, r * 0.16f, IM_COL32(16, 19, 46, 255), 32);
        }
        ImGui::Dummy({avail.x, ih + S(14)});
        if (disc_.isPspDisc) {
            ImGui::PushFont(fonts().heading);
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted((disc_.gameName.empty() ? disc_.title : disc_.gameName).c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
            badge(disc_.discId.c_str(), col::cyan);
            if (!disc_.discVersion.empty()) {
                ImGui::SameLine(0, S(6));
                badge(("v" + disc_.discVersion).c_str(), col::violet);
            }
            ImGui::SameLine(0, S(6));
            badge(formatBytes(disc_.fileSize).c_str(), col::amber);
            ImGui::Dummy({0, S(2)});
            if (disc_.ok) {
                badge("\xe2\x9c\x94  Supported disc", col::green);
            } else {
                badge("\xe2\x9c\x96  Not supported", col::red);
                textWrappedColored(col::red, disc_.error.c_str());
            }
        } else {
            ImGui::PushFont(fonts().heading);
            textColored(col::muted, isoBuf_[0] ? "No valid disc" : "No disc selected");
            ImGui::PopFont();
            if (!disc_.error.empty()) textWrappedColored(col::red, disc_.error.c_str());
            else textWrappedColored(col::muted, "Choose the .iso image you dumped from your own UMD.");
        }
    }
    endCard();
}

void App::drawToolchainCard(ImVec2 size) {
    if (beginCard("toolchain", size, col::green, "C++ compiler", "3")) {
        auto tc = toolchainIfReady();
        if (!tc) {
            spinner(S(8), S(2.5f), col::cyan, time_);
            ImGui::SameLine();
            ImGui::TextUnformatted("Looking for Microsoft C++ Build Tools...");
        } else if (tc->ok) {
            statusDot(col::green);
            ImGui::SameLine();
            ImGui::Text("MSVC %s", tc->msvcVersion.c_str());
            ImGui::SameLine();
            textColored(col::muted, "  Windows SDK %s", tc->sdkVersion.c_str());
            ImGui::PushFont(fonts().caption);
            textColored(col::muted, "%s", pathUtf8(tc->vsInstall).c_str());
            if (tc->rc.empty()) textColored(col::amber, "rc.exe not found: the game EXE will have no icon.");
            ImGui::PopFont();
        } else {
            statusDot(col::red);
            ImGui::SameLine();
            textColored(col::red, "Microsoft C++ Build Tools are required");
            ImGui::PushFont(fonts().caption);
            textWrappedColored(col::muted, tc->error.c_str());
            ImGui::PopFont();
            if (gradientButton("Download Build Tools", ImVec2(S(210), S(32)), col::green, col::cyan))
                openUrl("https://visualstudio.microsoft.com/visual-cpp-build-tools/");
            ImGui::SameLine();
            if (ImGui::Button("Check again", ImVec2(S(120), S(32)))) {
                resetToolchainDetection();
                toolchainIfReady();
            }
        }
    }
    endCard();
}

void App::drawStageChips(const BuildStatus& st) {
    const int n = 8;
    float gap = S(6);
    float w = (ImGui::GetContentRegionAvail().x - gap * (n - 1)) / n;
    float h = S(30);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool failed = st.finished && !st.ok;
    ImGui::PushFont(fonts().caption);
    for (int i = 0; i < n; i++) {
        Stage s = kChipStages[i];
        ImVec2 a = p0 + ImVec2(i * (w + gap), 0), b = a + ImVec2(w, h);
        bool done = (st.stage > s && st.stage != Stage::Idle) || (st.finished && st.ok);
        bool active = st.stage == s && !st.finished && st.running;
        bool bad = failed && st.stage == s && !st.cancelled;
        float r = h * 0.5f;
        ImU32 tc = col::muted;
        if (bad) {
            dl->AddRectFilled(a, b, withAlpha(col::red, 0.25f), r);
            dl->AddRect(a, b, col::red, r, 0, S(1.5f));
            tc = col::red;
        } else if (active) {
            float pulse = 0.75f + 0.25f * sinf(time_ * 5);
            glow(dl, (a + b) * 0.5f, w * 0.6f, withAlpha(col::magenta, 0.3f * pulse), 10);
            gradientRect(dl, a, b, withAlpha(col::violet, pulse), withAlpha(col::magenta, pulse), r);
            tc = IM_COL32_WHITE;
        } else if (done) {
            dl->AddRectFilled(a, b, withAlpha(col::green, 0.16f), r);
            dl->AddRect(a, b, withAlpha(col::green, 0.7f), r, 0, S(1));
            tc = col::green;
        } else {
            dl->AddRect(a, b, IM_COL32(255, 255, 255, 34), r, 0, S(1));
        }
        char label[64];
        snprintf(label, sizeof label, "%s%s", done ? "\xe2\x9c\x93 " : "", kChipNames[i]);
        ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(a + (ImVec2(w, h) - ts) * 0.5f, tc, label);
    }
    ImGui::PopFont();
    ImGui::Dummy({0, h});
}

void App::drawLog(float height) {
    std::vector<LogLine> fresh;
    size_t end = builder_.fetchLog(logEnd_, fresh);
    bool grew = end != logEnd_;
    for (auto& l : fresh) log_.push_back(std::move(l));
    logEnd_ = end;

    ImGuiTreeNodeFlags fl = prefs_.logOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0;
    char hdr[64];
    snprintf(hdr, sizeof hdr, "Build log  (%zu lines)###buildlog", log_.size());
    bool open = ImGui::CollapsingHeader(hdr, fl);
    if (open != prefs_.logOpen) {
        prefs_.logOpen = open;
        persist();
    }
    if (!open) return;
    float h = std::max(S(60), height - ImGui::GetFrameHeightWithSpacing());
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(p, p + ImVec2(ImGui::GetContentRegionAvail().x, h), IM_COL32(6, 8, 24, 170), S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(8)));
    ImGui::BeginChild("logview", ImVec2(0, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleVar();
    bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4;
    ImGui::PushFont(fonts().mono);
    if (log_.empty()) textColored(col::muted, "Output of the recompiler, compiler and linker appears here.");
    ImGuiListClipper clip;
    clip.Begin((int)log_.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const LogLine& l = log_[i];
            ImU32 c = l.level == LogLevel::Good    ? col::green
                      : l.level == LogLevel::Warn  ? col::amber
                      : l.level == LogLevel::Error ? col::red
                      : l.level == LogLevel::Detail ? col::muted
                                                    : col::text;
            textColored(withAlpha(col::muted, 0.7f), "%s", formatDuration(l.t).c_str());
            ImGui::SameLine(S(52));
            textColored(c, "%s", l.text.c_str());
        }
    ImGui::PopFont();
    if (grew && atBottom) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void App::drawBuildCard(ImVec2 size) {
    BuildStatus st = builder_.status();
    if (wasRunning_ && st.finished) {
        wasRunning_ = false;
        if (st.ok) {
            toast_ = st.upToDate ? "Already up to date - ready to play" : "Recompiled successfully - ready to play!";
            toastError_ = false;
        } else {
            toast_ = st.cancelled ? "Build cancelled" : "Build failed - see the log";
            toastError_ = !st.cancelled;
        }
        toastTime_ = time_;
    }
    if (beginCard("build", size, col::cyan, "Recompile", "4")) {
        drawStageChips(st);
        ImGui::Dummy({0, S(2)});
        char pct[32];
        snprintf(pct, sizeof pct, "%d%%", (int)std::floor(st.progress * 100.0f + (st.finished && st.ok ? 0.5f : 0.0f)));
        progressBar(st.progress, ImVec2(0, S(28)), st.running, time_, st.stage == Stage::Idle ? nullptr : pct);

        // status line
        std::string left;
        if (st.stage == Stage::Idle) left = "Ready when you are.";
        else {
            left = stageLabel(st.stage);
            if (!st.detail.empty()) left += "  \xc2\xb7  " + st.detail;
        }
        std::string right;
        if (st.stage != Stage::Idle) {
            if (st.filesTotal) {
                right += "Files " + std::to_string(st.filesCompiled) + "/" + std::to_string(st.filesToCompile) + " (" +
                         std::to_string(st.filesCached) + " cached)";
                if (st.running && st.stage == Stage::Compile) right += "  \xc2\xb7  " + std::to_string(st.activeJobs) + " jobs";
                right += "   ";
            }
            right += "Elapsed " + formatDuration(st.elapsed);
            if (st.running) right += "   ETA " + (st.eta >= 0 ? formatDuration(st.eta) : std::string("--:--"));
        }
        ImGui::PushFont(fonts().caption);
        float rw = ImGui::CalcTextSize(right.c_str()).x;
        float availW = ImGui::GetContentRegionAvail().x;
        ImGui::PushClipRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(availW - rw - S(16), S(40)), true);
        textColored(st.finished && !st.ok ? (st.cancelled ? col::amber : col::red) : col::text, "%s", left.c_str());
        ImGui::PopClipRect();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - rw);
        textColored(col::muted, "%s", right.c_str());
        ImGui::PopFont();
        if (st.finished && !st.ok && !st.cancelled && !st.error.empty()) {
            ImGui::PushFont(fonts().caption);
            textWrappedColored(col::red, st.error.c_str());
            ImGui::PopFont();
        }

        // buttons
        ImGui::Dummy({0, S(4)});
        auto tc = toolchainIfReady();
        bool canBuild = !st.running && disc_.ok && tc && tc->ok && outBuf_[0];
        std::error_code ec;
        fs::path exe = gameExePath(pathFromUtf8(outBuf_));
        bool haveExe = outBuf_[0] && fs::is_regular_file(exe, ec);
        float bh = S(46);
        if (st.running) {
            if (gradientButton("Cancel", ImVec2(S(220), bh), col::red, col::coral, true, fonts().heading)) builder_.cancel();
        } else {
            if (gradientButton(haveExe ? "Recompile##go" : "Recompile", ImVec2(S(220), bh), col::cyan, col::violet, canBuild,
                               fonts().heading))
                startBuild();
            if (!canBuild && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                const char* why = !disc_.ok ? "Select a supported disc image first."
                                  : !tc     ? "Still looking for the C++ compiler..."
                                  : !tc->ok ? "Install the Microsoft C++ Build Tools first."
                                            : "Choose an install folder.";
                ImGui::SetTooltip("%s", why);
            }
        }
        ImGui::SameLine(0, S(12));
        if (gradientButton("\xe2\x96\xb6  Play", ImVec2(S(170), bh), col::green, IM_COL32(32, 180, 160, 255), haveExe && !st.running,
                           fonts().heading)) {
            std::string err;
            if (launchDetached(exe, exe.parent_path(), &err)) {
                toast_ = "Starting the game...";
                toastError_ = false;
            } else {
                toast_ = "Could not start the game: " + err;
                toastError_ = true;
            }
            toastTime_ = time_;
        }
        ImGui::SameLine(0, S(16));
        ImGui::BeginGroup();
        ImGui::BeginDisabled(st.running);
        ImGui::Checkbox("Full rebuild", &forceRebuild_);
        ImGui::EndDisabled();
        ImGui::SameLine();
        helpMarker("Ignore the object cache and compile every file again. Normally only changed files are recompiled.");
        ImGui::PushFont(fonts().caption);
        if (haveExe) textColored(col::muted, "Game: %s", pathUtf8(exe.filename()).c_str());
        else textColored(col::muted, "%u CPU threads available", hardwareThreads());
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::SameLine();
        float ow = S(130);
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - ow);
        if (ImGui::Button("Open folder", ImVec2(ow, bh))) openFolder(pathFromUtf8(outBuf_));

        ImGui::Dummy({0, S(4)});
        drawLog(ImGui::GetContentRegionAvail().y);
    }
    endCard();
}

void App::drawInstallTab() {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float gap = S(16);
    auto tc = toolchainIfReady();
    float tcH = tc && !tc->ok ? S(196) : S(100);
    float rowH = S(114);
    float topH = rowH * 2 + tcH + S(16);
    float leftW = std::floor((avail.x - gap) * 0.60f);

    ImGui::BeginGroup();
    // 1) disc image
    if (beginCard("iso", ImVec2(leftW, rowH), col::amber, "Game disc image", "1")) {
        float bw = S(110);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - bw - S(10));
        ImGui::InputTextWithHint("##iso", "Path to your .iso (drag & drop works too)", isoBuf_, sizeof isoBuf_);
        if (ImGui::IsItemDeactivatedAfterEdit()) setIso(pathFromUtf8(isoBuf_));
        ImGui::SameLine();
        if (gradientButton("Browse...##iso", ImVec2(bw, ImGui::GetFrameHeight()), col::amber, col::coral)) {
            fs::path p;
            if (pickPath(hwnd(), false, pathFromUtf8(isoBuf_), p)) setIso(p);
        }
        ImGui::PushFont(fonts().caption);
        if (disc_.ok) {
            statusDot(col::green);
            ImGui::SameLine();
            textColored(col::green, "%s detected (%s)", (disc_.gameName.empty() ? disc_.title : disc_.gameName).c_str(), disc_.discId.c_str());
        } else if (!disc_.error.empty()) {
            statusDot(col::red);
            ImGui::SameLine();
            std::string first = disc_.error.substr(0, disc_.error.find('\n'));
            textColored(col::red, "%s", first.c_str());
        } else {
            statusDot(col::muted);
            ImGui::SameLine();
            textColored(col::muted, "Your disc never leaves your PC; nothing from it is uploaded or bundled.");
        }
        ImGui::PopFont();
    }
    endCard();
    ImGui::Dummy({0, S(2)});
    // 2) install folder
    if (beginCard("out", ImVec2(leftW, rowH), col::violet, "Install folder", "2")) {
        float bw = S(110);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - bw - S(10));
        ImGui::InputTextWithHint("##out", "Where the recompiled game is built and installed", outBuf_, sizeof outBuf_);
        if (ImGui::IsItemDeactivatedAfterEdit()) setOut(pathFromUtf8(outBuf_));
        ImGui::SameLine();
        if (gradientButton("Browse...##out", ImVec2(bw, ImGui::GetFrameHeight()), col::violet, col::magenta)) {
            fs::path p;
            if (pickPath(hwnd(), true, pathFromUtf8(outBuf_), p)) setOut(p);
        }
        ImGui::PushFont(fonts().caption);
        std::string fsText = freeSpaceText(pathFromUtf8(outBuf_));
        textColored(col::muted, "%s%sNeeds about 400 MB.", fsText.c_str(), fsText.empty() ? "" : "  \xc2\xb7  ");
        ImGui::SameLine();
        if (ImGui::SmallButton("Default location")) setOut(defaultInstallDir());
        ImGui::SameLine();
        if (ImGui::SmallButton("Portable (next to launcher)")) setOut(launcherDir() / "VC3Recompiled");
        ImGui::PopFont();
    }
    endCard();
    ImGui::Dummy({0, S(2)});
    drawToolchainCard(ImVec2(leftW, topH - rowH * 2 - S(16)));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    drawDiscCard(ImVec2(0, topH));

    ImGui::Dummy({0, S(6)});
    drawBuildCard(ImVec2(0, -S(4)));
}

void App::drawAboutTab() {
    float w = std::min(ImGui::GetContentRegionAvail().x, S(860));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - w) * 0.5f);
    if (beginCard("about", ImVec2(w, -S(4)), col::violet, "About this launcher")) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(
            "This tool performs a static recompilation: the MIPS machine code on your PSP disc is loaded, analyzed and "
            "translated to C, then compiled by Microsoft's C++ compiler into a native Windows executable. The runtime "
            "re-implements the PSP hardware and operating system services the game needs.");
        ImGui::Dummy({0, S(6)});
        ImGui::PushFont(fonts().heading);
        ImGui::TextUnformatted("How it works");
        ImGui::PopFont();
        const char* steps[] = {"Validate the disc image and read its PARAM.SFO",
                               "Load the game executable and find every function",
                               "Translate each MIPS function into portable C code",
                               "Compile the C files in parallel with cl.exe (cached between runs)",
                               "Build an Explorer icon from the disc's own icon and link the executable",
                               "Copy the runtime next to the game and remember where your disc is"};
        for (int i = 0; i < 6; i++) {
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::TextUnformatted(steps[i]);
        }
        ImGui::Dummy({0, S(6)});
        ImGui::PushFont(fonts().heading);
        ImGui::TextUnformatted("Your data stays yours");
        ImGui::PopFont();
        textWrappedColored(col::muted,
                           "No game code, art or audio ships with this project. Everything game-specific is derived from "
                           "your own disc at build time and is written only to the install folder you choose. Keep your "
                           "disc image where it is: the game reads its assets from it while playing.");
        ImGui::Dummy({0, S(6)});
        textWrappedColored(col::muted,
                           "This is an unofficial fan project for preservation and is not affiliated with, endorsed or "
                           "sponsored by the game's publisher or developer. All trademarks belong to their owners.");
        ImGui::Dummy({0, S(6)});
        ImGui::PushFont(fonts().heading);
        ImGui::TextUnformatted("Build information");
        ImGui::PopFont();
        {
            const vc2::BuildInfo& b = vc2::buildInfo();
            const std::string commit = std::string(b.commit) + (b.dirty ? " (with uncommitted changes)" : "") +
                                       "  on " + b.branch + ", " + b.commitDate;
            const std::string compiler = std::string(b.compiler) + " (" + b.buildType + ")";
            const char* rows[][2] = {{"Version", b.version},
                                     {"Commit", commit.c_str()},
                                     {"Built", b.buildDate},
                                     {"Compiler", compiler.c_str()}};
            const float valueX = ImGui::GetCursorPosX() + S(96);
            for (auto& r : rows) {
                textColored(col::muted, "%s", r[0]);
                ImGui::SameLine(valueX);
                ImGui::TextUnformatted(r[1]);
            }
            if (ImGui::SmallButton("Copy build information")) {
                ImGui::SetClipboardText(vc2::buildInfoText().c_str());
                toast_ = "Build information copied";
                toastError_ = false;
                toastTime_ = time_;
            }
            ImGui::SameLine();
            textColored(col::muted, "Include it when you report a problem.");
        }
        ImGui::Dummy({0, S(10)});
        ImGui::PushFont(fonts().caption);
        textColored(col::muted, "Launcher folder: %s", pathUtf8(launcherDir()).c_str());
        textColored(col::muted, "Settings: launcher.ini (launcher), vc3recomp.ini (game, install folder)");
        ImGui::PopFont();
        ImGui::PopTextWrapPos();
    }
    endCard();
}

void App::frame() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    drawBackground(time_);
    drawHeader();
    // content area
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size = vp->Size - ImVec2(S(48), pos.y - vp->Pos.y + S(16));
    ImGui::BeginChild("content", size, 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    if (tab_ == 0) drawInstallTab();
    else if (tab_ == 1) drawSettingsTab(settings_, time_);
    else drawAboutTab();
    ImGui::EndChild();

    // toast
    if (!toast_.empty() && time_ - toastTime_ < 4.0f) {
        float a = std::min(1.0f, (4.0f - (time_ - toastTime_)) * 2.0f);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImGui::PushFont(fonts().bold);
        ImVec2 ts = ImGui::CalcTextSize(toast_.c_str());
        ImVec2 pad(S(20), S(12));
        ImVec2 p(vp->Pos.x + (vp->Size.x - ts.x) * 0.5f - pad.x, vp->Pos.y + vp->Size.y - ts.y - pad.y * 2 - S(28));
        ImVec2 q = p + ts + pad * 2;
        dl->AddRectFilled(p + ImVec2(0, S(4)), q + ImVec2(0, S(4)), withAlpha(IM_COL32_BLACK, 0.3f * a), (q.y - p.y) * 0.5f);
        gradientRect(dl, p, q, withAlpha(toastError_ ? col::red : col::violet, a), withAlpha(toastError_ ? col::coral : col::cyan, a),
                     (q.y - p.y) * 0.5f);
        dl->AddText(p + pad, withAlpha(IM_COL32_WHITE, a), toast_.c_str());
        ImGui::PopFont();
    }
    ImGui::End();
}

bool App::saveScreenshot(const fs::path& file, ImDrawData* dd) {
    auto genFb = (PFN_GenFramebuffers)SDL_GL_GetProcAddress("glGenFramebuffers");
    auto bindFb = (PFN_BindFramebuffer)SDL_GL_GetProcAddress("glBindFramebuffer");
    auto fbTex = (PFN_FramebufferTexture2D)SDL_GL_GetProcAddress("glFramebufferTexture2D");
    auto check = (PFN_CheckFramebufferStatus)SDL_GL_GetProcAddress("glCheckFramebufferStatus");
    auto delFb = (PFN_DeleteFramebuffers)SDL_GL_GetProcAddress("glDeleteFramebuffers");
    if (!genFb || !bindFb || !fbTex || !check || !delFb) return false;
    int w = (int)(dd->DisplaySize.x * dd->FramebufferScale.x), h = (int)(dd->DisplaySize.y * dd->FramebufferScale.y);
    GLuint tex = 0, fb = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    genFb(1, &fb);
    bindFb(GL_FRAMEBUFFER, fb);
    fbTex(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    bool ok = check(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        glViewport(0, 0, w, h);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(dd);
        Image img;
        img.w = w;
        img.h = h;
        img.rgba.resize((size_t)w * h * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
        // flip vertically and force opaque
        std::vector<uint8_t> row((size_t)w * 4);
        for (int y = 0; y < h / 2; y++) {
            uint8_t* a = &img.rgba[(size_t)y * w * 4];
            uint8_t* b = &img.rgba[(size_t)(h - 1 - y) * w * 4];
            memcpy(row.data(), a, row.size());
            memcpy(a, b, row.size());
            memcpy(b, row.data(), row.size());
        }
        for (size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
        ok = savePng(img, file);
    }
    bindFb(GL_FRAMEBUFFER, 0);
    delFb(1, &fb);
    glDeleteTextures(1, &tex);
    return ok;
}

int App::run() {
    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        MessageBoxW(nullptr, widen(std::string("Could not initialize SDL: ") + SDL_GetError()).c_str(), L"VC3 Launcher", MB_ICONERROR);
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);

    float primaryScale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (primaryScale <= 0) primaryScale = 1.0f;
    int lw = opt_.width > 0 ? opt_.width : 1280, lh = opt_.height > 0 ? opt_.height : 900;
    int ww = (int)(lw * primaryScale), wh = (int)(lh * primaryScale);
    SDL_Rect usable{};
    if (opt_.width <= 0 && SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable)) {
        ww = std::min(ww, (int)(usable.w * 0.94f));
        wh = std::min(wh, (int)(usable.h * 0.94f));
    }
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (!opt_.screenshot.empty()) flags |= SDL_WINDOW_HIDDEN;
    win_ = SDL_CreateWindow(("Valkyria Chronicles III Recompiled - Launcher " + std::string(vc2::buildInfo().version)).c_str(), ww, wh, flags);
    if (!win_) {
        MessageBoxW(nullptr, widen(std::string("Could not create the window: ") + SDL_GetError()).c_str(), L"VC3 Launcher",
                    MB_ICONERROR);
        return 1;
    }
    SDL_SetWindowMinimumSize(win_, (int)(900 * primaryScale), (int)(640 * primaryScale));
    gl_ = SDL_GL_CreateContext(win_);
    if (!gl_) {
        MessageBoxW(nullptr, L"OpenGL 3.3 is required to show the launcher.", L"VC3 Launcher", MB_ICONERROR);
        return 1;
    }
    SDL_GL_MakeCurrent(win_, gl_);
    SDL_GL_SetSwapInterval(1);

    // window icon: original procedural emblem (multi-resolution)
    Image em256 = renderEmblem(256), em64 = renderEmblem(64), em32 = renderEmblem(32);
    SDL_Surface* icon = SDL_CreateSurfaceFrom(256, 256, SDL_PIXELFORMAT_ABGR8888, em256.rgba.data(), 256 * 4);
    SDL_Surface* i64 = SDL_CreateSurfaceFrom(64, 64, SDL_PIXELFORMAT_ABGR8888, em64.rgba.data(), 64 * 4);
    SDL_Surface* i32 = SDL_CreateSurfaceFrom(32, 32, SDL_PIXELFORMAT_ABGR8888, em32.rgba.data(), 32 * 4);
    if (icon) {
        if (i64) SDL_AddSurfaceAlternateImage(icon, i64);
        if (i32) SDL_AddSurfaceAlternateImage(icon, i32);
        SDL_SetWindowIcon(win_, icon);
    }
    if (i64) SDL_DestroySurface(i64);
    if (i32) SDL_DestroySurface(i32);
    if (icon) SDL_DestroySurface(icon);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplSDL3_InitForOpenGL(win_, gl_);
    ImGui_ImplOpenGL3_Init("#version 330");
    dpi_ = SDL_GetWindowDisplayScale(win_);
    if (dpi_ <= 0) dpi_ = primaryScale;
    ui::setup(dpi_);
    emblemTex_ = makeTexture(em256, true);

    // state
    prefs_ = loadPrefs();
    toolchainIfReady();  // start MSVC detection in the background right away
    fs::path iso = !opt_.iso.empty() ? opt_.iso : prefs_.iso;
    fs::path out = !opt_.out.empty() ? opt_.out : (prefs_.outDir.empty() ? defaultInstallDir() : prefs_.outDir);
    snprintf(outBuf_, sizeof outBuf_, "%s", pathUtf8(out).c_str());
    settings_.load(out / "vc3recomp.ini");
    setIso(iso);
    if (opt_.tab == "settings") tab_ = 1;
    else if (opt_.tab == "about") tab_ = 2;
    bool autoBuildPending = opt_.autoBuild;
    forceRebuild_ = opt_.rebuild;

    Uint64 t0 = SDL_GetTicksNS();
    bool quit = false, shotDone = false;
    int exitCode = 0;
    while (!quit) {
        SDL_Event e;
        bool any = SDL_WaitEventTimeout(&e, 16);
        while (any) {
            bool consumed = false;
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && tab_ == 1 && !settings_.captureKey.empty())
                consumed = settingsHandleKey(settings_, e.key.scancode, time_);
            if (!consumed) ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) quit = true;
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(win_)) quit = true;
            if (e.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) pendingDpi_ = SDL_GetWindowDisplayScale(win_);
            if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) {
                fs::path p = pathFromUtf8(e.drop.data);
                std::error_code ec;
                if (fs::is_directory(p, ec)) setOut(p);
                else setIso(p);
                tab_ = 0;
            }
            any = SDL_PollEvent(&e);
        }
        time_ = (float)((SDL_GetTicksNS() - t0) / 1e9);
        if (SDL_GetWindowFlags(win_) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(50);
            continue;
        }
        if (autoBuildPending && toolchainIfReady()) {
            autoBuildPending = false;
            startBuild();
        }
        if (opt_.exitWhenBuilt) {
            BuildStatus bs = builder_.status();
            if (bs.finished && bs.ok) quit = true;
            exitCode = bs.finished && bs.ok ? 0 : 2;  // closed before a successful build: 2
        }
        rebuildFontsIfNeeded();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        frame();
        ImGui::Render();
        ImDrawData* dd = ImGui::GetDrawData();
        if (!opt_.screenshot.empty() && !shotDone && time_ >= opt_.screenshotDelay && !autoBuildPending) {
            shotDone = true;
            bool ok = saveScreenshot(opt_.screenshot, dd);
            exitCode = ok ? 0 : 3;
            quit = true;
        }
        int fbw = 0, fbh = 0;
        SDL_GetWindowSizeInPixels(win_, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.06f, 0.07f, 0.17f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(dd);
        SDL_GL_SwapWindow(win_);
    }
    builder_.cancel();
    builder_.join();
    persist();
    if (discTex_) glDeleteTextures(1, &discTex_);
    if (emblemTex_) glDeleteTextures(1, &emblemTex_);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl_);
    SDL_DestroyWindow(win_);
    SDL_Quit();
    return exitCode;
}

}  // namespace

int runGui(const GuiOptions& opt) {
    App app(opt);
    return app.run();
}

}  // namespace vc2l
