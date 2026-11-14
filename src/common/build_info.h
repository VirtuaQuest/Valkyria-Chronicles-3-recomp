#pragma once
// Build information compiled into the launcher and the runtime. The definition is generated on every
// build by cmake/BuildInfo.cmake (build/generated/build_info.cpp).
#include <string>

namespace vc2 {

struct BuildInfo {
    const char* product;     // "Valkyria Chronicles III Recompiled"
    const char* version;     // release tag (v1.2.0), git describe (v1.2.0-3-gabc1234), or v<base>-dev+<commit>
    const char* commit;      // short git commit, "unknown" outside a git checkout
    const char* commitFull;
    const char* branch;
    const char* commitDate;  // YYYY-MM-DD
    const char* buildDate;   // YYYY-MM-DD HH:MM UTC
    const char* compiler;    // e.g. "MSVC 19.44.35207.0"
    const char* buildType;   // Release, Debug, ...
    bool dirty;              // built with uncommitted changes
};

const BuildInfo& buildInfo();

// One line: "v1.2.0 (commit abc1234, built 2026-11-13 20:00 UTC)"
inline std::string buildInfoLine() {
    const BuildInfo& b = buildInfo();
    return std::string(b.version) + " (commit " + b.commit + (b.dirty ? "+changes" : "") + ", built " + b.buildDate + ")";
}

// Several lines, for "copy build information" and --version.
inline std::string buildInfoText() {
    const BuildInfo& b = buildInfo();
    return std::string(b.product) + "\n" +
           "Version:   " + b.version + "\n" +
           "Commit:    " + b.commitFull + (b.dirty ? " (with uncommitted changes)" : "") + "\n" +
           "Branch:    " + b.branch + "\n" +
           "Committed: " + b.commitDate + "\n" +
           "Built:     " + b.buildDate + "\n" +
           "Compiler:  " + b.compiler + " (" + b.buildType + ")\n";
}

}  // namespace vc2
