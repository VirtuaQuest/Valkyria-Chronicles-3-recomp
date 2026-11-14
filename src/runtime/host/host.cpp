#include "host.h"

#include <SDL3/SDL.h>
#include <wincodec.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include "../../common/build_info.h"
#include "config.h"
#include "gl.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

#pragma comment(lib, "windowscodecs.lib")

namespace vc2rt::host {

namespace {
SDL_Window* g_window = nullptr;
SDL_GLContext g_gl = nullptr;
std::vector<SDL_Gamepad*> g_pads;
PadState g_pad;
bool g_fastForward = false;
bool g_overlayOpen = false;
bool g_quit = false;
bool g_confirmQuit = false;  // overlay "Quit game" asked for confirmation
// Started from RecompLauncher's Big Picture mode (controller, couch): borderless fullscreen for this
// session only; vc3recomp.ini keeps the player's own window mode. F11 returns to it.
bool g_sessionFullscreen = false;
SDL_AudioStream* g_audio = nullptr;

// presentation thread state (see swapBuffers)
struct PresentSlot {
    GLuint tex = 0, fbo = 0;  // fbo belongs to the emulation context (FBOs are not shared)
    int w = 0, h = 0;
    void* fence = nullptr;    // GLsync that must signal before the next user touches the slot
};
enum class SlotState { Free, Writing, Ready, Shown };
constexpr int kSlots = 3;
PresentSlot g_slots[kSlots];
SlotState g_slotState[kSlots];
int g_writeSlot = 0;
bool g_threaded = false;
bool g_presentQuit = false;
SDL_GLContext g_presentGl = nullptr;
std::thread g_presentThread;
std::mutex g_presentMtx;
std::condition_variable g_presentCv;
std::atomic<int> g_swapInterval{1};
std::atomic<u32> g_swapCount{0};

struct KeyBind {
    u32 bit;
    const char* name;
    const char* defaults;  // comma separated SDL key names
};
// PC defaults: WASD = analog stick, arrows = D-pad, Space/Enter = Cross (confirm),
// Esc/Backspace = Circle (cancel), F = Square, R = Triangle, Q/E = L/R, Tab = Start, C = Select.
const KeyBind kBinds[] = {
    {PSP_CROSS, "Cross", "Space,Return"},
    {PSP_CIRCLE, "Circle", "Escape,Backspace"},
    {PSP_SQUARE, "Square", "F"},
    {PSP_TRIANGLE, "Triangle", "R"},
    {PSP_LTRIGGER, "L", "Q"},
    {PSP_RTRIGGER, "R", "E"},
    {PSP_START, "Start", "Tab"},
    {PSP_SELECT, "Select", "C"},
    {PSP_UP, "Up", "Up"},
    {PSP_DOWN, "Down", "Down"},
    {PSP_LEFT, "Left", "Left"},
    {PSP_RIGHT, "Right", "Right"},
};
struct StickBind {
    const char* name;
    const char* defaults;
    int axis;  // 0 = x, 1 = y
    int dir;
};
const StickBind kStick[] = {
    {"StickUp", "W", 1, -1}, {"StickDown", "S", 1, 1}, {"StickLeft", "A", 0, -1}, {"StickRight", "D", 0, 1}};

struct OverlayDrawer {
    std::function<void()> draw;
    std::function<bool()> wantsInput;
};
std::vector<OverlayDrawer> g_drawers;

bool drawerWantsInput() {
    for (auto& d : g_drawers)
        if (d.wantsInput && d.wantsInput()) return true;
    return false;
}

std::map<u32, std::vector<SDL_Scancode>> g_keyMap;
std::vector<SDL_Scancode> g_stickKeys[4];

std::vector<SDL_Scancode> parseKeys(const std::string& s) {
    std::vector<SDL_Scancode> v;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        std::string name = s.substr(i, j - i);
        if (!name.empty()) {
            SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
            if (sc != SDL_SCANCODE_UNKNOWN) v.push_back(sc);
        }
        i = j + 1;
    }
    return v;
}

void buildKeyMap() {
    auto& kb = config().keyBindings;
    for (auto& b : kBinds) {
        auto it = kb.find(b.name);
        g_keyMap[b.bit] = parseKeys(it != kb.end() ? it->second : b.defaults);
        if (it == kb.end()) kb[b.name] = b.defaults;
    }
    for (int i = 0; i < 4; i++) {
        auto it = kb.find(kStick[i].name);
        g_stickKeys[i] = parseKeys(it != kb.end() ? it->second : kStick[i].defaults);
        if (it == kb.end()) kb[kStick[i].name] = kStick[i].defaults;
    }
}

void updatePad() {
    PadState p;
    float sx = 0, sy = 0;
    if (!overlayWantsInput()) {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        for (auto& [bit, scs] : g_keyMap)
            for (SDL_Scancode sc : scs)
                if (keys[sc]) p.buttons |= bit;
        for (int i = 0; i < 4; i++)
            for (SDL_Scancode sc : g_stickKeys[i])
                if (keys[sc]) (kStick[i].axis == 0 ? sx : sy) += (float)kStick[i].dir;
        // Shift = walk slowly (half tilt) for precise movement in battle
        float mag = (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) ? 0.5f : 1.0f;
        if (sx != 0 && sy != 0) {
            sx *= 0.7071f;
            sy *= 0.7071f;
        }
        sx *= mag;
        sy *= mag;
        g_fastForward = keys[SDL_SCANCODE_GRAVE];
    }
    const float dz = config().stickDeadzone / 100.0f;
    for (SDL_Gamepad* gp : g_pads) {
        auto btn = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(gp, b); };
        bool south = btn(SDL_GAMEPAD_BUTTON_SOUTH), east = btn(SDL_GAMEPAD_BUTTON_EAST);
        if (config().swapConfirm) std::swap(south, east);
        if (south) p.buttons |= PSP_CROSS;
        if (east) p.buttons |= PSP_CIRCLE;
        if (btn(SDL_GAMEPAD_BUTTON_WEST)) p.buttons |= PSP_SQUARE;
        if (btn(SDL_GAMEPAD_BUTTON_NORTH)) p.buttons |= PSP_TRIANGLE;
        if (btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) p.buttons |= PSP_LTRIGGER;
        if (btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) p.buttons |= PSP_RTRIGGER;
        if (btn(SDL_GAMEPAD_BUTTON_START)) p.buttons |= PSP_START;
        if (btn(SDL_GAMEPAD_BUTTON_BACK)) p.buttons |= PSP_SELECT;
        if (btn(SDL_GAMEPAD_BUTTON_DPAD_UP)) p.buttons |= PSP_UP;
        if (btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) p.buttons |= PSP_DOWN;
        if (btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) p.buttons |= PSP_LEFT;
        if (btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) p.buttons |= PSP_RIGHT;
        if (SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) p.buttons |= PSP_LTRIGGER;
        if (SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) p.buttons |= PSP_RTRIGGER;
        float ax = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        float ay = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        float m = sqrtf(ax * ax + ay * ay);
        if (m > dz) {
            float scale = std::min(1.0f, (m - dz) / (1.0f - dz)) / m;
            sx += ax * scale;
            sy += ay * scale;
        }
        // right stick doubles as a D-pad for menus
        float rx = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
        float ry = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
        if (rx < -0.6f) p.buttons |= PSP_LEFT;
        if (rx > 0.6f) p.buttons |= PSP_RIGHT;
        if (ry < -0.6f) p.buttons |= PSP_UP;
        if (ry > 0.6f) p.buttons |= PSP_DOWN;
    }
    sx = std::clamp(sx, -1.0f, 1.0f);
    sy = std::clamp(sy, -1.0f, 1.0f);
    p.lx = (u8)std::clamp((int)lroundf(128 + sx * 127.5f), 0, 255);
    p.ly = (u8)std::clamp((int)lroundf(128 + sy * 127.5f), 0, 255);
    if (drawerWantsInput()) {  // a system dialog owns keyboard + gamepad
        p = PadState{};
        g_fastForward = false;
    }
    g_pad = p;
}

}  // namespace

SDL_Window* window() { return g_window; }
PadState pad() { return g_pad; }
bool fastForwardHeld() { return g_fastForward; }

static void startPresenter();
static void stopPresenter();

bool init(const std::string& title) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        LOG_ERROR("HOST", "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    const Config& cfg = config();
    g_window = SDL_CreateWindow(title.c_str(), cfg.windowWidth, cfg.windowHeight,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_window) {
        LOG_ERROR("HOST", "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    SDL_SetWindowMinimumSize(g_window, 480, 272);
    g_gl = SDL_GL_CreateContext(g_window);
    if (!g_gl) {
        LOG_ERROR("HOST", "OpenGL 3.3 context creation failed: %s", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(g_window, g_gl);
    if (!glLoadFunctions()) return false;
    LOG_INFO("HOST", "OpenGL %s / %s", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
    if (const char* mode = getenv("RECOMPLAUNCHER_MODE"); mode && strcmp(mode, "bigpicture") == 0) {
        g_sessionFullscreen = true;
        LOG_INFO("HOST", "started from RecompLauncher Big Picture mode: fullscreen for this session");
    }
    applyDisplaySettings();
    buildKeyMap();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForOpenGL(g_window, g_gl);
    ImGui_ImplOpenGL3_Init("#version 330 core");
    startPresenter();
    return true;
}

void shutdown() {
    stopPresenter();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (g_audio) SDL_DestroyAudioStream(g_audio);
    for (auto* p : g_pads) SDL_CloseGamepad(p);
    if (g_gl) SDL_GL_DestroyContext(g_gl);
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
}

void applyDisplaySettings() {
    const Config& cfg = config();
    const WindowMode mode = g_sessionFullscreen && cfg.windowMode == WindowMode::Windowed ? WindowMode::Borderless
                                                                                         : cfg.windowMode;
    switch (mode) {
        case WindowMode::Windowed: SDL_SetWindowFullscreen(g_window, false); break;
        case WindowMode::Borderless:
            SDL_SetWindowFullscreenMode(g_window, nullptr);
            SDL_SetWindowFullscreen(g_window, true);
            break;
        case WindowMode::Fullscreen: {
            SDL_DisplayID d = SDL_GetDisplayForWindow(g_window);
            const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(d);
            if (dm) SDL_SetWindowFullscreenMode(g_window, dm);
            SDL_SetWindowFullscreen(g_window, true);
            break;
        }
    }
    g_swapInterval = cfg.vsync ? 1 : 0;
    // with a presentation thread the emulation context never swaps; only that thread waits for vsync
    SDL_GL_SetSwapInterval(g_threaded ? 0 : g_swapInterval.load());
}

bool pumpEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        ImGui_ImplSDL3_ProcessEvent(&e);
        switch (e.type) {
            case SDL_EVENT_QUIT: g_quit = true; break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (SDL_Gamepad* gp = SDL_OpenGamepad(e.gdevice.which)) {
                    g_pads.push_back(gp);
                    LOG_INFO("INPUT", "gamepad connected: %s", SDL_GetGamepadName(gp));
                }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                for (size_t i = 0; i < g_pads.size(); i++)
                    if (SDL_GetGamepadID(g_pads[i]) == e.gdevice.which) {
                        SDL_CloseGamepad(g_pads[i]);
                        g_pads.erase(g_pads.begin() + i);
                        break;
                    }
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.repeat) break;
                if (e.key.scancode == SDL_SCANCODE_F1) g_overlayOpen = !g_overlayOpen;
                if (e.key.scancode == SDL_SCANCODE_F11 ||
                    (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
                    Config& cfg = config();
                    if (g_sessionFullscreen) g_sessionFullscreen = false;  // back to the player's window
                    else cfg.windowMode = cfg.windowMode == WindowMode::Windowed ? WindowMode::Borderless : WindowMode::Windowed;
                    applyDisplaySettings();
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                // Guide button opens the settings overlay
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) g_overlayOpen = !g_overlayOpen;
                break;
        }
    }
    updatePad();
    return !g_quit;
}

void drawableSize(int& w, int& h) { SDL_GetWindowSizeInPixels(g_window, &w, &h); }

// ------------------------------------------------------------------ presentation thread
// SDL_GL_SwapWindow with vsync blocks until the monitor's next refresh. Done on the emulation
// thread, that wait comes straight out of the guest's CPU time: the PSP vblank (59.94 Hz) drifts
// slowly against a 60 Hz monitor, and for long stretches every swap blocked for almost a whole
// refresh, starving time-budgeted game loops (VC3's boot loader slowed from ~4 s to ~40 s).
// Instead the final image is rendered into one of three textures shared with a second GL context;
// a dedicated thread blits the newest one to the window and swaps. Fences order the two contexts.

u32 presentedFrames() { return g_swapCount.load(); }

unsigned windowFramebuffer() {
    if (!g_threaded) return 0;
    int w, h;
    drawableSize(w, h);
    w = std::max(w, 1), h = std::max(h, 1);
    PresentSlot& s = g_slots[g_writeSlot];
    void* fence = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_presentMtx);
        fence = s.fence;
        s.fence = nullptr;
    }
    if (fence) {  // the presentation thread may still be reading this texture
        glClientWaitSync((GLsync)fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        glDeleteSync((GLsync)fence);
    }
    if (!s.tex || s.w != w || s.h != h) {
        if (!s.tex) glGenTextures(1, &s.tex);
        glBindTexture(GL_TEXTURE_2D, s.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        if (!s.fbo) glGenFramebuffers(1, &s.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.tex, 0);
        s.w = w, s.h = h;
    }
    return s.fbo;
}

void swapBuffers() {
    if (!g_threaded) {
        SDL_GL_SwapWindow(g_window);
        g_swapCount++;
        return;
    }
    windowFramebuffer();  // make sure the slot exists even if nothing was drawn
    void* fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    std::lock_guard<std::mutex> lk(g_presentMtx);
    int next = -1;
    for (int i = 0; i < kSlots; i++)
        if (g_slotState[i] == SlotState::Ready) {  // never shown: the new frame replaces it
            next = i;
            if (g_slots[i].fence) glDeleteSync((GLsync)g_slots[i].fence);
            g_slots[i].fence = nullptr;
        }
    for (int i = 0; i < kSlots && next < 0; i++)
        if (g_slotState[i] == SlotState::Free) next = i;
    g_slots[g_writeSlot].fence = fence;
    g_slotState[g_writeSlot] = SlotState::Ready;
    g_slotState[next] = SlotState::Writing;
    g_writeSlot = next;
    g_presentCv.notify_one();
}

static void presenterMain() {
    SDL_GL_MakeCurrent(g_window, g_presentGl);
    GLuint readFbo = 0;
    glGenFramebuffers(1, &readFbo);
    int interval = -100;
    int shown = -1;
    for (;;) {
        int slot = -1;
        void* fence = nullptr;
        {
            std::unique_lock<std::mutex> lk(g_presentMtx);
            g_presentCv.wait(lk, [] {
                if (g_presentQuit) return true;
                for (int i = 0; i < kSlots; i++)
                    if (g_slotState[i] == SlotState::Ready) return true;
                return false;
            });
            if (g_presentQuit) break;
            for (int i = 0; i < kSlots; i++)
                if (g_slotState[i] == SlotState::Ready) slot = i;
            if (shown >= 0) g_slotState[shown] = SlotState::Free;
            g_slotState[slot] = SlotState::Shown;
            shown = slot;
            fence = g_slots[slot].fence;
            g_slots[slot].fence = nullptr;
        }
        if (fence) {
            glClientWaitSync((GLsync)fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
            glDeleteSync((GLsync)fence);
        }
        const int want = g_swapInterval.load();
        if (want != interval) {
            SDL_GL_SetSwapInterval(want);
            interval = want;
        }
        const PresentSlot& s = g_slots[slot];
        int ww, wh;
        SDL_GetWindowSizeInPixels(g_window, &ww, &wh);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.tex, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glViewport(0, 0, ww, wh);
        if (ww != s.w || wh != s.h) {  // window resized since this frame was drawn
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glBlitFramebuffer(0, 0, s.w, s.h, 0, 0, ww, wh, GL_COLOR_BUFFER_BIT,
                          (ww == s.w && wh == s.h) ? GL_NEAREST : GL_LINEAR);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        static const char* check = getenv("VC2_PRESENT_CHECK");  // debug: verify the window image
        if (check && g_swapCount % 300 == 0) {
            std::vector<u32> px((size_t)ww * wh);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glReadBuffer(GL_BACK);
            glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            u64 sum = 0;
            for (u32 c : px) sum += (c & 0xFF) + ((c >> 8) & 0xFF) + ((c >> 16) & 0xFF);
            LOG_INFO("HOST", "present check: swap %u, window %dx%d, mean brightness %.1f", g_swapCount.load(), ww, wh,
                     sum / (3.0 * px.size()));
        }
        void* done = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        {
            std::lock_guard<std::mutex> lk(g_presentMtx);
            g_slots[slot].fence = done;  // the emulation thread waits for it before drawing here again
        }
        SDL_GL_SwapWindow(g_window);
        g_swapCount++;
    }
    glDeleteFramebuffers(1, &readFbo);
    SDL_GL_MakeCurrent(g_window, nullptr);
}

static void startPresenter() {
    if (getenv("VC2_SYNC_PRESENT")) return;  // diagnostic: swap on the emulation thread
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    g_presentGl = SDL_GL_CreateContext(g_window);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    SDL_GL_MakeCurrent(g_window, g_gl);
    if (!g_presentGl) {
        LOG_WARN("HOST", "no shared GL context (%s); presenting on the emulation thread", SDL_GetError());
        return;
    }
    for (auto& st : g_slotState) st = SlotState::Free;
    g_writeSlot = 0;
    g_slotState[0] = SlotState::Writing;
    g_threaded = true;
    applyDisplaySettings();
    g_presentThread = std::thread(presenterMain);
    LOG_INFO("HOST", "presenting on a separate thread");
}

static void stopPresenter() {
    if (!g_threaded) return;
    {
        std::lock_guard<std::mutex> lk(g_presentMtx);
        g_presentQuit = true;
    }
    g_presentCv.notify_all();
    if (g_presentThread.joinable()) g_presentThread.join();
    SDL_GL_MakeCurrent(g_window, g_gl);
    for (auto& s : g_slots) {
        if (s.fence) glDeleteSync((GLsync)s.fence);
        if (s.fbo) glDeleteFramebuffers(1, &s.fbo);
        if (s.tex) glDeleteTextures(1, &s.tex);
        s = PresentSlot{};
    }
    SDL_GL_DestroyContext(g_presentGl);
    g_presentGl = nullptr;
    g_threaded = false;
}

static float g_gameFps = 0.0f;
void setGameFps(float fps) { g_gameFps = fps; }

bool overlayWantsInput() { return g_overlayOpen || drawerWantsInput(); }

void addOverlayDrawer(std::function<void()> draw, std::function<bool()> wantsInput) {
    g_drawers.push_back({std::move(draw), std::move(wantsInput)});
}

void overlayFrame() {
    Config& cfg = config();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (cfg.showFps) {
        ImGui::SetNextWindowPos(ImVec2(8, 8));
        ImGui::SetNextWindowBgAlpha(0.45f);
        ImGui::Begin("##fps", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
        ImGui::Text("Game %.1f FPS", g_gameFps);
        // frames actually swapped to the window, measured over ~1 s
        static u32 lastSwaps = 0;
        static u64 lastT = 0;
        static float displayHz = 0.0f;
        const u64 now = hostMicros();
        if (!lastT || now - lastT >= 1000000) {
            const u32 swaps = presentedFrames();
            if (lastT) displayHz = (swaps - lastSwaps) * 1e6f / (float)(now - lastT);
            lastSwaps = swaps;
            lastT = now;
        }
        ImGui::TextDisabled("Display %.1f Hz", displayHz);
        ImGui::End();
    }
    if (!g_overlayOpen) g_confirmQuit = false;
    if (g_overlayOpen) {
        ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_FirstUseEver);
        ImGui::Begin("Settings (F1 / Guide button to close)", &g_overlayOpen, ImGuiWindowFlags_AlwaysAutoResize);
        bool changed = false;
        if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
            const char* modes[] = {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"};
            int wm = (int)cfg.windowMode;
            if (ImGui::Combo("Window mode", &wm, modes, 3)) {
                cfg.windowMode = (WindowMode)wm;
                g_sessionFullscreen = false;
                applyDisplaySettings();
                changed = true;
            }
            if (ImGui::Checkbox("V-Sync", &cfg.vsync)) {
                applyDisplaySettings();
                changed = true;
            }
            changed |= ImGui::SliderInt("Internal resolution", &cfg.internalScale, 1, 8, "%dx PSP");
            ImGui::TextDisabled("   %d x %d", 480 * cfg.internalScale, 272 * cfg.internalScale);
            const char* asp[] = {"Keep 16:9", "Stretch to window", "Integer scale"};
            int a = (int)cfg.aspect;
            if (ImGui::Combo("Aspect ratio", &a, asp, 3)) {
                cfg.aspect = (AspectMode)a;
                changed = true;
            }
            const char* tf[] = {"Nearest (sharp pixels)", "Linear (smooth)"};
            int t = (int)cfg.textureFilter;
            if (ImGui::Combo("Texture filtering", &t, tf, 2)) {
                cfg.textureFilter = (TexFilter)t;
                changed = true;
            }
            changed |= ImGui::SliderInt("Anisotropic filtering", &cfg.anisotropy, 1, 16, "%dx");
            changed |= ImGui::SliderInt("Texture upscaling", &cfg.textureUpscale, 1, 4, cfg.textureUpscale == 1 ? "Off" : "%dx");
            changed |= ImGui::Checkbox("FXAA (anti-aliasing)", &cfg.fxaa);
            changed |= ImGui::Checkbox("Show FPS", &cfg.showFps);
            changed |= ImGui::SliderInt("Frame limit", &cfg.frameLimit, 0, 240, cfg.frameLimit == 0 ? "Unlimited" : "%d FPS");
            changed |= ImGui::Checkbox("60 FPS gameplay (experimental)", &cfg.highFrameRate);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Renders the 30 FPS battle/map scenes at 60 FPS using the engine's own time step.\n"
                                  "Valkyria Chronicles III only. Takes effect at the next scene change.\n"
                                  "Experimental: some screens may misbehave; turn it off if anything looks stuck.");
        }
        if (ImGui::CollapsingHeader("Audio")) changed |= ImGui::SliderInt("Master volume", &cfg.masterVolume, 0, 100, "%d%%");
        if (ImGui::CollapsingHeader("Controls")) {
            changed |= ImGui::SliderInt("Stick deadzone", &cfg.stickDeadzone, 0, 60, "%d%%");
            changed |= ImGui::Checkbox("Swap confirm/cancel on gamepad (A/B)", &cfg.swapConfirm);
            ImGui::TextWrapped(
                "Keyboard: WASD analog stick (hold Shift to walk slowly), arrows D-pad, Space/Enter = Cross, "
                "Esc/Backspace = Circle, F = Square, R = Triangle, Q/E = L/R, Tab = Start, C = Select. "
                "Hold ` (backquote) to fast-forward. F11 / Alt+Enter toggles fullscreen. Rebind keys in vc3recomp.ini.");
            ImGui::TextWrapped(
                "In battle: Square aims, Cross fires, Start ends the unit's action (and the phase in command mode), "
                "Circle crouches behind sandbags, Triangle swaps the camera.");
        }
        ImGui::Separator();
        // Quit without a keyboard (controller players open this menu with the Guide button)
        if (!g_confirmQuit) {
            if (ImGui::Button("Quit game")) g_confirmQuit = true;
        } else {
            ImGui::TextUnformatted("Quit the game? Progress since your last save is lost.");
            if (ImGui::Button("Quit")) g_quit = true;
            ImGui::SameLine();
            if (ImGui::Button("Keep playing")) g_confirmQuit = false;
        }
        ImGui::TextDisabled("%s %s", vc2::buildInfo().product, vc2::buildInfoLine().c_str());
        if (changed) cfg.save(paths().configFile);
        ImGui::End();
    }
    for (auto& d : g_drawers)
        if (d.draw) d.draw();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// ---------------------------------------------------------------- audio
void audioInit() {
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = 44100;
    g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!g_audio) {
        LOG_ERROR("AUDIO", "could not open audio device: %s", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(g_audio);
}
void audioPush(const s16* samples, size_t frames) {
    if (g_audio) SDL_PutAudioStreamData(g_audio, samples, (int)(frames * 4));
}
size_t audioQueuedFrames() { return g_audio ? (size_t)SDL_GetAudioStreamQueued(g_audio) / 4 : 0; }
bool audioDeviceOpen() { return g_audio != nullptr; }

// ---------------------------------------------------------------- icon
void setIconFromPng(const std::vector<u8>& png) {
    if (png.empty() || !g_window) return;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* f = nullptr;
    IWICStream* st = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE*)png.data(), (DWORD)png.size())) &&
        SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        conv->GetSize(&w, &h);
        // ICON0 is 144x80; centre it on a square canvas so the taskbar icon is not squashed
        UINT side = std::max(w, h);
        std::vector<u8> rgba(w * h * 4), sq(side * side * 4, 0);
        conv->CopyPixels(nullptr, w * 4, (UINT)rgba.size(), rgba.data());
        UINT ox = (side - w) / 2, oy = (side - h) / 2;
        for (UINT y = 0; y < h; y++) memcpy(&sq[((oy + y) * side + ox) * 4], &rgba[y * w * 4], w * 4);
        SDL_Surface* s = SDL_CreateSurfaceFrom((int)side, (int)side, SDL_PIXELFORMAT_ABGR8888, sq.data(), (int)side * 4);
        if (s) {
            SDL_SetWindowIcon(g_window, s);
            SDL_DestroySurface(s);
        }
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (st) st->Release();
    if (f) f->Release();
}

}  // namespace vc2rt::host
