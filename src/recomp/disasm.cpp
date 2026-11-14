// Compact Allegrex disassembler (for diagnostics and comments in generated code).
#include "disasm.h"

#include <cstdio>

#include "mips.h"

namespace vc2 {
using namespace mips;

static const char* kReg[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                               "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                               "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

const char* regName(u32 r) { return kReg[r & 31]; }

std::string disasm(u32 pc, u32 i) {
    char b[96];
    const char *S = kReg[rs(i)], *T = kReg[rt(i)], *D = kReg[rd(i)];
    s32 si = simm16(i);
    u32 ui = imm16(i);
    auto r3 = [&](const char* m) { snprintf(b, sizeof b, "%s %s, %s, %s", m, D, S, T); };
    auto ri = [&](const char* m) { snprintf(b, sizeof b, "%s %s, %s, %d", m, T, S, si); };
    auto ru = [&](const char* m) { snprintf(b, sizeof b, "%s %s, %s, 0x%x", m, T, S, ui); };
    auto mem = [&](const char* m) { snprintf(b, sizeof b, "%s %s, %d(%s)", m, T, si, S); };
    auto fmem = [&](const char* m) { snprintf(b, sizeof b, "%s f%u, %d(%s)", m, rt(i), si, S); };
    auto br2 = [&](const char* m) { snprintf(b, sizeof b, "%s %s, %s, 0x%08x", m, S, T, branchTarget(pc, i)); };
    auto br1 = [&](const char* m) { snprintf(b, sizeof b, "%s %s, 0x%08x", m, S, branchTarget(pc, i)); };
    if (i == 0) return "nop";
    switch (op(i)) {
        case 0x00:
            switch (funct(i)) {
                case 0x00: snprintf(b, sizeof b, "sll %s, %s, %u", D, T, sa(i)); break;
                case 0x02: snprintf(b, sizeof b, "%s %s, %s, %u", rs(i) == 1 ? "rotr" : "srl", D, T, sa(i)); break;
                case 0x03: snprintf(b, sizeof b, "sra %s, %s, %u", D, T, sa(i)); break;
                case 0x04: snprintf(b, sizeof b, "sllv %s, %s, %s", D, T, S); break;
                case 0x06: snprintf(b, sizeof b, "%s %s, %s, %s", sa(i) == 1 ? "rotrv" : "srlv", D, T, S); break;
                case 0x07: snprintf(b, sizeof b, "srav %s, %s, %s", D, T, S); break;
                case 0x08: snprintf(b, sizeof b, "jr %s", S); break;
                case 0x09: snprintf(b, sizeof b, "jalr %s, %s", D, S); break;
                case 0x0A: r3("movz"); break;
                case 0x0B: r3("movn"); break;
                case 0x0C: snprintf(b, sizeof b, "syscall 0x%x", (i >> 6) & 0xFFFFF); break;
                case 0x0D: snprintf(b, sizeof b, "break 0x%x", (i >> 6) & 0xFFFFF); break;
                case 0x0F: return "sync";
                case 0x10: snprintf(b, sizeof b, "mfhi %s", D); break;
                case 0x11: snprintf(b, sizeof b, "mthi %s", S); break;
                case 0x12: snprintf(b, sizeof b, "mflo %s", D); break;
                case 0x13: snprintf(b, sizeof b, "mtlo %s", S); break;
                case 0x16: snprintf(b, sizeof b, "clz %s, %s", D, S); break;
                case 0x17: snprintf(b, sizeof b, "clo %s, %s", D, S); break;
                case 0x18: snprintf(b, sizeof b, "mult %s, %s", S, T); break;
                case 0x19: snprintf(b, sizeof b, "multu %s, %s", S, T); break;
                case 0x1A: snprintf(b, sizeof b, "div %s, %s", S, T); break;
                case 0x1B: snprintf(b, sizeof b, "divu %s, %s", S, T); break;
                case 0x1C: snprintf(b, sizeof b, "madd %s, %s", S, T); break;
                case 0x1D: snprintf(b, sizeof b, "maddu %s, %s", S, T); break;
                case 0x2E: snprintf(b, sizeof b, "msub %s, %s", S, T); break;
                case 0x2F: snprintf(b, sizeof b, "msubu %s, %s", S, T); break;
                case 0x20: r3("add"); break;
                case 0x21: r3("addu"); break;
                case 0x22: r3("sub"); break;
                case 0x23: r3("subu"); break;
                case 0x24: r3("and"); break;
                case 0x25: r3("or"); break;
                case 0x26: r3("xor"); break;
                case 0x27: r3("nor"); break;
                case 0x2A: r3("slt"); break;
                case 0x2B: r3("sltu"); break;
                case 0x2C: r3("max"); break;
                case 0x2D: r3("min"); break;
                default: snprintf(b, sizeof b, "special.%02x", funct(i));
            }
            return b;
        case 0x01: {
            static const char* n[32] = {"bltz", "bgez", "bltzl", "bgezl", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                        "bltzal", "bgezal", "bltzall", "bgezall"};
            if (n[rt(i)]) br1(n[rt(i)]);
            else snprintf(b, sizeof b, "regimm.%02x", rt(i));
            return b;
        }
        case 0x02: snprintf(b, sizeof b, "j 0x%08x", jumpTarget(pc, i)); return b;
        case 0x03: snprintf(b, sizeof b, "jal 0x%08x", jumpTarget(pc, i)); return b;
        case 0x04: br2("beq"); return b;
        case 0x05: br2("bne"); return b;
        case 0x06: br1("blez"); return b;
        case 0x07: br1("bgtz"); return b;
        case 0x08: ri("addi"); return b;
        case 0x09: ri("addiu"); return b;
        case 0x0A: ri("slti"); return b;
        case 0x0B: ri("sltiu"); return b;
        case 0x0C: ru("andi"); return b;
        case 0x0D: ru("ori"); return b;
        case 0x0E: ru("xori"); return b;
        case 0x0F: snprintf(b, sizeof b, "lui %s, 0x%x", T, ui); return b;
        case 0x11:
            switch (rs(i)) {
                case 0x00: snprintf(b, sizeof b, "mfc1 %s, f%u", T, rd(i)); return b;
                case 0x02: snprintf(b, sizeof b, "cfc1 %s, fcr%u", T, rd(i)); return b;
                case 0x04: snprintf(b, sizeof b, "mtc1 %s, f%u", T, rd(i)); return b;
                case 0x06: snprintf(b, sizeof b, "ctc1 %s, fcr%u", T, rd(i)); return b;
                case 0x08: {
                    static const char* n[4] = {"bc1f", "bc1t", "bc1fl", "bc1tl"};
                    snprintf(b, sizeof b, "%s 0x%08x", n[rt(i) & 3], branchTarget(pc, i));
                    return b;
                }
                case 0x10: {
                    static const char* n[64] = {"add.s", "sub.s", "mul.s", "div.s", "sqrt.s", "abs.s", "mov.s", "neg.s",
                                                0, 0, 0, 0, "round.w.s", "trunc.w.s", "ceil.w.s", "floor.w.s"};
                    u32 fn = funct(i);
                    if (fn < 16 && n[fn]) snprintf(b, sizeof b, "%s f%u, f%u, f%u", n[fn], sa(i), rd(i), rt(i));
                    else if (fn == 0x24) snprintf(b, sizeof b, "cvt.w.s f%u, f%u", sa(i), rd(i));
                    else if (fn >= 0x30) snprintf(b, sizeof b, "c.%u.s f%u, f%u", fn & 15, rd(i), rt(i));
                    else snprintf(b, sizeof b, "cop1.s.%02x", fn);
                    return b;
                }
                case 0x14:
                    if (funct(i) == 0x20) snprintf(b, sizeof b, "cvt.s.w f%u, f%u", sa(i), rd(i));
                    else snprintf(b, sizeof b, "cop1.w.%02x", funct(i));
                    return b;
            }
            snprintf(b, sizeof b, "cop1 0x%08x", i);
            return b;
        case 0x12:
            if (rs(i) == 0x08) {
                static const char* n[4] = {"bvf", "bvt", "bvfl", "bvtl"};
                snprintf(b, sizeof b, "%s %u, 0x%08x", n[(i >> 16) & 3], (i >> 18) & 7, branchTarget(pc, i));
                return b;
            }
            snprintf(b, sizeof b, "vfpu.cop2 0x%08x", i);
            return b;
        case 0x14: br2("beql"); return b;
        case 0x15: br2("bnel"); return b;
        case 0x16: br1("blezl"); return b;
        case 0x17: br1("bgtzl"); return b;
        case 0x1C:
            snprintf(b, sizeof b, "special2.%02x", funct(i));
            return b;
        case 0x1F:
            switch (funct(i)) {
                case 0x00: snprintf(b, sizeof b, "ext %s, %s, %u, %u", T, S, sa(i), rd(i) + 1); return b;
                case 0x04: snprintf(b, sizeof b, "ins %s, %s, %u, %u", T, S, sa(i), rd(i) + 1 - sa(i)); return b;
                case 0x20:
                    switch (sa(i)) {
                        case 0x02: snprintf(b, sizeof b, "wsbh %s, %s", D, T); return b;
                        case 0x03: snprintf(b, sizeof b, "wsbw %s, %s", D, T); return b;
                        case 0x10: snprintf(b, sizeof b, "seb %s, %s", D, T); return b;
                        case 0x14: snprintf(b, sizeof b, "bitrev %s, %s", D, T); return b;
                        case 0x18: snprintf(b, sizeof b, "seh %s, %s", D, T); return b;
                    }
            }
            snprintf(b, sizeof b, "special3 0x%08x", i);
            return b;
        case 0x20: mem("lb"); return b;
        case 0x21: mem("lh"); return b;
        case 0x22: mem("lwl"); return b;
        case 0x23: mem("lw"); return b;
        case 0x24: mem("lbu"); return b;
        case 0x25: mem("lhu"); return b;
        case 0x26: mem("lwr"); return b;
        case 0x28: mem("sb"); return b;
        case 0x29: mem("sh"); return b;
        case 0x2A: mem("swl"); return b;
        case 0x2B: mem("sw"); return b;
        case 0x2E: mem("swr"); return b;
        case 0x2F: return "cache";
        case 0x30: mem("ll"); return b;
        case 0x38: mem("sc"); return b;
        case 0x31: fmem("lwc1"); return b;
        case 0x39: fmem("swc1"); return b;
        default: break;
    }
    if (isVfpu(i)) {
        snprintf(b, sizeof b, "vfpu.%02x 0x%08x", op(i), i);
        return b;
    }
    snprintf(b, sizeof b, ".word 0x%08x", i);
    return b;
}

}  // namespace vc2
