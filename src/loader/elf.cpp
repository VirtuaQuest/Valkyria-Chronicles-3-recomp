#include "elf.h"
#include <algorithm>
#include <cstring>

namespace vc2 {

namespace {
enum : u32 {
    ET_EXEC = 2,
    ET_PRX = 0xFFA0,
    PT_LOAD = 1,
    PT_PSPREL = 0x700000A0,
    PT_PSPREL2 = 0x700000A1,
    SHT_REL = 9,
    SHT_PSPREL = 0x700000A0,
    R_MIPS_NONE = 0,
    R_MIPS_16 = 1,
    R_MIPS_32 = 2,
    R_MIPS_26 = 4,
    R_MIPS_HI16 = 5,
    R_MIPS_LO16 = 6,
    R_MIPS_GPREL16 = 7,
};

struct Rel {
    u32 offset, info;
};
}  // namespace

const u8* LoadedModule::ptr(u32 addr) const {
    for (auto& s : segments)
        if (addr >= s.vaddr && addr < s.vaddr + s.memSize) return &s.data[addr - s.vaddr];
    return nullptr;
}
u32 LoadedModule::read32(u32 addr) const {
    for (auto& s : segments)
        if (addr >= s.vaddr && addr + 4 <= s.vaddr + s.memSize) return rd32(&s.data[addr - s.vaddr]);
    return 0;
}
std::string LoadedModule::readString(u32 addr) const {
    std::string r;
    for (int i = 0; i < 256; i++) {
        const u8* p = ptr(addr + i);
        if (!p || !*p) break;
        r.push_back((char)*p);
    }
    return r;
}

static void applyRelocs(const std::vector<Rel>& rels, std::vector<ElfSegment>& segs, const std::vector<u32>& segBase,
                        const std::vector<u32>& origVaddr, std::vector<LoadedModule::PtrRef>& refs) {
    auto wordAt = [&](u32 segIdx, u32 off) -> u8* {
        if (segIdx >= segs.size()) return nullptr;
        // r_offset is relative to the segment's linked address
        if (off + 4 > segs[segIdx].memSize) return nullptr;
        return &segs[segIdx].data[off];
    };
    // snapshot original ops so HI16 can pair with an unpatched LO16
    std::vector<u32> orig(rels.size(), 0);
    for (size_t i = 0; i < rels.size(); i++) {
        u8* p = wordAt((rels[i].info >> 8) & 0xFF, rels[i].offset);
        orig[i] = p ? rd32(p) : 0;
    }
    for (size_t i = 0; i < rels.size(); i++) {
        u32 type = rels[i].info & 0xFF;
        u32 ofsSeg = (rels[i].info >> 8) & 0xFF;
        u32 valSeg = (rels[i].info >> 16) & 0xFF;
        u8* p = wordAt(ofsSeg, rels[i].offset);
        if (!p || valSeg >= segBase.size()) continue;
        u32 relocTo = segBase[valSeg];
        (void)origVaddr;
        u32 op = orig[i];
        switch (type) {
            case R_MIPS_32:
                op += relocTo;
                refs.push_back({segBase[ofsSeg] + rels[i].offset, op, true});
                break;
            case R_MIPS_26: {
                u32 tgt = ((op & 0x03FFFFFF) << 2) + relocTo;
                op = (op & 0xFC000000) | ((tgt >> 2) & 0x03FFFFFF);
                break;
            }
            case R_MIPS_HI16: {
                u32 cur = (op & 0xFFFF) << 16;
                bool found = false;
                for (size_t j = i + 1; j < rels.size(); j++) {
                    u32 t2 = rels[j].info & 0xFF;
                    if (t2 == R_MIPS_LO16 || t2 == R_MIPS_16) {
                        cur += (u32)(s32)(s16)(orig[j] & 0xFFFF);
                        found = true;
                        break;
                    }
                }
                if (found) {
                    cur += relocTo;
                    refs.push_back({segBase[ofsSeg] + rels[i].offset, cur, false});
                    u32 hi = (cur >> 16) + ((cur & 0x8000) ? 1 : 0);
                    op = (op & 0xFFFF0000) | (hi & 0xFFFF);
                }
                break;
            }
            case R_MIPS_LO16:
            case R_MIPS_16: op = (op & 0xFFFF0000) | ((op + relocTo) & 0xFFFF); break;
            default: break;
        }
        wr32(p, op);
    }
}

bool loadPspElf(const Bytes& elf, u32 base, LoadedModule& out, std::string* err) {
    auto fail = [&](const char* m) {
        if (err) *err = m;
        return false;
    };
    if (elf.size() < 0x34 || memcmp(elf.data(), "\x7F""ELF", 4) != 0) return fail("not an ELF");
    const u8* h = elf.data();
    u16 type = rd16(h + 16);
    u32 entry = rd32(h + 24), phoff = rd32(h + 28), shoff = rd32(h + 32);
    u16 phentsize = rd16(h + 42), phnum = rd16(h + 44), shentsize = rd16(h + 46), shnum = rd16(h + 48),
        shstrndx = rd16(h + 50);
    out.relocatable = (type == ET_PRX);
    if (!out.relocatable) base = 0;
    out.baseAddr = base;

    std::vector<u32> segBase, origVaddr;
    std::vector<Rel> rels;
    u32 firstLoadPaddr = 0;
    bool haveFirst = false;
    for (u16 i = 0; i < phnum; i++) {
        const u8* ph = h + phoff + i * phentsize;
        if (phoff + (i + 1) * phentsize > elf.size()) return fail("bad program header");
        u32 ptype = rd32(ph), off = rd32(ph + 4), va = rd32(ph + 8), pa = rd32(ph + 12), fsz = rd32(ph + 16),
            msz = rd32(ph + 20), flg = rd32(ph + 24);
        if (ptype == PT_LOAD) {
            if (!haveFirst) {
                firstLoadPaddr = pa;
                haveFirst = true;
            }
            ElfSegment s;
            s.vaddr = va + base;
            s.fileSize = fsz;
            s.memSize = msz;
            s.flags = flg;
            s.data.assign(msz, 0);
            if (off + fsz > elf.size()) return fail("segment outside file");
            memcpy(s.data.data(), h + off, fsz);
            out.segments.push_back(std::move(s));
            segBase.push_back(va + base);
            origVaddr.push_back(va);
        } else if (ptype == PT_PSPREL) {
            for (u32 o = 0; o + 8 <= fsz; o += 8) rels.push_back({rd32(h + off + o), rd32(h + off + o + 4)});
        } else if (ptype == PT_PSPREL2) {
            return fail("type-B relocations not supported yet");
        }
    }

    // sections (names are informational; relocations may live here instead of in program headers)
    if (shoff && shnum && shoff + shnum * shentsize <= elf.size()) {
        const u8* strsh = h + shoff + shstrndx * shentsize;
        u32 strOff = rd32(strsh + 16);
        bool relFromPh = !rels.empty();
        for (u16 i = 0; i < shnum; i++) {
            const u8* sh = h + shoff + i * shentsize;
            ElfSection s;
            u32 nameOff = rd32(sh);
            if (strOff + nameOff < elf.size()) s.name = (const char*)(h + strOff + nameOff);
            s.type = rd32(sh + 4);
            s.flags = rd32(sh + 8);
            s.addr = rd32(sh + 12) + base;
            s.offset = rd32(sh + 16);
            s.size = rd32(sh + 20);
            out.sections.push_back(s);
            if (!relFromPh && (s.type == SHT_PSPREL || s.type == SHT_REL) && s.offset + s.size <= elf.size())
                for (u32 o = 0; o + 8 <= s.size; o += 8)
                    rels.push_back({rd32(h + s.offset + o), rd32(h + s.offset + o + 4)});
        }
    }
    if (out.relocatable) applyRelocs(rels, out.segments, segBase, origVaddr, out.pointerRefs);

    out.entry = entry + base;
    out.textStart = 0xFFFFFFFF;
    out.textEnd = 0;
    for (auto& s : out.segments) {
        out.imageEnd = std::max(out.imageEnd, s.vaddr + s.memSize);
        if (s.flags & 1) {
            out.textStart = std::min(out.textStart, s.vaddr);
            out.textEnd = std::max(out.textEnd, s.vaddr + s.fileSize);
        }
    }
    for (auto& s : out.sections)
        if (s.name == ".text") {
            out.textStart = s.addr;
            out.textEnd = s.addr + s.size;
        }

    // module info: located at first PT_LOAD p_paddr (file offset relative to segment) for PRX,
    // or in the .rodata.sceModuleInfo section.
    u32 modInfo = 0;
    for (auto& s : out.sections)
        if (s.name == ".rodata.sceModuleInfo") modInfo = s.addr;
    if (!modInfo && haveFirst && !out.segments.empty()) {
        // p_paddr & 0x7FFFFFFF is the file offset; convert using the first segment
        const u8* ph0 = h + phoff;
        u32 off0 = rd32(ph0 + 4), va0 = rd32(ph0 + 8);
        modInfo = (firstLoadPaddr & 0x7FFFFFFF) - off0 + va0 + base;
    }
    if (!out.ptr(modInfo)) return fail("module info not found");
    // SceModuleInfo: u16 attr, u16 ver, char name[28], u32 gp, u32 ent_top, ent_end, stub_top, stub_end
    out.name = std::string((const char*)out.ptr(modInfo + 4), strnlen((const char*)out.ptr(modInfo + 4), 28));
    out.gp = out.read32(modInfo + 32);
    u32 entTop = out.read32(modInfo + 36), entEnd = out.read32(modInfo + 40);
    u32 stubTop = out.read32(modInfo + 44), stubEnd = out.read32(modInfo + 48);

    for (u32 a = stubTop; a < stubEnd;) {
        ImportLib lib;
        u32 nameP = out.read32(a);
        lib.name = nameP ? out.readString(nameP) : "";
        lib.version = (u16)out.read32(a + 4);
        lib.flags = (u16)(out.read32(a + 4) >> 16);
        u32 w2 = out.read32(a + 8);
        u8 entSize = (u8)(w2 & 0xFF);
        u8 varCount = (u8)((w2 >> 8) & 0xFF);
        u16 funcCount = (u16)(w2 >> 16);
        u32 nids = out.read32(a + 12), funcs = out.read32(a + 16), vars = entSize >= 6 ? out.read32(a + 20) : 0;
        for (u32 i = 0; i < funcCount; i++) lib.funcs.push_back({out.read32(nids + i * 4), funcs + i * 8});
        for (u32 i = 0; i < varCount; i++) lib.vars.push_back({out.read32(vars + i * 8 + 4), out.read32(vars + i * 8)});
        out.imports.push_back(std::move(lib));
        if (entSize < 5) break;
        a += entSize * 4;
    }
    out.moduleStart = out.entry;
    for (u32 a = entTop; a < entEnd;) {
        ExportLib lib;
        u32 nameP = out.read32(a);
        lib.name = nameP ? out.readString(nameP) : "";
        u32 w2 = out.read32(a + 8);
        u8 entSize = (u8)(w2 & 0xFF);
        u8 varCount = (u8)((w2 >> 8) & 0xFF);
        u16 funcCount = (u16)(w2 >> 16);
        u32 tbl = out.read32(a + 12);
        u32 total = funcCount + varCount;
        for (u32 i = 0; i < funcCount; i++) lib.funcs.push_back({out.read32(tbl + i * 4), out.read32(tbl + (total + i) * 4)});
        for (u32 i = 0; i < varCount; i++)
            lib.vars.push_back({out.read32(tbl + (funcCount + i) * 4), out.read32(tbl + (total + funcCount + i) * 4)});
        if (lib.name.empty())
            for (auto& [nid, addr] : lib.funcs) {
                if (nid == 0xD632ACDB) out.moduleStart = addr;
                if (nid == 0xCEE8593C) out.moduleStop = addr;
            }
        out.exports.push_back(std::move(lib));
        if (entSize < 4) break;
        a += entSize * 4;
    }
    return true;
}

}  // namespace vc2
