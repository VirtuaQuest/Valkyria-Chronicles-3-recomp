#include "codegen.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "disasm.h"
#include "mips.h"

namespace vc2 {
using namespace mips;

namespace {

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string R(u32 r) { return r == 0 ? std::string("0u") : fmt("c->r[%u]", r); }
std::string SR(u32 r) { return r == 0 ? std::string("0") : fmt("(s32)c->r[%u]", r); }

class Emitter {
public:
    Emitter(const LoadedModule& m, const Analysis& a, bool comments, u32 importBase, const std::set<u32>* hooks = nullptr)
        : m_(m), a_(a), comments_(comments), importBase_(importBase), hooks_(hooks) {}

    std::string function(const Function& f) {
        out_.str("");
        out_.clear();
        fn_ = &f;
        labels_ = f.labels;
        // a label at a delay slot needs the following instruction labelled too
        for (u32 pc = f.start; pc < f.end; pc += 4) {
            Flow fl = classify(m_.read32(pc));
            if (hasDelaySlot(fl) && labels_.count(pc + 4)) labels_.insert(pc + 8);
        }
        out_ << "void f_" << hex(f.start) << "(PspContext* c) {\n";
        if (hooks_ && hooks_->count(f.start)) out_ << "  psp_hook(c, 0x" << hex(f.start) << "u);\n";
        for (u32 pc = f.start; pc < f.end;) {
            pc = emitAt(pc);
        }
        // fell off the end of the function
        u32 next = f.end;
        out_ << "  " << tailTo(next) << "\n";
        out_ << "}\n\n";
        return out_.str();
    }

private:
    static std::string hex(u32 v) { return fmt("%08x", v); }

    void label(u32 pc) {
        if (labels_.count(pc)) out_ << "L_" << hex(pc) << ":;\n";
    }

    std::string callTo(u32 t) {
        auto st = a_.stubs.find(t);
        if (st != a_.stubs.end()) return fmt("psp_hle_call(c, %u);", importBase_ + st->second.index);
        if (a_.isFunctionStart(t)) return fmt("f_%08x(c);", t);
        return fmt("psp_call_indirect(c, 0x%08xu);", t);
    }

    std::string tailTo(u32 t) {
        if (t >= fn_->start && t < fn_->end) return fmt("goto L_%08x;", t);
        return "{ " + callTo(t) + " return; }";
    }

    std::string gotoOrTail(u32 t) {
        if (t >= fn_->start && t < fn_->end) {
            labels_.insert(t);
            return fmt("goto L_%08x;", t);
        }
        return tailTo(t);
    }

    std::string condFor(u32 pc, u32 i) {
        switch (op(i)) {
            case 0x01:
                switch (rt(i)) {
                    case 0x00: case 0x02: case 0x10: case 0x12: return SR(rs(i)) + " < 0";
                    default: return SR(rs(i)) + " >= 0";
                }
            case 0x04: case 0x14: return rs(i) == rt(i) ? "1" : R(rs(i)) + " == " + R(rt(i));
            case 0x05: case 0x15: return rs(i) == rt(i) ? "0" : R(rs(i)) + " != " + R(rt(i));
            case 0x06: case 0x16: return SR(rs(i)) + " <= 0";
            case 0x07: case 0x17: return SR(rs(i)) + " > 0";
            case 0x11: return (rt(i) & 1) ? "FCOND(c)" : "!FCOND(c)";
            case 0x12: return fmt(((i >> 16) & 1) ? "VCC(c, %u)" : "!VCC(c, %u)", (i >> 18) & 7);
        }
        (void)pc;
        return "0";
    }

    // Emits the instruction at pc (and its delay slot if any). Returns next pc to emit.
    u32 emitAt(u32 pc) {
        label(pc);
        u32 i = m_.read32(pc);
        if (comments_) out_ << "  /* " << hex(pc) << ": " << disasm(pc, i) << " */\n";
        Flow fl = classify(i);
        if (!hasDelaySlot(fl)) {
            std::string s = simple(pc, i);
            if (!s.empty()) out_ << "  " << s << "\n";
            return pc + 4;
        }
        u32 d = m_.read32(pc + 4);
        std::string delay = (pc + 4 < fn_->end) ? simple(pc + 4, d) : std::string();
        if (hasDelaySlot(classify(d))) delay = fmt("psp_unhandled(c, 0x%08xu, 0x%08xu); /* branch in delay slot */", pc + 4, d);
        std::string ra = fmt("c->r[31] = 0x%08xu;", pc + 8);
        switch (fl) {
            case Flow::Branch: {
                u32 t = branchTarget(pc, i);
                std::string cond = condFor(pc, i);
                if (cond == "1") out_ << "  " << delay << " " << gotoOrTail(t) << "\n";
                else out_ << "  { int b_ = " << cond << "; " << delay << " if (b_) " << gotoOrTail(t) << " }\n";
                break;
            }
            case Flow::BranchLikely: {
                u32 t = branchTarget(pc, i);
                out_ << "  if (" << condFor(pc, i) << ") { " << delay << " " << gotoOrTail(t) << " }\n";
                break;
            }
            case Flow::BranchLink: {
                u32 t = branchTarget(pc, i);
                out_ << "  { int b_ = " << condFor(pc, i) << "; " << ra << " " << delay << " if (b_) " << callTo(t) << " }\n";
                break;
            }
            case Flow::BranchLinkLikely: {
                u32 t = branchTarget(pc, i);
                out_ << "  { int b_ = " << condFor(pc, i) << "; " << ra << " if (b_) { " << delay << " " << callTo(t) << " } }\n";
                break;
            }
            case Flow::Jump:
                out_ << "  " << delay << " " << gotoOrTail(jumpTarget(pc, i)) << "\n";
                break;
            case Flow::Call:
                out_ << "  " << ra << " " << delay << " " << callTo(jumpTarget(pc, i)) << "\n";
                break;
            case Flow::JumpReg: {
                if (rs(i) == 31) {
                    out_ << "  " << delay << " return;\n";
                    break;
                }
                out_ << "  { u32 t_ = " << R(rs(i)) << "; " << delay << "\n";
                auto jt = a_.jumpTables.find(pc);
                if (jt != a_.jumpTables.end() && !jt->second.targets.empty()) {
                    out_ << "    switch (t_) {\n";
                    std::set<u32> seen;
                    for (u32 t : jt->second.targets) {
                        if (!seen.insert(t).second) continue;
                        labels_.insert(t);
                        out_ << "      case 0x" << hex(t) << "u: goto L_" << hex(t) << ";\n";
                    }
                    out_ << "    }\n";
                }
                out_ << "    psp_jump_indirect(c, t_); return; }\n";
                break;
            }
            case Flow::CallReg: {
                std::string link = rd(i) ? fmt("c->r[%u] = 0x%08xu;", rd(i), pc + 8) : std::string();
                out_ << "  { u32 t_ = " << R(rs(i)) << "; " << link << " " << delay << " psp_call_indirect(c, t_); }\n";
                break;
            }
            default: break;
        }
        // Something branches into the delay slot: give it a standalone copy.
        if (labels_.count(pc + 4)) {
            out_ << "  goto L_" << hex(pc + 8) << ";\n";
            out_ << "L_" << hex(pc + 4) << ":;\n  " << delay << "\n";
            labels_.insert(pc + 8);
        }
        return pc + 8;
    }

    // Non-control-flow instruction. Returns C statement(s) or "" for nops.
    std::string simple(u32 pc, u32 i) {
        if (i == 0) return "";
        const u32 s = rs(i), t = rt(i), d = rd(i), sh = sa(i);
        const s32 si = simm16(i);
        const u32 ui = imm16(i);
        auto wr = [&](u32 reg, const std::string& expr) -> std::string {
            if (reg == 0) return "";
            return fmt("c->r[%u] = ", reg) + expr + ";";
        };
        auto ea = [&]() { return si == 0 ? R(s) : fmt("%s + 0x%08xu", R(s).c_str(), (u32)si); };
        auto unk = [&]() { return fmt("psp_unhandled(c, 0x%08xu, 0x%08xu);", pc, i); };
        if (isVfpu(i)) return fmt("c->pc = 0x%08xu; psp_vfpu(c, 0x%08xu);", pc, i);
        switch (op(i)) {
            case 0x00:
                switch (funct(i)) {
                    case 0x00: return wr(d, sh ? R(t) + fmt(" << %u", sh) : R(t));
                    case 0x02:
                        if (s == 1) return wr(d, fmt("ROTR(%s, %u)", R(t).c_str(), sh));
                        return wr(d, sh ? R(t) + fmt(" >> %u", sh) : R(t));
                    case 0x03: return wr(d, fmt("(u32)(%s >> %u)", SR(t).c_str(), sh));
                    case 0x04: return wr(d, R(t) + " << (" + R(s) + " & 31)");
                    case 0x06:
                        if (sh == 1) return wr(d, "ROTR(" + R(t) + ", " + R(s) + ")");
                        return wr(d, R(t) + " >> (" + R(s) + " & 31)");
                    case 0x07: return wr(d, "(u32)(" + SR(t) + " >> (" + R(s) + " & 31))");
                    case 0x0A: return d ? "if (" + R(t) + " == 0) " + wr(d, R(s)) : "";
                    case 0x0B: return d ? "if (" + R(t) + " != 0) " + wr(d, R(s)) : "";
                    case 0x0C: return fmt("psp_syscall(c, 0x%08xu, 0x%xu);", pc, (i >> 6) & 0xFFFFF);
                    case 0x0D: return fmt("psp_break(c, 0x%08xu, 0x%xu);", pc, (i >> 6) & 0xFFFFF);
                    case 0x0F: return "";
                    case 0x10: return wr(d, "c->hi");
                    case 0x11: return "c->hi = " + R(s) + ";";
                    case 0x12: return wr(d, "c->lo");
                    case 0x13: return "c->lo = " + R(s) + ";";
                    case 0x16: return wr(d, "CLZ(" + R(s) + ")");
                    case 0x17: return wr(d, "CLZ(~" + R(s) + ")");
                    case 0x18: return "SETHILO(c, (u64)((s64)" + SR(s) + " * (s64)" + SR(t) + "));";
                    case 0x19: return "SETHILO(c, (u64)" + R(s) + " * (u64)" + R(t) + ");";
                    case 0x1A: return "DIV(c, " + R(s) + ", " + R(t) + ");";
                    case 0x1B: return "DIVU(c, " + R(s) + ", " + R(t) + ");";
                    case 0x1C: return "SETHILO(c, HILO(c) + (u64)((s64)" + SR(s) + " * (s64)" + SR(t) + "));";
                    case 0x1D: return "SETHILO(c, HILO(c) + (u64)" + R(s) + " * (u64)" + R(t) + ");";
                    case 0x2E: return "SETHILO(c, HILO(c) - (u64)((s64)" + SR(s) + " * (s64)" + SR(t) + "));";
                    case 0x2F: return "SETHILO(c, HILO(c) - (u64)" + R(s) + " * (u64)" + R(t) + ");";
                    case 0x20: case 0x21: return wr(d, R(s) + " + " + R(t));
                    case 0x22: case 0x23: return wr(d, R(s) + " - " + R(t));
                    case 0x24: return wr(d, R(s) + " & " + R(t));
                    case 0x25:
                        if (t == 0) return wr(d, R(s));
                        if (s == 0) return wr(d, R(t));
                        return wr(d, R(s) + " | " + R(t));
                    case 0x26: return wr(d, R(s) + " ^ " + R(t));
                    case 0x27: return wr(d, "~(" + R(s) + " | " + R(t) + ")");
                    case 0x2A: return wr(d, "(u32)(" + SR(s) + " < " + SR(t) + ")");
                    case 0x2B: return wr(d, "(u32)(" + R(s) + " < " + R(t) + ")");
                    case 0x2C: return wr(d, "(" + SR(s) + " > " + SR(t) + ") ? " + R(s) + " : " + R(t));
                    case 0x2D: return wr(d, "(" + SR(s) + " < " + SR(t) + ") ? " + R(s) + " : " + R(t));
                    default: return unk();
                }
            case 0x08: case 0x09:
                return wr(t, si ? fmt("%s + 0x%08xu", R(s).c_str(), (u32)si) : R(s));
            case 0x0A: return wr(t, fmt("(u32)(%s < %d)", SR(s).c_str(), si));
            case 0x0B: return wr(t, fmt("(u32)(%s < 0x%08xu)", R(s).c_str(), (u32)si));
            case 0x0C: return wr(t, fmt("%s & 0x%xu", R(s).c_str(), ui));
            case 0x0D: return wr(t, ui ? fmt("%s | 0x%xu", R(s).c_str(), ui) : R(s));
            case 0x0E: return wr(t, fmt("%s ^ 0x%xu", R(s).c_str(), ui));
            case 0x0F: return wr(t, fmt("0x%08xu", ui << 16));
            case 0x10: return "";  // COP0: privileged, ignored
            case 0x11: return cop1(pc, i);
            case 0x1C:
                switch (funct(i)) {
                    case 0x24: return wr(t, "psp_mfic(c)");
                    case 0x26: return "psp_mtic(c, " + R(t) + ");";
                    default: return unk();
                }
            case 0x1F:
                switch (funct(i)) {
                    case 0x00: {
                        u32 size = d + 1;
                        u32 mask = size >= 32 ? 0xFFFFFFFFu : ((1u << size) - 1);
                        return wr(t, fmt("(%s >> %u) & 0x%08xu", R(s).c_str(), sh, mask));
                    }
                    case 0x04: {
                        u32 size = d + 1 - sh;
                        u32 mask = (size >= 32 ? 0xFFFFFFFFu : ((1u << size) - 1)) << sh;
                        return wr(t, fmt("(%s & 0x%08xu) | ((%s << %u) & 0x%08xu)", R(t).c_str(), ~mask, R(s).c_str(), sh, mask));
                    }
                    case 0x20:
                        switch (sh) {
                            case 0x02: return wr(d, "WSBH(" + R(t) + ")");
                            case 0x03: return wr(d, "WSBW(" + R(t) + ")");
                            case 0x10: return wr(d, "(u32)(s32)(s8)" + R(t));
                            case 0x14: return wr(d, "BITREV(" + R(t) + ")");
                            case 0x18: return wr(d, "(u32)(s32)(s16)" + R(t));
                        }
                }
                return unk();
            case 0x20: return t ? wr(t, "LB(" + ea() + ")") : "";
            case 0x21: return t ? wr(t, "LH(" + ea() + ")") : "";
            case 0x22: return t ? wr(t, "LWL(" + ea() + ", " + R(t) + ")") : "";
            case 0x23: case 0x30: return t ? wr(t, "LW(" + ea() + ")") : "";
            case 0x24: return t ? wr(t, "LBU(" + ea() + ")") : "";
            case 0x25: return t ? wr(t, "LHU(" + ea() + ")") : "";
            case 0x26: return t ? wr(t, "LWR(" + ea() + ", " + R(t) + ")") : "";
            case 0x28: return "SB(" + ea() + ", " + R(t) + ");";
            case 0x29: return "SH(" + ea() + ", " + R(t) + ");";
            case 0x2A: return "SWL(" + ea() + ", " + R(t) + ");";
            case 0x2B: return "SW(" + ea() + ", " + R(t) + ");";
            case 0x2E: return "SWR(" + ea() + ", " + R(t) + ");";
            case 0x2F: return "";  // cache
            case 0x38: return "SW(" + ea() + ", " + R(t) + "); " + wr(t, "1u");
            case 0x31: return fmt("c->fi[%u] = LW(%s);", t, ea().c_str());
            case 0x39: return fmt("SW(%s, c->fi[%u]);", ea().c_str(), t);
        }
        return unk();
    }

    std::string cop1(u32 pc, u32 i) {
        const u32 fs = rd(i), ft = rt(i), fd = sa(i), t = rt(i);
        switch (rs(i)) {
            case 0x00: return t ? fmt("c->r[%u] = c->fi[%u];", t, fs) : "";
            case 0x02:
                if (!t) return "";
                if (fs == 31) return fmt("c->r[%u] = c->fcr31;", t);
                if (fs == 0) return fmt("c->r[%u] = 0x00003351u;", t);
                return fmt("c->r[%u] = 0;", t);
            case 0x04: return fmt("c->fi[%u] = %s;", fs, R(t).c_str());
            case 0x06:
                if (fs == 31) return fmt("c->fcr31 = %s & 0x0183FFFFu;", R(t).c_str());
                return "";
            case 0x10:
                switch (funct(i)) {
                    case 0x00: return fmt("c->f[%u] = c->f[%u] + c->f[%u];", fd, fs, ft);
                    case 0x01: return fmt("c->f[%u] = c->f[%u] - c->f[%u];", fd, fs, ft);
                    case 0x02: return fmt("c->f[%u] = c->f[%u] * c->f[%u];", fd, fs, ft);
                    case 0x03: return fmt("c->f[%u] = c->f[%u] / c->f[%u];", fd, fs, ft);
                    case 0x04: return fmt("c->f[%u] = sqrtf(c->f[%u]);", fd, fs);
                    case 0x05: return fmt("c->fi[%u] = c->fi[%u] & 0x7FFFFFFFu;", fd, fs);
                    case 0x06: return fmt("c->fi[%u] = c->fi[%u];", fd, fs);
                    case 0x07: return fmt("c->fi[%u] = c->fi[%u] ^ 0x80000000u;", fd, fs);
                    case 0x0C: return fmt("c->fi[%u] = FROUNDW(c->f[%u]);", fd, fs);
                    case 0x0D: return fmt("c->fi[%u] = FTRUNCW(c->f[%u]);", fd, fs);
                    case 0x0E: return fmt("c->fi[%u] = FCEILW(c->f[%u]);", fd, fs);
                    case 0x0F: return fmt("c->fi[%u] = FFLOORW(c->f[%u]);", fd, fs);
                    case 0x24: return fmt("c->fi[%u] = FCVTWS(c, c->f[%u]);", fd, fs);
                    default:
                        if (funct(i) >= 0x30) return fmt("FCMP(c, c->f[%u], c->f[%u], %uu);", fs, ft, funct(i) & 7);
                }
                break;
            case 0x14:
                if (funct(i) == 0x20) return fmt("c->f[%u] = (float)(s32)c->fi[%u];", fd, fs);
                break;
        }
        return fmt("psp_unhandled(c, 0x%08xu, 0x%08xu);", pc, i);
    }

    const LoadedModule& m_;
    const Analysis& a_;
    bool comments_;
    u32 importBase_;
    const std::set<u32>* hooks_ = nullptr;
    const Function* fn_ = nullptr;
    std::set<u32> labels_;
    std::ostringstream out_;
};

bool writeFile(const std::filesystem::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    f.write(s.data(), (std::streamsize)s.size());
    return (bool)f;
}

}  // namespace

RecompResult generateC(const std::vector<ModuleUnit>& units, const RecompOptions& opt, const ProgressFn& progress) {
    RecompResult res;
    std::error_code ec;
    std::filesystem::create_directories(opt.outDir, ec);
    if (ec) {
        res.error = "cannot create output directory: " + ec.message();
        return res;
    }
    const std::string banner =
        "/* Generated by vc2recomp from the user's own game disc. Do not distribute. */\n";

    size_t totalFuncs = 0, totalInstrs = 0;
    for (auto& u : units) {
        totalFuncs += u.analysis->functions.size();
        totalInstrs += u.analysis->instructionCount;
    }

    // prototypes
    {
        std::string h = banner + "#pragma once\n#include \"psp_recomp.h\"\n";
        for (auto& u : units)
            for (auto& f : u.analysis->functions) h += fmt("void f_%08x(PspContext* c);\n", f.start);
        // Import stubs get thunks too: games may take the address of an imported function.
        for (auto& u : units)
            for (auto& [addr, s] : u.analysis->stubs) h += fmt("void f_%08x(PspContext* c);\n", addr);
        if (!writeFile(opt.outDir / "funcs.h", h)) {
            res.error = "write failed: funcs.h";
            return res;
        }
    }

    // function bodies, split into roughly equal chunks by instruction count
    const size_t perFile = (totalInstrs + opt.fileCount - 1) / opt.fileCount;
    size_t done = 0, fileIdx = 0, importBase = 0;
    std::string body;
    size_t instrs = 0;
    auto flush = [&]() -> bool {
        if (body.empty()) return true;
        auto path = opt.outDir / fmt("code_%03zu.c", fileIdx++);
        if (!writeFile(path, banner + "#include \"funcs.h\"\n\n" + body)) {
            res.error = "write failed: " + path.string();
            return false;
        }
        res.sources.push_back(path);
        body.clear();
        instrs = 0;
        return true;
    };
    for (auto& u : units) {
        Emitter em(*u.module, *u.analysis, opt.comments, (u32)importBase, &opt.hooks);
        for (auto& f : u.analysis->functions) {
            body += em.function(f);
            instrs += (f.end - f.start) / 4;
            if (++done % 256 == 0 && progress) progress(done, totalFuncs, "Translating MIPS functions to C");
            if (instrs >= perFile && !flush()) return res;
        }
        importBase += u.analysis->stubs.size();
    }
    if (!flush()) return res;

    // tables
    {
        std::string t = banner + "#include \"funcs.h\"\n\n";
        size_t stubBase = 0, totalStubs = 0;
        for (auto& u : units) {
            for (auto& [addr, s] : u.analysis->stubs)
                t += fmt("void f_%08x(PspContext* c) { psp_hle_call(c, %zuu); }\n", addr, stubBase + s.index);
            stubBase += u.analysis->stubs.size();
            totalStubs += u.analysis->stubs.size();
        }
        t += "\nconst PspFuncEntry psp_func_table[] = {\n";
        for (auto& u : units)
            for (auto& f : u.analysis->functions)
                t += fmt("  {0x%08xu, 0x%xu, f_%08x},\n", f.start, f.end - f.start, f.start);
        for (auto& u : units)
            for (auto& [addr, s] : u.analysis->stubs) t += fmt("  {0x%08xu, 0x8u, f_%08x},\n", addr, addr);
        t += "};\n";
        t += fmt("const u32 psp_func_table_count = %zuu;\n\n", totalFuncs + totalStubs);
        t += "const PspImportEntry psp_import_table[] = {\n";
        size_t nImports = 0;
        for (auto& u : units) {
            std::vector<const ImportStub*> stubs(u.analysis->stubs.size());
            for (auto& [addr, s] : u.analysis->stubs) stubs[s.index] = &s;
            for (auto* s : stubs) t += fmt("  {\"%s\", 0x%08xu, 0x%08xu},\n", s->lib.c_str(), s->nid, s->addr);
            nImports += stubs.size();
        }
        t += "  {0, 0, 0}\n};\n";
        t += fmt("const u32 psp_import_table_count = %zuu;\n\n", nImports);
        t += "const PspModuleEntry psp_module_table[] = {\n";
        for (auto& u : units)
            t += fmt("  {\"%s\", \"%s\", 0x%08xu, \"%s\"},\n", u.module->name.c_str(), u.discPath.c_str(),
                     u.module->baseAddr, u.sha1.c_str());
        t += "};\n";
        t += fmt("const u32 psp_module_table_count = %zuu;\n", units.size());
        auto path = opt.outDir / "tables.c";
        if (!writeFile(path, t)) {
            res.error = "write failed: tables.c";
            return res;
        }
        res.sources.push_back(path);
    }
    if (progress) progress(totalFuncs, totalFuncs, "Translation complete");
    res.functions = totalFuncs;
    res.instructions = totalInstrs;
    res.ok = true;
    return res;
}

}  // namespace vc2
