#pragma once
// Quick validation of the user's disc image (no decryption): PARAM.SFO, disc ID, module presence, size sanity.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vc2l {

struct DiscInfo {
    bool ok = false;           // supported disc, looks complete
    bool isPspDisc = false;    // PARAM.SFO parsed (even if it is another game)
    std::string error;         // user-facing reason when !ok
    std::string title, discId, discVersion, volumeId;
    std::string gameName;      // plain-text name from the game profile (e.g. "Valkyria Chronicles III")
    std::string exeName;       // executable name for this game (e.g. "VC3Recompiled.exe")
    uint64_t fileSize = 0;
    std::vector<uint8_t> iconPng;  // PSP_GAME/ICON0.PNG, kept in memory only
};

DiscInfo probeDisc(const std::filesystem::path& iso);

}  // namespace vc2l
