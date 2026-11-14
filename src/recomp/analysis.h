#pragma once
// Static control-flow analysis: function discovery, jump table recovery.
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../loader/elf.h"

namespace vc2 {

struct JumpTable {
    u32 jrAddr = 0;        // address of the jr instruction
    u32 tableAddr = 0;
    std::vector<u32> targets;
};

struct Function {
    u32 start = 0, end = 0;               // [start, end)
    std::set<u32> labels;                 // in-function branch targets
    std::vector<u32> externalEntries;     // addresses entered from outside (non-start)
};

struct ImportStub {
    u32 addr = 0;
    u32 nid = 0;
    std::string lib;
    u32 index = 0;   // global import index
};

struct Analysis {
    u32 codeStart = 0, codeEnd = 0;
    std::vector<Function> functions;           // sorted by start
    std::map<u32, ImportStub> stubs;           // by stub address
    std::map<u32, JumpTable> jumpTables;       // by jr address
    std::set<u32> jumpTableTargets;
    // statistics / diagnostics
    size_t instructionCount = 0;
    size_t unresolvedJr = 0;
    size_t externalBranches = 0;
    std::vector<std::string> warnings;

    const Function* functionAt(u32 addr) const;  // function containing addr
    bool isFunctionStart(u32 addr) const;
};

bool analyzeModule(const LoadedModule& m, Analysis& out);

}  // namespace vc2
