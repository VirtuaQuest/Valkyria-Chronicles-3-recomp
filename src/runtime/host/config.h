#pragma once
// User settings (vc3recomp.ini next to the game executable; also edited by the launcher).
#include <filesystem>
#include <map>
#include <string>

namespace vc2rt {

enum class WindowMode { Windowed = 0, Borderless = 1, Fullscreen = 2 };
enum class AspectMode { Keep = 0, Stretch = 1, IntegerScale = 2 };
enum class TexFilter { Nearest = 0, Linear = 1 };

struct Config {
    // display
    WindowMode windowMode = WindowMode::Windowed;
    int windowWidth = 1440, windowHeight = 816;
    bool vsync = true;
    int internalScale = 3;        // render resolution multiplier (1 = native 480x272)
    AspectMode aspect = AspectMode::Keep;
    TexFilter textureFilter = TexFilter::Linear;
    int anisotropy = 8;
    int msaa = 0;                 // 0, 2, 4, 8
    int textureUpscale = 1;       // 1 = off, 2..4 = smart texture upscaling factor
    bool fxaa = false;
    bool showFps = false;
    int frameLimit = 60;          // 0 = unlimited (fast forward)
    bool highFrameRate = false;   // render 30 FPS gameplay at 60 FPS where the engine supports it
    // audio
    int masterVolume = 100;       // 0..100
    int audioLatencyMs = 64;
    // input
    int stickDeadzone = 20;       // percent
    bool swapConfirm = false;     // swap Cross/Circle semantics
    std::map<std::string, std::string> keyBindings;  // PSP button -> SDL key name
    // misc
    bool logHle = false;

    bool load(const std::filesystem::path& file);
    bool save(const std::filesystem::path& file) const;
};

Config& config();

}  // namespace vc2rt
