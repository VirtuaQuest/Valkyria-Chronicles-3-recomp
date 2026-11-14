#pragma once
// Allegrex (PSP MIPS32 + extensions) instruction field helpers and control-flow classification.
#include "../common/types.h"

namespace vc2::mips {

inline u32 op(u32 i) { return i >> 26; }
inline u32 rs(u32 i) { return (i >> 21) & 31; }
inline u32 rt(u32 i) { return (i >> 16) & 31; }
inline u32 rd(u32 i) { return (i >> 11) & 31; }
inline u32 sa(u32 i) { return (i >> 6) & 31; }
inline u32 funct(u32 i) { return i & 63; }
inline u32 imm16(u32 i) { return i & 0xFFFF; }
inline s32 simm16(u32 i) { return (s32)(s16)(i & 0xFFFF); }
inline u32 branchTarget(u32 pc, u32 i) { return pc + 4 + ((u32)simm16(i) << 2); }
inline u32 jumpTarget(u32 pc, u32 i) { return ((pc + 4) & 0xF0000000) | ((i & 0x03FFFFFF) << 2); }

enum class Flow {
    None,         // ordinary instruction
    Branch,       // conditional PC-relative branch (has delay slot)
    BranchLikely, // conditional, delay slot nullified when not taken
    BranchLink,   // conditional branch-and-link (bltzal...)
    BranchLinkLikely,
    Jump,         // j
    Call,         // jal
    JumpReg,      // jr
    CallReg,      // jalr
    Stop,         // break / halt etc.
};

inline Flow classify(u32 i) {
    switch (op(i)) {
        case 0x00:
            switch (funct(i)) {
                case 0x08: return Flow::JumpReg;
                case 0x09: return Flow::CallReg;
                case 0x0D: return Flow::Stop;
                default: return Flow::None;
            }
        case 0x01:
            switch (rt(i)) {
                case 0x00: case 0x01: return Flow::Branch;
                case 0x02: case 0x03: return Flow::BranchLikely;
                case 0x10: case 0x11: return Flow::BranchLink;
                case 0x12: case 0x13: return Flow::BranchLinkLikely;
                default: return Flow::None;
            }
        case 0x02: return Flow::Jump;
        case 0x03: return Flow::Call;
        case 0x04: case 0x05: case 0x06: case 0x07: return Flow::Branch;
        case 0x14: case 0x15: case 0x16: case 0x17: return Flow::BranchLikely;
        case 0x11:  // COP1 bc1x
            if (rs(i) == 0x08) return (rt(i) & 2) ? Flow::BranchLikely : Flow::Branch;
            return Flow::None;
        case 0x12:  // COP2 (VFPU) bvf/bvt
            if (rs(i) == 0x08) return ((i >> 17) & 1) ? Flow::BranchLikely : Flow::Branch;
            return Flow::None;
        default: return Flow::None;
    }
}

inline bool hasDelaySlot(Flow f) { return f != Flow::None && f != Flow::Stop; }
inline bool isConditional(Flow f) {
    return f == Flow::Branch || f == Flow::BranchLikely || f == Flow::BranchLink || f == Flow::BranchLinkLikely;
}

// VFPU opcodes handled by the runtime VFPU interpreter (everything except branches).
inline bool isVfpu(u32 i) {
    switch (op(i)) {
        case 0x18: case 0x19: case 0x1B: case 0x32: case 0x34: case 0x35: case 0x36: case 0x37:
        case 0x3A: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
            return true;
        case 0x12: return rs(i) != 0x08;
        default: return false;
    }
}

}  // namespace vc2::mips
