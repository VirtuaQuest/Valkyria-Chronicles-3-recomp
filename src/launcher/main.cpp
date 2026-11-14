// VC3Launcher entry point (Valkyria Chronicles III Recompiled).
//   VC3Launcher.exe --version                         build information (version, commit, date, compiler)
//   VC3Launcher.exe                                   GUI
//   VC3Launcher.exe --iso <path> --out <dir> --build  headless recompile with console progress
// Options: --jobs N, --rebuild (ignore object cache), --verbose (print every compiler line)
// Debug:   --screenshot <file.png> [--screenshot-delay S] [--tab install|settings|about] [--autobuild] [--size WxH]
//          --cancel-after S (CLI: cancel the build after S seconds); env VC2_SIMULATE_NO_MSVC=1
// RecompLauncher (see recomplauncher.json; only these options change behaviour):
//   VC3Launcher.exe --iso <path> --out <dir> --prepare  windowless recompile, "progress 0.42 Text" lines
//   VC3Launcher.exe --iso <path> --out <dir> --play     start the built game, wait, return its exit code
//   exit codes 20 unsupported disc, 21 no C++ Build Tools, 22 not set up yet, 23 rebuild after update stopped
#include <windows.h>
#include <objbase.h>

#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app.h"
#include "builder.h"
#include "common/build_info.h"
#include "disc.h"
#include "toolchain.h"
#include "util.h"

using namespace vc2l;

namespace {

Builder* g_cliBuilder = nullptr;

BOOL WINAPI onCtrl(DWORD) {
    if (g_cliBuilder) g_cliBuilder->cancel();
    return TRUE;
}

void attachConsole() {
    // GUI-subsystem exe: inherited pipes/files work as-is; otherwise attach to the parent's console.
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    bool inherited = h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
    if (!inherited && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    SetConsoleOutputCP(CP_UTF8);
}

const char* levelTag(LogLevel l) {
    switch (l) {
        case LogLevel::Good: return " ok ";
        case LogLevel::Warn: return "warn";
        case LogLevel::Error: return "FAIL";
        case LogLevel::Detail: return "    ";
        default: return "info";
    }
}

int runCli(const BuildOptions& bo, bool verbose, double cancelAfter) {
    attachConsole();
    printf("VC3 Recompiled launcher %s - headless build\n", vc2::buildInfoLine().c_str());
    fflush(stdout);
    Builder b;
    g_cliBuilder = &b;
    SetConsoleCtrlHandler(onCtrl, TRUE);
    if (!b.start(bo)) return 1;
    size_t logEnd = 0;
    Stage lastStage = Stage::Idle;
    double lastPrint = -10;
    for (;;) {
        Sleep(100);
        std::vector<LogLine> lines;
        logEnd = b.fetchLog(logEnd, lines);
        BuildStatus st = b.status();
        if (cancelAfter > 0 && st.elapsed >= cancelAfter && !st.finished) {
            printf("  (debug: cancelling after %.0f s)\n", cancelAfter);
            b.cancel();
            cancelAfter = 0;
        }
        for (auto& l : lines) {
            // compile lines are frequent; show them only with --verbose (progress lines cover them)
            if (l.level == LogLevel::Detail && !verbose && l.text.rfind("[", 0) == 0) continue;
            printf("[%s] %5s  %s\n", levelTag(l.level), formatDuration(l.t).c_str(), l.text.c_str());
        }
        bool stageChanged = st.stage != lastStage;
        if (!st.finished && (stageChanged || st.elapsed - lastPrint >= 2.0)) {
            lastStage = st.stage;
            lastPrint = st.elapsed;
            char eta[32] = "--:--";
            if (st.eta >= 0) snprintf(eta, sizeof eta, "%s", formatDuration(st.eta).c_str());
            printf("  [%5.1f%%] %-22s %-44s elapsed %s  ETA %s\n", st.progress * 100.0f, stageLabel(st.stage),
                   st.detail.substr(0, 44).c_str(), formatDuration(st.elapsed).c_str(), eta);
        }
        fflush(stdout);
        if (st.finished) {
            printf("\nStage timings:");
            for (int i = (int)Stage::Toolchain; i < (int)Stage::Done; i++)
                printf("  %s %.1fs", stageLabel((Stage)i), st.stageTime[i]);
            printf("\nFiles: %d total, %d compiled, %d cached\n", st.filesTotal, st.filesCompiled, st.filesCached);
            if (st.ok) {
                printf("RESULT: OK%s in %s -> %s\n", st.upToDate ? " (up to date)" : "", formatDuration(st.elapsed).c_str(),
                       pathUtf8(st.exePath).c_str());
            } else {
                printf("RESULT: %s: %s\n", st.cancelled ? "CANCELLED" : "FAILED", st.error.c_str());
            }
            fflush(stdout);
            g_cliBuilder = nullptr;
            return st.ok ? 0 : (st.cancelled ? 2 : 1);
        }
    }
}

// ---------------------------------------------------------------- RecompLauncher (recomplauncher.json)
// Used only when RecompLauncher passes --prepare or --play; the GUI and --build are unchanged.
enum : int { kExitBadDisc = 20, kExitNoCompiler = 21, kExitNotSetUp = 22, kExitRebuildStopped = 23 };

// What the game executable was last built with: the launcher (translator) and the runtime SDK. A
// release update replaces both, so a different stamp means the game must be rebuilt before it runs.
std::string buildStamp() {
    std::string s;
    for (const fs::path& p : {launcherDir() / "VC3Launcher.exe", launcherDir() / "sdk" / "vc3runtime.lib"}) {
        std::error_code ec;
        const auto size = fs::file_size(p, ec);
        const auto time = fs::last_write_time(p, ec).time_since_epoch().count();
        s += pathUtf8(p.filename()) + " " + std::to_string(size) + " " + std::to_string(time) + "\n";
    }
    return s;
}
fs::path stampFile(const fs::path& out) { return out / "build" / "recomplauncher.stamp"; }
void writeStamp(const fs::path& out) {
    const std::string s = buildStamp();
    writeWholeFile(stampFile(out), s.data(), s.size());
}
bool stampCurrent(const fs::path& out) {
    std::vector<uint8_t> old;
    return readWholeFile(stampFile(out), old) && std::string(old.begin(), old.end()) == buildStamp();
}

// --prepare: the windowless setup step. Prints "progress <0..1> <text>" lines and exits 0 when the
// game executable is ready.
int runPrepare(const BuildOptions& bo) {
    attachConsole();
    DiscInfo d = probeDisc(bo.iso);
    if (!d.ok) {
        printf("error %s\n", d.error.c_str());
        return kExitBadDisc;
    }
    printf("progress 0.00 Looking for the Microsoft C++ Build Tools\n");
    fflush(stdout);
    auto tc = detectToolchain();
    if (!tc || !tc->ok) {
        printf("error %s\n", tc ? tc->error.c_str() : "No C++ compiler found.");
        return kExitNoCompiler;
    }
    Builder b;
    g_cliBuilder = &b;
    SetConsoleCtrlHandler(onCtrl, TRUE);
    if (!b.start(bo)) return 1;
    float lastProgress = -1;
    std::string lastText;
    for (;;) {
        Sleep(200);
        BuildStatus st = b.status();
        std::string text = std::string(stageLabel(st.stage)) + (st.detail.empty() ? "" : ": " + st.detail.substr(0, 60));
        if (st.progress - lastProgress >= 0.005f || text != lastText) {
            printf("progress %.3f %s\n", st.progress, text.c_str());
            fflush(stdout);
            lastProgress = st.progress;
            lastText = text;
        }
        if (!st.finished) continue;
        g_cliBuilder = nullptr;
        if (!st.ok) {
            printf("error %s\n", st.cancelled ? "Cancelled." : st.error.c_str());
            return st.cancelled ? 2 : 1;
        }
        writeStamp(bo.outDir);
        printf("progress 1.000 Ready to play\n");
        return 0;
    }
}

// Starts `cmdline` and waits for it. The child is closed with this process (job object).
int runAndWait(const fs::path& exe, std::wstring cmdline, const fs::path& cwd) {
    ProcessGroup group;
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, cwd.c_str(), &si,
                        &pi))
        return -1;
    if (group.job()) AssignProcessToJobObject(group.job(), pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}

// --play: starts the game built by --prepare and waits for it, returning its exit code. After a
// release update the launcher's own window rebuilds the game first (progress bar, closes when done).
int runPlay(const BuildOptions& bo, const std::vector<std::string>& gameArgs) {
    DiscInfo d = probeDisc(bo.iso);
    if (!d.ok) return kExitBadDisc;
    const fs::path exe = bo.outDir / pathFromUtf8(d.exeName);
    std::error_code ec;
    if (!fs::is_regular_file(exe, ec)) return kExitNotSetUp;
    if (!stampCurrent(bo.outDir)) {
        const fs::path self = launcherDir() / "VC3Launcher.exe";
        std::wstring cmd = quoteArg(self.wstring()) + L" --iso " + quoteArg(bo.iso.wstring()) + L" --out " +
                           quoteArg(bo.outDir.wstring()) + L" --autobuild --exit-when-built";
        if (runAndWait(self, cmd, launcherDir()) != 0) return kExitRebuildStopped;
        writeStamp(bo.outDir);
    }
    std::wstring cmd = quoteArg(exe.wstring()) + L" " + quoteArg(bo.iso.wstring());
    for (auto& a : gameArgs) cmd += L" " + quoteArg(pathFromUtf8(a).wstring());
    return runAndWait(exe, cmd, bo.outDir);
}

void usage() {
    attachConsole();
    printf(
        "VC3Launcher.exe                                    start the GUI\n"
        "VC3Launcher.exe --iso <file> --out <dir> --build   recompile headless\n"
        "VC3Launcher.exe --iso <file> --out <dir> --prepare recompile headless, RecompLauncher progress lines\n"
        "VC3Launcher.exe --iso <file> --out <dir> --play    start the game built in <dir> and wait for it\n"
        "VC3Launcher.exe --version                          print the build information\n"
        "    --jobs N      parallel compiler processes (default: CPU threads)\n"
        "    --rebuild     ignore the object cache\n"
        "    --verbose     print one line per compiled file\n"
        "Debug: --screenshot <png> [--screenshot-delay S] [--tab install|settings|about] [--autobuild] [--size WxH]\n");
}

}  // namespace

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    BuildOptions bo;
    GuiOptions go;
    bool build = false, verbose = false, prepare = false, play = false;
    std::vector<std::string> gameArgs;  // --play: unknown options go to the game (e.g. --debug)
    double cancelAfter = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--iso" || a == "--game") bo.iso = go.iso = pathFromUtf8(next());
        else if (a == "--out") bo.outDir = go.out = pathFromUtf8(next());
        else if (a == "--build") build = true;
        else if (a == "--prepare") prepare = true;
        else if (a == "--play") play = true;
        else if (a == "--exit-when-built") go.exitWhenBuilt = true;
        else if (a == "--jobs") bo.jobs = atoi(next().c_str());
        else if (a == "--rebuild") bo.forceRebuild = go.rebuild = true;
        else if (a == "--cancel-after") cancelAfter = atof(next().c_str());
        else if (a == "--verbose") verbose = true;
        else if (a == "--screenshot") go.screenshot = pathFromUtf8(next());
        else if (a == "--screenshot-delay") go.screenshotDelay = (float)atof(next().c_str());
        else if (a == "--tab") go.tab = next();
        else if (a == "--autobuild") go.autoBuild = true;
        else if (a == "--size") {
            std::string s = next();
            sscanf(s.c_str(), "%dx%d", &go.width, &go.height);
        } else if (a == "--version") {
            attachConsole();
            printf("%s", vc2::buildInfoText().c_str());
            return 0;
        } else if (a == "--help" || a == "-h" || a == "/?") {
            usage();
            return 0;
        } else if (play) {
            gameArgs.push_back(a);
        }
    }
    int rc;
    if (prepare || play) {
        if (bo.iso.empty() || bo.outDir.empty()) {
            usage();
            return 1;
        }
        rc = prepare ? runPrepare(bo) : runPlay(bo, gameArgs);
    } else if (build) {
        if (bo.iso.empty() || bo.outDir.empty()) {
            LauncherPrefs p = loadPrefs();
            if (bo.iso.empty()) bo.iso = p.iso;
            if (bo.outDir.empty()) bo.outDir = p.outDir.empty() ? defaultInstallDir() : p.outDir;
        }
        if (bo.iso.empty()) {
            usage();
            return 1;
        }
        rc = runCli(bo, verbose, cancelAfter);
    } else {
        rc = runGui(go);
    }
    // COM is intentionally left initialized: thread-local WIC factories are released at thread exit.
    return rc;
}
