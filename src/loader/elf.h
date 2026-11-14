#pragma once
// PSP ELF / PRX loading: segments, relocations, module info, import and export tables.
#include "../common/types.h"
#include <map>
#include <string>
#include <vector>

namespace vc2 {

struct ElfSegment {
    u32 vaddr = 0;      // relocated address
    u32 fileSize = 0;
    u32 memSize = 0;
    u32 flags = 0;      // PF_X=1 PF_W=2 PF_R=4
    Bytes data;         // memSize bytes, relocated
};

struct ElfSection {
    std::string name;
    u32 type = 0, flags = 0, addr = 0, offset = 0, size = 0;
};

struct ImportFunc {
    u32 nid = 0;
    u32 stubAddr = 0;   // address of the 8 byte stub the game calls
};
struct ImportVar {
    u32 nid = 0;
    u32 addr = 0;       // address of the variable reference table
};
struct ImportLib {
    std::string name;
    u16 version = 0, flags = 0;
    std::vector<ImportFunc> funcs;
    std::vector<ImportVar> vars;
};
struct ExportLib {
    std::string name;
    std::vector<std::pair<u32, u32>> funcs;  // nid, addr
    std::vector<std::pair<u32, u32>> vars;
};

struct LoadedModule {
    std::string name;
    u32 baseAddr = 0;
    u32 entry = 0;
    u32 gp = 0;
    u32 textStart = 0, textEnd = 0;   // executable range
    u32 imageEnd = 0;                 // highest address used (incl. bss)
    std::vector<ElfSegment> segments;
    std::vector<ElfSection> sections;
    std::vector<ImportLib> imports;
    std::vector<ExportLib> exports;
    u32 moduleStart = 0, moduleStop = 0;  // from exports (0xD632ACDB / 0xCEE8593C) or entry
    bool relocatable = false;

    // Pointer facts recovered from relocations (used by the recompiler's function discovery).
    struct PtrRef {
        u32 where;   // address of the relocated word / instruction
        u32 value;   // final pointer value
        bool data;   // true: R_MIPS_32 word in memory, false: lui/lo16 pair in code
    };
    std::vector<PtrRef> pointerRefs;

    // Reads a 32 bit word from the loaded image (0 if outside).
    u32 read32(u32 addr) const;
    const u8* ptr(u32 addr) const;
    std::string readString(u32 addr) const;
};

// Loads and relocates a decrypted PSP ELF at `base` (ignored for fixed ET_EXEC images).
bool loadPspElf(const Bytes& elf, u32 base, LoadedModule& out, std::string* err);

}  // namespace vc2
