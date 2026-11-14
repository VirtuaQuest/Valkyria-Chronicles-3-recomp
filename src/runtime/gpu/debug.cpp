// Debug facilities (all opt-in through environment variables):
//   VC2_DUMP_DIR=dir        directory for frame dumps (PNG, window image without the overlay)
//   VC2_DUMP_EVERY=N        dump every Nth presented frame (default 60 when VC2_DUMP_DIR is set)
//   VC2_DUMP_AT=a,b,c       dump exactly these frame numbers
//   VC2_INPUT=f:btn[:len],  press PSP buttons at given frames through a virtual SDL gamepad
//                           (btn: cross circle square triangle start select up down left right l r)
//                           "from-to/period:btn[:len]" repeats a press
//   VC2_INPUT_AFTER=text[#n] start VC2_INPUT_THEN (same syntax, frames relative) once the n-th log
//   VC2_INPUT_THEN=...       message containing `text` has been written
//   VC2_INPUT_LIVE=file      play interactively: the running game reads commands appended to `file`
//                           ("<btn> [len]", "wait <frames>", "stick <x> <y> <frames>" with -1..1,
//                           "rstick ...", "shot <name>" -> <VC2_DUMP_DIR>/<name>.png, "trace on|off"
//                           = HLE call log); one per line
//   VC2_GPU_STATS=N         log GE statistics every N frames
#include <SDL3/SDL.h>
#include <wincodec.h>
#include <windows.h>

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>

#include "../hle/hle.h"
#include "../host/config.h"
#include "../host/host.h"
#include "gpu_internal.h"

namespace vc2rt::gpu {

namespace {
std::string g_dumpDir;
u64 g_dumpEvery = 0;
std::vector<u64> g_dumpAt;
u64 g_statsEvery = 0;

struct InputEvent {
    u64 frame;
    int button;  // SDL gamepad button, or -1 for an axis event
    u64 len;
    int axis = -1;
    Sint16 value = 0;
};
std::map<u64, std::string> g_namedDumps;  // frame -> file name (live input "shot")
std::string g_liveFile;
u64 g_liveOffset = 0;
std::vector<InputEvent> g_inputs;
SDL_Joystick* g_vjoy = nullptr;
SDL_JoystickID g_vjoyId = 0;

std::string env(const char* n) {
    char buf[4096];
    DWORD r = GetEnvironmentVariableA(n, buf, sizeof buf);
    return (r > 0 && r < sizeof buf) ? std::string(buf, r) : std::string();
}

int buttonFromName(const std::string& s) {
    struct M {
        const char* n;
        int b;
    } const map[] = {{"cross", SDL_GAMEPAD_BUTTON_SOUTH},        {"circle", SDL_GAMEPAD_BUTTON_EAST},
                     {"square", SDL_GAMEPAD_BUTTON_WEST},        {"triangle", SDL_GAMEPAD_BUTTON_NORTH},
                     {"start", SDL_GAMEPAD_BUTTON_START},        {"select", SDL_GAMEPAD_BUTTON_BACK},
                     {"up", SDL_GAMEPAD_BUTTON_DPAD_UP},         {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
                     {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},     {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
                     {"l", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},    {"r", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}};
    for (auto& m : map)
        if (s == m.n) return m.b;
    return -1;
}

std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(c, i);
        if (j == std::string::npos) j = s.size();
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

// VC2_INPUT_AFTER=text[#n] + VC2_INPUT_THEN=script: a second script whose frame numbers count from
// the frame on which the n-th (default 1st) log message containing `text` was seen.
std::vector<InputEvent> g_anchored;
u32 g_anchorCount = 0;
bool g_anchorFired = false;

void parseScript(const std::string& spec, std::vector<InputEvent>& out) {
    for (auto& item : split(spec, ',')) {
        auto parts = split(item, ':');
        if (parts.size() < 2) continue;
        int b = buttonFromName(parts[1]);
        if (b < 0) continue;
        const u64 len = parts.size() > 2 ? (u64)std::stoull(parts[2]) : 6;
        // "from-to/period:button[:len]" repeats a press, e.g. 300-20000/40:cross:4
        const size_t dash = parts[0].find('-'), slash = parts[0].find('/');
        if (dash != std::string::npos && slash != std::string::npos && slash > dash) {
            u64 from = std::stoull(parts[0].substr(0, dash)), to = std::stoull(parts[0].substr(dash + 1, slash - dash - 1));
            u64 period = std::max<u64>(1, std::stoull(parts[0].substr(slash + 1)));
            for (u64 f = from; f <= to; f += period) out.push_back({f, b, len});
            continue;
        }
        out.push_back({(u64)std::stoull(parts[0]), b, len});
    }
}

void setupInput() {
    parseScript(env("VC2_INPUT"), g_inputs);
    std::string after = env("VC2_INPUT_AFTER");
    if (!after.empty()) {
        parseScript(env("VC2_INPUT_THEN"), g_anchored);
        g_anchorCount = 1;
        const size_t hash = after.rfind('#');
        if (hash != std::string::npos) {
            g_anchorCount = std::max(1, atoi(after.c_str() + hash + 1));
            after.resize(hash);
        }
        logWatch(after);
    }
    g_liveFile = env("VC2_INPUT_LIVE");
    if (!g_liveFile.empty()) std::ofstream(g_liveFile, std::ios::app).close();  // create it
    if (g_inputs.empty() && g_anchored.empty() && g_liveFile.empty()) return;
    // the window usually has no focus in automated runs; SDL drops pad input otherwise
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.name = "VC2 scripted input";
    g_vjoyId = SDL_AttachVirtualJoystick(&desc);
    if (g_vjoyId) g_vjoy = SDL_OpenJoystick(g_vjoyId);
    LOG_INFO("GPU", "scripted input: %zu events (virtual pad %s)", g_inputs.size(), g_vjoy ? "ok" : SDL_GetError());
}

// Reads commands appended to the VC2_INPUT_LIVE file since the last frame.
void pollLiveInput() {
    if (g_liveFile.empty() || g_frame % 4) return;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(g_liveFile.c_str(), GetFileExInfoStandard, &fa)) return;
    const u64 size = ((u64)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    if (size < g_liveOffset) g_liveOffset = 0;  // file was truncated: start over
    if (size == g_liveOffset) return;
    std::ifstream f(g_liveFile, std::ios::binary);
    f.seekg((std::streamoff)g_liveOffset);
    std::string chunk((size_t)(size - g_liveOffset), '\0');
    f.read(chunk.data(), (std::streamsize)chunk.size());
    const size_t lastNl = chunk.rfind('\n');
    if (lastNl == std::string::npos) return;  // wait for a complete line
    g_liveOffset += lastNl + 1;
    u64 at = g_frame + 1;
    std::istringstream lines(chunk.substr(0, lastNl));
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream w(line);
        std::string cmd;
        if (!(w >> cmd)) continue;
        if (cmd == "wait") {
            u64 n = 0;
            w >> n;
            at += n;
        } else if (cmd == "stick" || cmd == "rstick") {
            float x = 0, y = 0;
            u64 n = 30;
            w >> x >> y >> n;
            const int ax = cmd == "stick" ? SDL_GAMEPAD_AXIS_LEFTX : SDL_GAMEPAD_AXIS_RIGHTX;
            const int ay = cmd == "stick" ? SDL_GAMEPAD_AXIS_LEFTY : SDL_GAMEPAD_AXIS_RIGHTY;
            g_inputs.push_back({at, -1, n, ax, (Sint16)std::clamp(x * 32767.0f, -32767.0f, 32767.0f)});
            g_inputs.push_back({at, -1, n, ay, (Sint16)std::clamp(y * 32767.0f, -32767.0f, 32767.0f)});
            at += n;
        } else if (cmd == "trace") {  // "trace on|off": HLE call log (debug level) while investigating
            std::string v;
            w >> v;
            const bool on = v != "off";
            setLogLevel(on ? LogLevel::Debug : LogLevel::Info);
            hleSetTrace(on);
        } else if (cmd == "shot") {
            std::string name;
            w >> name;
            g_namedDumps[at] = name.empty() ? "shot" : name;
        } else if (int b = buttonFromName(cmd); b >= 0) {
            u64 n = 6;
            w >> n;
            g_inputs.push_back({at, b, n});
            at += n + 4;
        } else {
            LOG_WARN("GPU", "live input: unknown command '%s'", line.c_str());
            continue;
        }
        LOG_INFO("GPU", "live input @%llu: %s", (unsigned long long)at, line.c_str());
    }
}

void runInput() {
    if (!g_vjoy) return;
    pollLiveInput();
    if (!g_anchorFired && !g_anchored.empty() && logWatchHits() >= g_anchorCount) {
        g_anchorFired = true;
        LOG_INFO("GPU", "scripted input: anchor reached at frame %llu", (unsigned long long)g_frame);
        for (auto e : g_anchored) {
            e.frame += g_frame + 1;
            g_inputs.push_back(e);
        }
    }
    for (auto& e : g_inputs) {
        if (e.axis >= 0) {
            if (g_frame == e.frame) SDL_SetJoystickVirtualAxis(g_vjoy, e.axis, e.value);
            if (g_frame == e.frame + e.len) SDL_SetJoystickVirtualAxis(g_vjoy, e.axis, 0);
            continue;
        }
        if (g_frame == e.frame && !SDL_SetJoystickVirtualButton(g_vjoy, e.button, true))
            LOG_WARN("GPU", "virtual button failed: %s", SDL_GetError());
        if (g_frame == e.frame + 3 && g_liveFile.empty()) {
            SDL_Gamepad* gp = SDL_GetGamepadFromID(g_vjoyId);
            LOG_INFO("GPU", "scripted input: button %d held, pad buttons now %06x (joy btn %d, gamepad %p btn %d, mapping %s)",
                     e.button, host::pad().buttons, SDL_GetJoystickButton(g_vjoy, e.button), (void*)gp,
                     gp ? SDL_GetGamepadButton(gp, (SDL_GamepadButton)e.button) : -1,
                     gp ? SDL_GetGamepadMapping(gp) : "none");
        }
        if (g_frame == e.frame + e.len) SDL_SetJoystickVirtualButton(g_vjoy, e.button, false);
    }
}
}  // namespace

void debugRequestDump(u64 frame) { g_dumpAt.push_back(frame); }

// Reads the final window image and writes it as PNG on a worker thread: a synchronous encode
// stalls vblank and audio mixing on the host thread long enough to change the game's own timing.
void captureWindow(const char* path, int winW, int winH) {
    std::vector<u32> px((size_t)winW * winH);
    const unsigned target = host::windowFramebuffer();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
    glReadBuffer(target ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, winW, winH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    std::thread([px = std::move(px), p = std::string(path), winW, winH]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (!savePng(p.c_str(), px.data(), winW, winH, true)) LOG_WARN("GPU", "could not write %s", p.c_str());
        CoUninitialize();
    }).detach();
}

bool savePng(const char* path, const u32* px, int w, int h, bool flipY, bool keepAlpha) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* f = nullptr;
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = false;
    std::wstring wpath(path, path + strlen(path));
    std::vector<u8> bgra((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
        const u32* s = px + (size_t)(flipY ? h - 1 - y : y) * w;
        u8* d = bgra.data() + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            u32 c = s[x];
            d[x * 4 + 0] = (u8)(c >> 16);
            d[x * 4 + 1] = (u8)(c >> 8);
            d[x * 4 + 2] = (u8)c;
            d[x * 4 + 3] = keepAlpha ? (u8)(c >> 24) : 255;
        }
    }
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h))) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
            SUCCEEDED(frame->WritePixels(h, w * 4, (UINT)bgra.size(), bgra.data())) && SUCCEEDED(frame->Commit()) &&
            SUCCEEDED(enc->Commit()))
            ok = true;
    }
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    if (f) f->Release();
    return ok;
}

void debugInit() {
    g_dumpDir = env("VC2_DUMP_DIR");
    if (!g_dumpDir.empty()) {
        CreateDirectoryA(g_dumpDir.c_str(), nullptr);
        std::string every = env("VC2_DUMP_EVERY");
        g_dumpEvery = every.empty() ? 60 : std::stoull(every);
        for (auto& s : split(env("VC2_DUMP_AT"), ',')) g_dumpAt.push_back(std::stoull(s));
        LOG_INFO("GPU", "frame dumps to %s (every %llu, %zu fixed)", g_dumpDir.c_str(), (unsigned long long)g_dumpEvery, g_dumpAt.size());
    }
    std::string st = env("VC2_GPU_STATS");
    if (!st.empty()) g_statsEvery = std::max<u64>(1, std::stoull(st));
    setupInput();
}

void debugFrame(int winW, int winH) {
    runInput();
    // VC2_SET_SCALE=frame:scale[,frame:scale] switches the internal resolution at runtime (testing)
    static std::vector<std::pair<u64, int>> scaleSwitch = [] {
        std::vector<std::pair<u64, int>> v;
        for (auto& it : split(env("VC2_SET_SCALE"), ',')) {
            auto p = split(it, ':');
            if (p.size() == 2) v.push_back({std::stoull(p[0]), std::stoi(p[1])});
        }
        return v;
    }();
    for (auto& [f, sc] : scaleSwitch)
        if (f == g_frame) config().internalScale = sc;
    if (g_statsEvery && g_frame % g_statsEvery == 0) {
        static u64 lastT = 0;
        const u64 now = hostMicros();
        const double fps = lastT ? g_statsEvery * 1e6 / (double)(now - lastT) : 0.0;
        lastT = now;
        const double n = (double)g_statsEvery;
        LOG_INFO("GPU", "frame %llu (%.1f fps): per frame prims %.0f verts %.0f batches %.0f tex %.0f uploads %.1f fbsw %.1f xfers %.1f | lists %u done %u reentered %u deferred %u | cpu ge %.2fms (cb %.2f tex %.2f fb %.2f vtx %.2f) present %.2fms",
                 (unsigned long long)g_frame, fps, g_stats.prims / n, g_stats.verts / n, g_stats.batches / n,
                 g_stats.textures / n, g_stats.texUploads / n, g_stats.fbSwitches / n, g_stats.transfers / n, g_stats.lists, g_stats.finished, g_stats.reentered, g_stats.deferred,
                 g_stats.geMicros / n / 1000.0, g_stats.cbMicros / n / 1000.0, g_stats.texMicros / n / 1000.0,
                 g_stats.fbMicros / n / 1000.0, g_stats.vtxMicros / n / 1000.0, g_stats.presentMicros / n / 1000.0);
        g_stats = GpuStats{};
    }
    if (g_dumpDir.empty()) return;
    bool dump = g_dumpEvery && g_frame % g_dumpEvery == 0;
    for (u64 f : g_dumpAt) dump |= f == g_frame;
    if (!dump) return;
    char path[1024];
    snprintf(path, sizeof path, "%s\\frame_%06llu.png", g_dumpDir.c_str(), (unsigned long long)g_frame);
    captureWindow(path, winW, winH);
}

// Named live-input screenshots are taken after the overlay (system dialogs such as the on-screen
// keyboard, the settings menu) so they show exactly what the player sees.
void debugAfterOverlay(int winW, int winH) {
    if (g_dumpDir.empty()) return;
    auto named = g_namedDumps.find(g_frame);
    if (named == g_namedDumps.end()) return;
    char path[1024];
    snprintf(path, sizeof path, "%s\\%s.png", g_dumpDir.c_str(), named->second.c_str());
    g_namedDumps.erase(named);
    captureWindow(path, winW, winH);
}

}  // namespace vc2rt::gpu
