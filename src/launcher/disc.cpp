#include "disc.h"

#include "common/game_layout.h"
#include "common/iso9660.h"
#include "common/sfo.h"
#include "util.h"

namespace vc2l {

DiscInfo probeDisc(const std::filesystem::path& isoPath) {
    DiscInfo d;
    std::error_code ec;
    if (isoPath.empty()) {
        d.error = "No disc image selected.";
        return d;
    }
    if (!std::filesystem::is_regular_file(isoPath, ec)) {
        d.error = "The file does not exist: " + pathUtf8(isoPath);
        return d;
    }
    vc2::IsoImage iso;
    std::string err;
    if (!iso.open(isoPath, &err)) {
        d.error = "This file is not a readable ISO disc image (" + err + ").\nMake sure you selected an .iso dump "
                  "of your UMD (CSO/CHD compressed images are not supported).";
        return d;
    }
    d.fileSize = iso.totalSize();
    d.volumeId = iso.volumeId();
    Bytes sfoRaw;
    vc2::SfoData sfo;
    if (!iso.readFile("PSP_GAME/PARAM.SFO", sfoRaw) || !vc2::parseSfo(sfoRaw, sfo)) {
        d.error = "This disc image is not a PSP game (PSP_GAME/PARAM.SFO is missing or damaged).";
        return d;
    }
    d.isPspDisc = true;
    d.title = sfo.get("TITLE");
    d.discId = sfo.get("DISC_ID");
    d.discVersion = sfo.get("DISC_VERSION");
    iso.readFile("PSP_GAME/ICON0.PNG", d.iconPng);
    const vc2::GameProfile* profile = vc2::findGameProfile(d.discId.c_str());
    if (!profile) {
        d.error = "Unsupported disc: this is \"" + d.title + "\" (" + d.discId +
                  ").\nThis launcher builds Valkyria Chronicles III: ULJM05781 (Japan v1.01 with the English "
                  "fan-translation patch applied).";
        return d;
    }
    d.gameName = profile->name;
    d.exeName = profile->exeName;
    // Truncated or damaged dumps: every recompiled module must be fully inside the image.
    for (size_t mi = 0; mi < vc2::profileModuleCount(*profile); mi++) {
        const auto& m = profile->modules[mi];
        const vc2::IsoEntry* e = iso.find(m.discPath);
        if ((!e || e->size == 0) && strstr(m.discPath, "EBOOT.OLD")) {
            // VC3: only discs with the English fan translation applied carry the decrypted EBOOT.OLD
            d.error = "This disc doesn't have the English fan-translation patch applied. The recompilation "
                      "currently needs a patched disc image (it uses the patch's decrypted SYSDIR/EBOOT.OLD).";
            return d;
        }
        if (!e || e->size == 0) {
            d.error = std::string("The disc image is incomplete: ") + m.discPath + " is missing.";
            return d;
        }
        if ((uint64_t)e->lba * 2048 + e->size > d.fileSize) {
            d.error = std::string("The disc image is truncated (") + m.discPath +
                      " lies beyond the end of the file). Please re-dump your UMD.";
            return d;
        }
        uint8_t probe[16];
        if (!iso.readBytes((uint64_t)e->lba * 2048, probe, sizeof probe)) {
            d.error = std::string("Could not read ") + m.discPath + " from the disc image.";
            return d;
        }
    }
    d.ok = true;
    return d;
}

}  // namespace vc2l
