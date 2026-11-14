#pragma once
// Settings tab: edits vc3recomp.ini in the install folder (format shared with the runtime via host/config.cpp).
#include <SDL3/SDL_scancode.h>

#include <filesystem>
#include <string>

#include "runtime/host/config.h"

namespace vc2l {

struct SettingsState {
    std::filesystem::path file;
    vc2rt::Config cfg, saved;
    bool loaded = false, existsOnDisk = false;
    std::string captureKey;      // binding currently waiting for a key press
    bool captureAppend = false;  // add the key instead of replacing
    std::string message;
    bool messageError = false;
    double messageTime = -100;

    void load(const std::filesystem::path& iniFile);
    bool save();
    bool dirty() const;
};

void drawSettingsTab(SettingsState& s, float time);
// Called from the event loop; returns true when the key press was consumed by a pending key capture.
bool settingsHandleKey(SettingsState& s, SDL_Scancode sc, float time);

}  // namespace vc2l
