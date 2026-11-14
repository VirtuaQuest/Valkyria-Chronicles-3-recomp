// Developer CLI: inspect the disc, extract the game executable, dump imports.
//   vc2tool info    <iso>
//   vc2tool decrypt <iso> <disc path> <out.elf>   (copies a plain ELF out of the disc)
//   vc2tool imports <iso>
//   vc2tool hashes  <iso|cso>                     (SHA-1 of every file on the disc)
#include <cstdio>
#include <cstring>
#include <fstream>

#include "common/iso9660.h"
#include "common/sfo.h"
#include "crypto/prx_decrypt.h"
#include "loader/elf.h"
#include "recomp/analysis.h"
#include "recomp/mips.h"
#include "recomp/disasm.h"
#include "recomp/codegen.h"
#include "recomp/pipeline.h"
#include <map>

using namespace vc2;

static int usage() {
    fprintf(stderr, "usage: vc2tool info|hashes|decrypt|imports <iso> [args]\n");
    return 1;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string cmd = argv[1];
    IsoImage iso;
    std::string err;
    if (!iso.open(argv[2], &err)) {
        fprintf(stderr, "open failed: %s\n", err.c_str());
        return 1;
    }
    if (cmd == "info") {
        Bytes sfo;
        SfoData s;
        if (iso.readFile("PSP_GAME/PARAM.SFO", sfo) && parseSfo(sfo, s))
            printf("title=%s id=%s ver=%s\n", s.get("TITLE").c_str(), s.get("DISC_ID").c_str(), s.get("DISC_VERSION").c_str());
        for (auto& [p, e] : iso.entries()) printf("%10u %8u %s%s\n", e.size, e.lba, p.c_str(), e.isDir ? "/" : "");
        return 0;
    }
    if (cmd == "hashes") {  // SHA-1 of every file: compare an .iso with a .cso of the same disc
        printf("image %llu bytes%s\n", (unsigned long long)iso.totalSize(), iso.compressed() ? " (CSO)" : "");
        int bad = 0;
        for (auto& [p, e] : iso.entries()) {
            if (e.isDir) continue;
            Bytes data;
            u8 h[20];
            if (!iso.readFile(p, data) || !sha1(data.data(), data.size(), h)) {
                printf("READ-ERROR %s\n", p.c_str());
                bad++;
                continue;
            }
            for (u8 b : h) printf("%02x", b);
            printf(" %s\n", p.c_str());
        }
        return bad ? 1 : 0;
    }
    std::string path = cmd == "decrypt" && argc >= 4 ? argv[3] : "PSP_GAME/SYSDIR/EBOOT.OLD";
    if (const char* env = getenv("VC2_MODULE")) path = env;  // inspect another module (dev aid)
    Bytes raw, elf;
    if (!iso.readFile(path, raw)) {
        fprintf(stderr, "file not found: %s\n", path.c_str());
        return 1;
    }
    DecryptResult r = decryptPspExecutable(raw, elf);
    if (r != DecryptResult::Ok && r != DecryptResult::NotEncrypted) {
        fprintf(stderr, "cannot load: %s\n", decryptResultString(r));
        return 1;
    }
    if (cmd == "decrypt") {
        if (argc < 5) return usage();
        std::ofstream(argv[4], std::ios::binary).write((const char*)elf.data(), elf.size());
        printf("wrote %zu bytes (%s)\n", elf.size(), decryptResultString(r));
        return 0;
    }
    if (cmd == "imports") {
        LoadedModule m;
        if (!loadPspElf(elf, 0x08804000, m, &err)) {
            fprintf(stderr, "load failed: %s\n", err.c_str());
            return 1;
        }
        printf("module %s entry=%08x gp=%08x text=%08x-%08x end=%08x reloc=%d\n", m.name.c_str(), m.entry, m.gp,
               m.textStart, m.textEnd, m.imageEnd, m.relocatable);
        for (auto& s : m.sections) printf("  sec %-24s %08x %08x type=%08x\n", s.name.c_str(), s.addr, s.size, s.type);
        for (auto& l : m.imports) {
            printf("import %s (%zu funcs, %zu vars)\n", l.name.c_str(), l.funcs.size(), l.vars.size());
            for (auto& f : l.funcs) printf("  %08x @ %08x\n", f.nid, f.stubAddr);
            for (auto& v : l.vars) printf("  var %08x @ %08x\n", v.nid, v.addr);
        }
        for (auto& l : m.exports) printf("export '%s' %zu funcs %zu vars\n", l.name.c_str(), l.funcs.size(), l.vars.size());
        return 0;
    }
    if (cmd == "analyze") {
        LoadedModule m;
        if (!loadPspElf(elf, 0x08804000, m, &err)) return 1;
        Analysis a;
        analyzeModule(m, a);
        size_t withExt = 0, maxLen = 0;
        for (auto& f : a.functions) {
            if (!f.externalEntries.empty()) withExt++;
            maxLen = std::max<size_t>(maxLen, (f.end - f.start) / 4);
        }
        printf("instructions=%zu functions=%zu jumptables=%zu unresolvedJr=%zu externalBranches=%zu funcsWithExtEntries=%zu maxFuncInstrs=%zu refs=%zu\n",
               a.instructionCount, a.functions.size(), a.jumpTables.size(), a.unresolvedJr, a.externalBranches, withExt, maxLen, m.pointerRefs.size());
        std::map<u32, u32> opHist;
        for (u32 pc = a.codeStart; pc < a.codeEnd; pc += 4) {
            u32 w = m.read32(pc);
            if (mips::isVfpu(w)) opHist[mips::op(w)]++;
        }
        for (auto& w : a.warnings) printf("warn: %s\n", w.c_str());
        for (auto& [o, n] : opHist) printf("vfpu op %02x: %u\n", o, n);
        if (argc > 3 && std::string(argv[3]) == "-v") {
            for (auto& f : a.functions)
                for (u32 e : f.externalEntries) printf("ext entry %08x in func %08x\n", e, f.start);
            for (u32 pc = a.codeStart; pc < a.codeEnd; pc += 4) {
                u32 w = m.read32(pc);
                if (mips::classify(w) == mips::Flow::JumpReg && mips::rs(w) != 31 && !a.jumpTables.count(pc))
                    printf("unresolved jr r%u at %08x\n", mips::rs(w), pc);
            }
        }
        return 0;
    }
    if (cmd == "recomp") {
        if (argc < 4) return usage();
        auto rep = translateGame(argv[2], argv[3], [](size_t d, size_t t, const std::string& s) {
            printf("\r%-48s %zu/%zu", s.c_str(), d, t);
        });
        if (!rep.ok) {
            fprintf(stderr, "\nfailed: %s\n", rep.error.c_str());
            return 1;
        }
        printf("\n%s [%s]: %zu functions, %zu instructions, %zu sources\n", rep.title.c_str(), rep.discId.c_str(),
               rep.functions, rep.instructions, rep.sources.size());
        return 0;
    }
    if (cmd == "dis") {
        LoadedModule m;
        if (!loadPspElf(elf, 0x08804000, m, &err) || argc < 5) return 1;
        u32 from = strtoul(argv[3], nullptr, 16), n = strtoul(argv[4], nullptr, 0);
        for (u32 pc = from; pc < from + n * 4; pc += 4) printf("%08x: %08x  %s\n", pc, m.read32(pc), disasm(pc, m.read32(pc)).c_str());
        return 0;
    }
    return usage();
}
