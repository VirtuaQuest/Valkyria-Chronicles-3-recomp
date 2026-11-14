#include "pipeline.h"

#include <fstream>

#include "../common/game_layout.h"
#include "../common/iso9660.h"
#include "../common/sfo.h"
#include "../crypto/prx_decrypt.h"
#include "../loader/elf.h"

namespace vc2 {

std::string sha1Hex(const Bytes& data) {
    u8 d[20];
    sha1(data.data(), data.size(), d);
    char hex[41];
    for (int i = 0; i < 20; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
    return hex;
}

bool extractIcon(const std::filesystem::path& isoPath, const std::filesystem::path& dst) {
    IsoImage iso;
    Bytes png;
    if (!iso.open(isoPath) || !iso.readFile("PSP_GAME/ICON0.PNG", png) || png.empty()) return false;
    std::ofstream f(dst, std::ios::binary);
    f.write((const char*)png.data(), (std::streamsize)png.size());
    return (bool)f;
}

static const char* kMainC = R"(/* Generated entry point for the recompiled game executable. */
#include "funcs.h"
#include "vc2_runtime.h"

u8* psp_mem;

int main(int argc, char** argv) {
    Vc2GameImage img;
    img.abiVersion = VC2_RUNTIME_ABI;
    img.mem = &psp_mem;
    img.funcs = psp_func_table;
    img.funcCount = psp_func_table_count;
    img.imports = psp_import_table;
    img.importCount = psp_import_table_count;
    img.modules = psp_module_table;
    img.moduleCount = psp_module_table_count;
    return vc2_run(&img, argc, argv);
}
)";

TranslateReport translateGame(const std::filesystem::path& isoPath, const std::filesystem::path& outDir,
                              const ProgressFn& progress) {
    TranslateReport rep;
    auto step = [&](size_t d, size_t t, const std::string& s) {
        if (progress) progress(d, t, s);
    };
    step(0, 1, "Opening disc image");
    IsoImage iso;
    std::string err;
    if (!iso.open(isoPath, &err)) {
        rep.error = "Could not read the disc image: " + err;
        return rep;
    }
    Bytes sfoRaw;
    SfoData sfo;
    if (!iso.readFile("PSP_GAME/PARAM.SFO", sfoRaw) || !parseSfo(sfoRaw, sfo)) {
        rep.error = "This does not look like a PSP game disc (PARAM.SFO missing).";
        return rep;
    }
    rep.title = sfo.get("TITLE");
    rep.discId = sfo.get("DISC_ID");
    const GameProfile* profile = findGameProfile(rep.discId.c_str());
    if (!profile) {
        rep.error = "Unsupported disc '" + rep.discId + "'. Supported: ULJM05781 (Valkyria Chronicles III, Japan v1.01 "
                    "with the English fan-translation patch applied).";
        return rep;
    }
    rep.profile = profile;

    struct Unit {
        LoadedModule mod;
        Analysis ana;
        std::string sha;
        std::string path;
    };
    std::vector<Unit> units(profileModuleCount(*profile));
    for (size_t k = 0; k < units.size(); k++) {
        const auto& spec = profile->modules[k];
        step(k, units.size(), std::string("Loading ") + spec.discPath);
        Bytes raw, elf;
        if (!iso.readFile(spec.discPath, raw)) {
            rep.error = std::string("Missing file on disc: ") + spec.discPath;
            return rep;
        }
        DecryptResult dr = decryptPspExecutable(raw, elf);
        if (dr != DecryptResult::Ok && dr != DecryptResult::NotEncrypted) {
            rep.error = std::string("Could not load ") + spec.discPath + ": " + decryptResultString(dr);
            return rep;
        }
        units[k].sha = sha1Hex(elf);
        units[k].path = spec.discPath;
        step(k, units.size(), std::string("Analyzing ") + spec.discPath);
        if (!loadPspElf(elf, spec.base, units[k].mod, &err)) {
            rep.error = std::string("Could not load ") + spec.discPath + ": " + err;
            return rep;
        }
        analyzeModule(units[k].mod, units[k].ana);
        if (units[k].ana.externalBranches || units[k].ana.unresolvedJr) {
            // Not fatal: the runtime falls back to dynamic dispatch, but report it.
        }
    }

    std::vector<ModuleUnit> mu;
    for (auto& u : units) mu.push_back({&u.mod, &u.ana, u.path, u.sha});
    RecompOptions opt;
    opt.outDir = outDir;
    for (u32 h : profile->hooks)
        if (h) opt.hooks.insert(h);
    RecompResult rr = generateC(mu, opt, progress);
    if (!rr.ok) {
        rep.error = rr.error;
        return rep;
    }
    {
        std::ofstream f(outDir / "main.c", std::ios::binary);
        f << kMainC;
    }
    rep.sources = rr.sources;
    rep.sources.push_back(outDir / "main.c");
    rep.functions = rr.functions;
    rep.instructions = rr.instructions;
    rep.ok = true;
    return rep;
}

}  // namespace vc2
