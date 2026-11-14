#include "analysis.h"

#include <algorithm>
#include <cstdio>

#include "mips.h"

namespace vc2 {

using namespace mips;

const Function* Analysis::functionAt(u32 addr) const {
    auto it = std::upper_bound(functions.begin(), functions.end(), addr,
                               [](u32 a, const Function& f) { return a < f.start; });
    if (it == functions.begin()) return nullptr;
    --it;
    return (addr >= it->start && addr < it->end) ? &*it : nullptr;
}

bool Analysis::isFunctionStart(u32 addr) const {
    const Function* f = functionAt(addr);
    return f && f->start == addr;
}

bool analyzeModule(const LoadedModule& m, Analysis& a) {
    a.codeStart = m.textStart;
    a.codeEnd = m.textEnd;
    auto inCode = [&](u32 x) { return x >= a.codeStart && x < a.codeEnd && (x & 3) == 0; };
    auto word = [&](u32 x) { return m.read32(x); };

    u32 idx = 0;
    for (auto& lib : m.imports)
        for (auto& f : lib.funcs) a.stubs[f.stubAddr] = {f.stubAddr, f.nid, lib.name, idx++};

    // Index relocation facts.
    std::map<u32, u32> codeRefAt;   // lui address -> full value
    std::map<u32, u32> dataRefAt;   // word address -> value
    for (auto& r : m.pointerRefs) {
        if (r.data)
            dataRefAt[r.where] = r.value;
        else
            codeRefAt[r.where] = r.value;
    }

    a.instructionCount = (a.codeEnd - a.codeStart) / 4;

    // ---- jump tables
    for (u32 pc = a.codeStart; pc < a.codeEnd; pc += 4) {
        u32 in = word(pc);
        if (classify(in) != Flow::JumpReg || rs(in) == 31) continue;
        u32 reg = rs(in);
        u32 table = 0, count = 0;
        for (u32 back = 1; back <= 24 && pc >= a.codeStart + back * 4; back++) {
            u32 p = pc - back * 4, w = word(p);
            if (op(w) == 0x0F) {  // lui
                auto it = codeRefAt.find(p);
                if (it != codeRefAt.end() && !inCode(it->second) && !table) table = it->second;
            }
            if (op(w) == 0x0B && !count) count = imm16(w);  // sltiu bound
            if (classify(w) == Flow::JumpReg || classify(w) == Flow::Jump) break;
            if (table && count) break;
        }
        if (!table) {
            a.unresolvedJr++;
            continue;
        }
        JumpTable jt;
        jt.jrAddr = pc;
        jt.tableAddr = table;
        u32 limit = count ? count : 4096;
        for (u32 k = 0; k < limit; k++) {
            auto it = dataRefAt.find(table + k * 4);
            if (it == dataRefAt.end() || !inCode(it->second)) break;
            jt.targets.push_back(it->second);
        }
        if (jt.targets.empty()) {
            a.unresolvedJr++;
            continue;
        }
        for (u32 t : jt.targets) a.jumpTableTargets.insert(t);
        a.jumpTables[pc] = std::move(jt);
    }

    // ---- function starts. "Strong" starts are certain (called, exported, entry); "weak" starts come
    // from relocated pointers and may turn out to be labels inside a function.
    std::set<u32> strong, starts;
    if (inCode(m.entry)) strong.insert(m.entry);
    if (inCode(m.moduleStart)) strong.insert(m.moduleStart);
    for (auto& e : m.exports)
        for (auto& [nid, addr] : e.funcs)
            if (inCode(addr)) strong.insert(addr);
    for (u32 pc = a.codeStart; pc < a.codeEnd; pc += 4) {
        u32 in = word(pc);
        if (classify(in) == Flow::Call) {
            u32 t = jumpTarget(pc, in);
            if (inCode(t)) strong.insert(t);
        }
    }
    strong.insert(a.codeStart);
    starts = strong;
    for (auto& [where, v] : dataRefAt)
        if (inCode(v) && !a.jumpTableTargets.count(v)) starts.insert(v);
    for (auto& [where, v] : codeRefAt)
        if (inCode(v) && !a.jumpTableTargets.count(v)) starts.insert(v);

    const auto originalTables = a.jumpTables;
    for (int iter = 0; iter < 16; iter++) {
        a.functions.clear();
        a.warnings.clear();
        a.jumpTables = originalTables;
        a.externalBranches = 0;

        // ---- partition
        std::vector<u32> sv(starts.begin(), starts.end());
        for (size_t i = 0; i < sv.size(); i++) {
            Function f;
            f.start = sv[i];
            f.end = i + 1 < sv.size() ? sv[i + 1] : a.codeEnd;
            a.functions.push_back(std::move(f));
        }

        // ---- truncate jump tables at the first entry outside the function containing the jr
        for (auto& [jr, jt] : a.jumpTables) {
            const Function* f = a.functionAt(jr);
            if (!f) continue;
            size_t k = 0;
            while (k < jt.targets.size() && jt.targets[k] >= f->start && jt.targets[k] < f->end) k++;
            if (k < jt.targets.size()) {
                char buf[128];
                snprintf(buf, sizeof buf, "jump table at %08x (jr %08x) truncated %zu -> %zu", jt.tableAddr, jr,
                         jt.targets.size(), k);
                a.warnings.push_back(buf);
                jt.targets.resize(k);
            }
        }

        // ---- labels & external edges
        std::map<u32, std::set<u32>> extEntries;  // function start -> entries
        auto edge = [&](Function& f, u32 t) {
            if (t >= f.start && t < f.end) {
                f.labels.insert(t);
            } else if (!a.isFunctionStart(t)) {
                const Function* tf = a.functionAt(t);
                if (tf) extEntries[tf->start].insert(t);
                a.externalBranches++;
            }
        };
        for (auto& f : a.functions) {
            for (u32 pc = f.start; pc < f.end; pc += 4) {
                u32 in = word(pc);
                Flow fl = classify(in);
                if (isConditional(fl)) edge(f, branchTarget(pc, in));
                else if (fl == Flow::Jump) edge(f, jumpTarget(pc, in));
                auto jt = a.jumpTables.find(pc);
                if (jt != a.jumpTables.end())
                    for (u32 t : jt->second.targets) edge(f, t);
            }
        }
        // Merge weak starts that are entered mid-body: the target function absorbs into its predecessor.
        bool changed = false;
        for (auto& [fs, ents] : extEntries)
            if (!strong.count(fs)) {
                starts.erase(fs);
                changed = true;
            }
        if (changed) continue;
        for (auto& f : a.functions) {
            auto it = extEntries.find(f.start);
            if (it != extEntries.end()) {
                f.externalEntries.assign(it->second.begin(), it->second.end());
                for (u32 t : it->second) f.labels.insert(t);
            }
        }
        break;
    }
    return true;
}

}  // namespace vc2
