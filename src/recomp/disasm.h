#pragma once
#include <string>

#include "../common/types.h"

namespace vc2 {
std::string disasm(u32 pc, u32 insn);
const char* regName(u32 r);
}  // namespace vc2
