#pragma once
// MIPS -> C code generator and full recompilation driver.
#include <filesystem>
#include <functional>
#include <set>
#include <string>

#include "analysis.h"

namespace vc2 {

struct RecompOptions {
    std::filesystem::path outDir;
    int fileCount = 96;          // number of generated .c files (parallel compile units)
    bool comments = false;       // emit disassembly comments
    std::set<u32> hooks;         // functions that call psp_hook(c, addr) on entry
};

// progress: (done, total, stage text)
using ProgressFn = std::function<void(size_t, size_t, const std::string&)>;

struct RecompResult {
    bool ok = false;
    std::string error;
    std::vector<std::filesystem::path> sources;
    size_t functions = 0, instructions = 0;
};

struct ModuleUnit {
    const LoadedModule* module = nullptr;
    const Analysis* analysis = nullptr;
    std::string discPath;   // path on the disc, e.g. PSP_GAME/SYSDIR/EBOOT.BIN
    std::string sha1;       // SHA1 of the ELF, verified by the runtime at boot
};

RecompResult generateC(const std::vector<ModuleUnit>& units, const RecompOptions& opt, const ProgressFn& progress);

}  // namespace vc2
