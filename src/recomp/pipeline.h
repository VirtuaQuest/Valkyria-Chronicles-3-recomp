#pragma once
// End-to-end translation pipeline shared by the CLI and the launcher:
//   disc -> read modules -> load/relocate -> analyze -> generate C.
#include <filesystem>
#include <string>

#include "codegen.h"

namespace vc2 {

class IsoImage;
struct GameProfile;

struct TranslateReport {
    bool ok = false;
    std::string error;
    std::string title, discId;
    const GameProfile* profile = nullptr;
    std::vector<std::filesystem::path> sources;
    size_t functions = 0, instructions = 0;
};

// Validates the disc and writes the generated C sources plus main.c to `outDir`.
TranslateReport translateGame(const std::filesystem::path& isoPath, const std::filesystem::path& outDir,
                              const ProgressFn& progress);

// Copies PSP_GAME/ICON0.PNG from the disc to `dst` (used for the window icon). Never stored in the repo.
bool extractIcon(const std::filesystem::path& isoPath, const std::filesystem::path& dst);

std::string sha1Hex(const Bytes& data);

}  // namespace vc2
