#include "config.h"

#include <fstream>
#include <sstream>

namespace vc2rt {

Config& config() {
    static Config c;
    return c;
}

static std::string trim(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}

bool Config::load(const std::filesystem::path& file) {
    std::ifstream f(file);
    if (!f) return false;
    std::string line, section;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = line.substr(eq + 1);
        // ignore a trailing "; comment" (older versions wrote them after values); key bindings may be ";"
        if (section != "Keys") v = v.substr(0, v.find(';'));
        v = trim(v);
        auto I = [&]() { return atoi(v.c_str()); };
        auto B = [&]() { return v == "1" || v == "true" || v == "yes"; };
        if (section == "Display") {
            if (k == "WindowMode") windowMode = (WindowMode)std::clamp(I(), 0, 2);
            else if (k == "WindowWidth") windowWidth = std::max(480, I());
            else if (k == "WindowHeight") windowHeight = std::max(272, I());
            else if (k == "VSync") vsync = B();
            else if (k == "InternalScale") internalScale = std::clamp(I(), 1, 8);
            else if (k == "Aspect") aspect = (AspectMode)std::clamp(I(), 0, 2);
            else if (k == "TextureFilter") textureFilter = (TexFilter)std::clamp(I(), 0, 1);
            else if (k == "Anisotropy") anisotropy = std::clamp(I(), 1, 16);
            else if (k == "MSAA") msaa = std::clamp(I(), 0, 8);
            else if (k == "TextureUpscale") textureUpscale = std::clamp(I(), 1, 4);
            else if (k == "FXAA") fxaa = B();
            else if (k == "ShowFPS") showFps = B();
            else if (k == "FrameLimit") frameLimit = std::max(0, I());
            else if (k == "HighFrameRate") highFrameRate = B();
        } else if (section == "Audio") {
            if (k == "MasterVolume") masterVolume = std::clamp(I(), 0, 100);
            else if (k == "LatencyMs") audioLatencyMs = std::clamp(I(), 16, 500);
        } else if (section == "Input") {
            if (k == "StickDeadzone") stickDeadzone = std::clamp(I(), 0, 90);
            else if (k == "SwapConfirm") swapConfirm = B();
        } else if (section == "Keys") {
            keyBindings[k] = v;
        } else if (section == "Debug") {
            if (k == "LogHLE") logHle = B();
        }
    }
    return true;
}

bool Config::save(const std::filesystem::path& file) const {
    std::ofstream f(file);
    if (!f) return false;
    f << "; Valkyria Chronicles Recompiled - settings\n";
    f << "[Display]\n";
    f << "; WindowMode: 0=windowed 1=borderless 2=fullscreen\n";
    f << "WindowMode=" << (int)windowMode << "\n";
    f << "WindowWidth=" << windowWidth << "\nWindowHeight=" << windowHeight << "\n";
    f << "VSync=" << vsync << "\n";
    f << "; InternalScale: 1=480x272 (PSP), 2=960x544, 3=1440x816, 4=1920x1088 ...\n";
    f << "InternalScale=" << internalScale << "\n";
    f << "; Aspect: 0=keep 16:9 1=stretch 2=integer scale\n";
    f << "Aspect=" << (int)aspect << "\n";
    f << "; TextureFilter: 0=nearest 1=linear\n";
    f << "TextureFilter=" << (int)textureFilter << "\n";
    f << "Anisotropy=" << anisotropy << "\nMSAA=" << msaa << "\n";
    f << "; TextureUpscale: 1=off 2..4=smooth texture upscaling\n";
    f << "TextureUpscale=" << textureUpscale << "\n";
    f << "FXAA=" << fxaa << "\nShowFPS=" << showFps << "\nFrameLimit=" << frameLimit << "\n";
    f << "; HighFrameRate: 60 FPS gameplay (engine frame-rate hook, experimental)\n";
    f << "HighFrameRate=" << highFrameRate << "\n";
    f << "[Audio]\nMasterVolume=" << masterVolume << "\nLatencyMs=" << audioLatencyMs << "\n";
    f << "[Input]\nStickDeadzone=" << stickDeadzone << "\nSwapConfirm=" << swapConfirm << "\n";
    f << "[Keys]\n";
    for (auto& [k, v] : keyBindings) f << k << "=" << v << "\n";
    f << "[Debug]\nLogHLE=" << logHle << "\n";
    return true;
}

}  // namespace vc2rt
