#pragma once
#include <filesystem>
#include <string>

namespace vc2l {

struct GuiOptions {
    std::filesystem::path iso, out;  // optional overrides from the command line
    std::filesystem::path screenshot;  // debug: render to an offscreen framebuffer, save PNG, exit
    float screenshotDelay = 1.0f;      // seconds after start (lets a build make progress with --autobuild)
    std::string tab;                   // "install" | "settings" | "about"
    bool autoBuild = false;            // debug: press Recompile on start
    bool rebuild = false;              // pre-tick "Full rebuild"
    bool exitWhenBuilt = false;        // close (exit code 0) once a build succeeds; used by --play
    int width = 0, height = 0;         // logical window size override
};

int runGui(const GuiOptions& opt);

// launcher.ini (next to the launcher exe; falls back to %LOCALAPPDATA%\VC3Recompiled when not writable)
struct LauncherPrefs {
    std::filesystem::path iso, outDir;
    bool logOpen = true;
    int jobs = 0;
};
LauncherPrefs loadPrefs();
bool savePrefs(const LauncherPrefs& p);

}  // namespace vc2l
