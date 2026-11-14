#include "builder.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>

#include "common/build_info.h"
#include "disc.h"
#include "imaging.h"
#include "recomp/pipeline.h"
#include "toolchain.h"
#include "util.h"

namespace vc2l {

namespace {

struct Cancelled {};

// Compiler flags for the generated C. They mirror the CMake `vc2game` developer target and are part of the
// object-cache key, so changing anything here invalidates the cache automatically.
const wchar_t* kClFlags =
    L"/nologo /c /TC /O2 /Ob2 /MT /utf-8 /W0 /Zc:preprocessor /DWIN32 /D_WINDOWS /DNDEBUG /DNOMINMAX "
    L"/DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS";
const wchar_t* kLinkFlags =
    L"/nologo /machine:x64 /INCREMENTAL:NO /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /STACK:8388608 /MANIFEST:EMBED";
const wchar_t* kLinkLibs =
    L"kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib "
    L"advapi32.lib";
const std::vector<int> kIcoSizes = {256, 64, 48, 32, 16};

// Rough expected durations (seconds, 8 cores) used to weight the progress bar before real numbers are known.
constexpr double kCompileCpuSecPerMB = 24.0;  // measured: cl /O2 needs ~24-26 s per MB of generated C per core

std::string readTail(const fs::path& file, size_t maxLines) {
    std::vector<uint8_t> data;
    if (!readWholeFile(file, data)) return {};
    std::string s(data.begin(), data.end());
    std::vector<std::string> lines;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find('\n', i);
        if (j == std::string::npos) j = s.size();
        std::string l = s.substr(i, j - i);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!l.empty()) lines.push_back(l);
        i = j + 1;
    }
    std::string out;
    size_t start = lines.size() > maxLines ? lines.size() - maxLines : 0;
    for (size_t k = start; k < lines.size(); k++) out += lines[k] + "\n";
    if (!out.empty()) out.pop_back();
    return out;
}

bool insideGitRepo(const fs::path& dir, fs::path& repo) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(dir, ec);
    if (ec) p = dir;
    for (fs::path cur = p; !cur.empty(); cur = cur.parent_path()) {
        if (fs::exists(cur / ".git", ec)) {
            repo = cur;
            return true;
        }
        if (cur == cur.parent_path()) break;
    }
    return false;
}

bool sameFile(const fs::path& a, const fs::path& b) {
    std::error_code e1, e2;
    if (!fs::exists(b, e2)) return false;
    return fs::file_size(a, e1) == fs::file_size(b, e2) && !e1 && !e2 &&
           fs::last_write_time(a, e1) == fs::last_write_time(b, e2) && !e1 && !e2;
}

bool writeUtf16File(const fs::path& file, const std::wstring& text) {
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    const unsigned char bom[2] = {0xFF, 0xFE};
    f.write((const char*)bom, 2);
    f.write((const char*)text.data(), (std::streamsize)(text.size() * sizeof(wchar_t)));
    return (bool)f;
}

}  // namespace

const char* stageLabel(Stage s) {
    switch (s) {
        case Stage::Idle: return "Ready";
        case Stage::Toolchain: return "Locating MSVC";
        case Stage::Validate: return "Validating disc";
        case Stage::Decrypt: return "Loading & analyzing";
        case Stage::Translate: return "Translating to C";
        case Stage::Compile: return "Compiling";
        case Stage::Resource: return "Building icon";
        case Stage::Link: return "Linking";
        case Stage::Install: return "Installing runtime";
        case Stage::Done: return "Done";
        default: return "";
    }
}

static std::wstring g_gameExeName = L"VC3Recompiled.exe";
const wchar_t* gameExeName() { return g_gameExeName.c_str(); }
void setGameExeName(const std::string& utf8Name) {
    if (!utf8Name.empty()) g_gameExeName = std::wstring(utf8Name.begin(), utf8Name.end());
}
fs::path gameExePath(const fs::path& outDir) { return outDir / gameExeName(); }

Builder::~Builder() {
    cancel();
    join();
}

bool Builder::start(const BuildOptions& opt) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (st_.running) return false;
    }
    join();
    std::lock_guard<std::mutex> lk(mtx_);
    opt_ = opt;
    if (opt_.runtimeDir.empty()) opt_.runtimeDir = launcherDir();
    if (opt_.jobs <= 0) opt_.jobs = (int)hardwareThreads();
    opt_.jobs = std::clamp(opt_.jobs, 1, 64);
    st_ = BuildStatus{};
    st_.running = true;
    st_.jobs = opt_.jobs;
    st_.exePath = gameExePath(opt_.outDir);
    log_.clear();
    cancel_ = false;
    t0_ = std::chrono::steady_clock::now();
    stageStart_ = 0;
    shown_ = 0;
    for (auto& f : frac_) f = 0;
    for (auto& n : nominal_) n = 0;
    nominal_[(int)Stage::Toolchain] = toolchainIfReady() ? 0.05 : 2.0;
    nominal_[(int)Stage::Validate] = 0.3;
    nominal_[(int)Stage::Decrypt] = 0.8;
    nominal_[(int)Stage::Translate] = 1.2;
    // guess until the cache has been checked: incremental if an object cache exists
    std::error_code ec;
    bool haveCache = fs::is_directory(opt_.outDir / "build" / "obj", ec) && !fs::is_empty(opt_.outDir / "build" / "obj", ec);
    nominal_[(int)Stage::Compile] = haveCache && !opt_.forceRebuild ? 2.0 : 115.0;
    nominal_[(int)Stage::Resource] = 0.4;
    nominal_[(int)Stage::Link] = 3.0;
    nominal_[(int)Stage::Install] = 0.2;
    thread_ = std::thread(&Builder::run, this);
    return true;
}

void Builder::cancel() { cancel_ = true; }

void Builder::join() {
    if (thread_.joinable()) thread_.join();
}

BuildStatus Builder::status() const {
    std::lock_guard<std::mutex> lk(mtx_);
    BuildStatus s = st_;
    s.elapsed = st_.running ? now() : st_.elapsed;
    return s;
}

size_t Builder::fetchLog(size_t from, std::vector<LogLine>& out) const {
    std::lock_guard<std::mutex> lk(mtx_);
    for (size_t i = from; i < log_.size(); i++) out.push_back(log_[i]);
    return log_.size();
}

size_t Builder::logSize() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return log_.size();
}

double Builder::now() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count(); }

void Builder::log(LogLevel lv, std::string text) {
    std::lock_guard<std::mutex> lk(mtx_);
    log_.push_back({lv, now(), std::move(text)});
}

void Builder::setStage(Stage s, const std::string& detail) {
    if (cancel_) throw Cancelled{};
    std::lock_guard<std::mutex> lk(mtx_);
    double t = now();
    if (st_.stage != Stage::Idle) {
        st_.stageTime[(int)st_.stage] = t - stageStart_;
        for (int i = 0; i < (int)s; i++) frac_[i] = 1.0f;
    }
    st_.stage = s;
    st_.detail = detail;
    stageStart_ = t;
    recompute();
}

void Builder::setFraction(float f, const std::string* detail) {
    if (cancel_) throw Cancelled{};
    std::lock_guard<std::mutex> lk(mtx_);
    frac_[(int)st_.stage] = std::clamp(f, 0.0f, 1.0f);
    if (detail) st_.detail = *detail;
    recompute();
}

void Builder::recompute() {
    double total = 0, done = 0;
    for (int i = 1; i < (int)Stage::Done; i++) {
        total += nominal_[i];
        done += nominal_[i] * frac_[i];
    }
    float p = total > 0 ? (float)(done / total) : 0;
    shown_ = std::max(shown_, std::min(p, 0.999f));
    st_.progress = shown_;
    // ETA: remaining nominal time of later stages + the current stage extrapolated from its observed rate
    int cur = (int)st_.stage;
    if (cur <= 0 || cur >= (int)Stage::Done) {
        st_.eta = -1;
        return;
    }
    double t = now();
    double inStage = t - stageStart_;
    double f = frac_[cur];
    double curRemain = nominal_[cur] * (1 - f);
    if (f > 0.04 && inStage > 1.5) curRemain = inStage / f * (1 - f);
    double later = 0;
    for (int i = cur + 1; i < (int)Stage::Done; i++) later += nominal_[i];
    st_.eta = curRemain + later;
}

void Builder::run() {
    bool ok = false;
    std::string err;
    bool cancelled = false;
    try {
        ok = runStages();
    } catch (const Cancelled&) {
        cancelled = true;
    } catch (const std::exception& e) {
        err = std::string("Unexpected error: ") + e.what();
    }
    if (cancelled || cancel_) log(LogLevel::Warn, "Build cancelled.");
    else if (!err.empty()) log(LogLevel::Error, err);
    std::lock_guard<std::mutex> lk(mtx_);
    double t = now();
    if (st_.stage != Stage::Idle && st_.stage != Stage::Done) st_.stageTime[(int)st_.stage] = t - stageStart_;
    st_.elapsed = t;
    st_.finished = true;
    st_.running = false;
    st_.cancelled = cancelled || (bool)cancel_;
    st_.ok = ok && !st_.cancelled;
    if (!err.empty() && st_.error.empty()) st_.error = err;
    if (st_.cancelled) st_.error = "Cancelled.";
    if (st_.ok) {
        st_.stage = Stage::Done;
        st_.progress = 1.0f;
        st_.detail = st_.upToDate ? "Everything is up to date" : "Build complete";
    }
    st_.eta = 0;
}

bool Builder::runStages() {
    auto fail = [&](const std::string& e) {
        log(LogLevel::Error, e);
        std::lock_guard<std::mutex> lk(mtx_);
        st_.error = e;
        return false;
    };
    log(LogLevel::Info, "VC3 Recompiled launcher " + vc2::buildInfoLine());
    const fs::path out = opt_.outDir;
    const fs::path buildDir = out / "build";
    const fs::path genDir = buildDir / "src";
    const fs::path objDir = buildDir / "obj";
    const fs::path logDir = buildDir / "logs";
    const fs::path resDir = buildDir / "res";
    const fs::path sdkDir = opt_.runtimeDir / "sdk";
    std::error_code ec;

    log(LogLevel::Info, "Disc image: " + pathUtf8(opt_.iso));
    log(LogLevel::Info, "Install folder: " + pathUtf8(out));

    // ---------------------------------------------------------------- toolchain
    setStage(Stage::Toolchain, "Looking for the Microsoft C++ compiler");
    auto tc = detectToolchain();
    if (!tc->ok) return fail(tc->error);
    log(LogLevel::Good, "MSVC " + tc->msvcVersion + " found (" + pathUtf8(tc->vsInstall) + ")");
    log(LogLevel::Detail, "cl: " + pathUtf8(tc->cl));
    if (tc->rc.empty()) log(LogLevel::Warn, "rc.exe (Windows SDK) not found: the game executable will have no icon.");

    // ---------------------------------------------------------------- validate
    setStage(Stage::Validate, "Checking the disc image");
    if (out.empty()) return fail("No install folder selected.");
    fs::path repo;
    if (insideGitRepo(out, repo))
        return fail("The install folder is inside a source repository (" + pathUtf8(repo) +
                    "). Choose a folder outside it so files derived from your disc never end up in version control.");
    for (const char* f : {"vc3runtime.lib", "psp_recomp.h", "vc2_runtime.h"})
        if (!fs::is_regular_file(sdkDir / f, ec))
            return fail(std::string("The runtime SDK is incomplete: ") + pathUtf8(sdkDir / f) +
                        " is missing. Reinstall the launcher package.");
    if (!fs::is_regular_file(opt_.runtimeDir / "vc3runtime.dll", ec))
        return fail("vc3runtime.dll is missing next to the launcher (" + pathUtf8(opt_.runtimeDir) + ").");
    DiscInfo disc = probeDisc(opt_.iso);
    setGameExeName(disc.exeName);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        st_.exePath = gameExePath(opt_.outDir);
    }
    if (!disc.ok) return fail(disc.error);
    log(LogLevel::Good, "Disc OK: " + disc.title + " [" + disc.discId + "] " + formatBytes(disc.fileSize));
    for (const fs::path& d : {out, buildDir, genDir, objDir, logDir, resDir}) {
        fs::create_directories(d, ec);
        if (ec) return fail("Cannot create folder " + pathUtf8(d) + ": " + ec.message());
    }

    // ---------------------------------------------------------------- load + analyze + translate
    setStage(Stage::Decrypt, "Loading the game executable");
    std::string lastDetail;
    bool translating = false;
    // The pipeline reports "Opening/Loading/Analyzing <module>" (k of modules) and then
    // "Translating MIPS functions to C" (functions done of total). Throwing from the callback aborts it.
    auto progress = [&](size_t done, size_t total, const std::string& what) {
        if (cancel_) throw Cancelled{};
        if (!translating &&
            (what.rfind("Loading", 0) == 0 || what.rfind("Analyzing", 0) == 0 || what.rfind("Opening", 0) == 0)) {
            float f = total ? (float)(done + (what[0] == 'A' ? 0.5f : 0.0f)) / (float)total : 0.0f;
            if (what != lastDetail) log(LogLevel::Detail, what);
            lastDetail = what;
            setFraction(f, &what);
            return;
        }
        if (!translating) {
            translating = true;
            setStage(Stage::Translate, "Translating MIPS functions to C");
        }
        setFraction(total ? (float)done / (float)total : 0.0f, &what);
    };
    vc2::TranslateReport rep = vc2::translateGame(opt_.iso, genDir, progress);
    if (!rep.ok) return fail(rep.error.empty() ? "Translation failed." : rep.error);
    if (!translating) setStage(Stage::Translate, "Translation complete");
    setFraction(1.0f);
    log(LogLevel::Good, "Translated " + std::to_string(rep.functions) + " functions (" + std::to_string(rep.instructions) +
                            " MIPS instructions) into " + std::to_string(rep.sources.size()) + " C files");
    // drop stale generated files from earlier runs with a different split
    {
        std::set<fs::path> keep(rep.sources.begin(), rep.sources.end());
        for (auto& e : fs::directory_iterator(genDir, ec))
            if (e.path().extension() == ".c" && !keep.count(e.path())) fs::remove(e.path(), ec);
    }

    // ---------------------------------------------------------------- compile (cached, parallel)
    setStage(Stage::Compile, "Checking the object cache");
    // leftovers of an interrupted build (killed launcher, power loss) are never valid cache entries
    for (auto& e : fs::directory_iterator(objDir, ec)) {
        std::wstring fn = e.path().filename().wstring();
        if (fn.size() > 8 && fn.compare(fn.size() - 8, 8, L".tmp.obj") == 0) fs::remove(e.path(), ec);
    }
    struct Unit {
        fs::path src, obj, tmp, log;
        std::string name;
        uint64_t size = 0;
    };
    std::string headerHash;
    {
        Sha256 h;
        h.updateFile(genDir / "funcs.h");
        h.updateFile(sdkDir / "psp_recomp.h");
        h.updateFile(sdkDir / "vc2_runtime.h");
        headerHash = h.hex();
    }
    std::string flagsKey = narrow(kClFlags) + "|" + pathUtf8(tc->cl) + "|" + tc->msvcVersion + "|" + tc->sdkVersion;
    std::vector<Unit> all, todo;
    std::set<fs::path> liveObjs;
    uint64_t todoBytes = 0;
    for (auto& src : rep.sources) {
        Unit u;
        u.src = src;
        u.name = pathUtf8(src.filename());
        u.size = fs::file_size(src, ec);
        Sha256 h;
        h.update(flagsKey);
        h.update(headerHash);
        h.update(u.name);
        if (!h.updateFile(src)) return fail("Cannot read " + pathUtf8(src));
        std::string key = h.hex().substr(0, 20);
        std::wstring stem = src.stem().wstring();
        u.obj = objDir / (stem + L"-" + widen(key) + L".obj");
        u.tmp = objDir / (stem + L"-" + widen(key) + L".tmp.obj");
        u.log = logDir / (stem + L".log");
        liveObjs.insert(u.obj);
        if (opt_.forceRebuild || !fs::is_regular_file(u.obj, ec)) {
            todo.push_back(u);
            todoBytes += u.size;
        }
        all.push_back(u);
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        st_.filesTotal = (int)all.size();
        st_.filesToCompile = (int)todo.size();
        st_.filesCached = (int)(all.size() - todo.size());
        int par = std::min<int>(opt_.jobs, (int)std::max<size_t>(1, todo.size()));
        nominal_[(int)Stage::Compile] = todo.empty() ? 0.05 : (todoBytes / 1048576.0) * kCompileCpuSecPerMB / par + 1.0;
        recompute();
    }
    if (todo.empty()) {
        log(LogLevel::Good, "All " + std::to_string(all.size()) + " object files are up to date (cache hit).");
    } else {
        log(LogLevel::Info, "Compiling " + std::to_string(todo.size()) + " of " + std::to_string(all.size()) + " files (" +
                                formatBytes(todoBytes) + " of C) with " + std::to_string(std::min<size_t>(opt_.jobs, todo.size())) +
                                " parallel cl.exe processes; " + std::to_string(all.size() - todo.size()) + " cached");
        // biggest first for better load balancing
        std::stable_sort(todo.begin(), todo.end(), [](const Unit& a, const Unit& b) { return a.size > b.size; });
        ProcessGroup group;
        struct Running {
            size_t idx;
            ChildProcess proc;
            double start;
        };
        std::vector<Running> running;
        size_t next = 0;
        int completed = 0;
        uint64_t bytesDone = 0;
        auto closeAll = [&]() {
            group.terminateAll();
            for (auto& r : running) {
                WaitForSingleObject(r.proc.process, 5000);
                closeProcess(r.proc);
            }
            running.clear();
            // Partial objects: the killed compilers' file handles (or an AV scanner) may linger briefly.
            for (int attempt = 0; attempt < 40; attempt++) {
                bool left = false;
                for (auto& u : todo)
                    if (fs::exists(u.tmp, ec) && !fs::remove(u.tmp, ec)) left = true;
                if (!left) break;
                Sleep(50);
            }
        };
        // progress updates must not throw here: cancellation is handled at the top of the loop so that
        // closeAll() can delete the partial objects of the killed compilers
        auto compileProgress = [&](const std::string* d) {
            try {
                setFraction(todoBytes ? (float)((double)bytesDone / (double)todoBytes) : 1.0f, d);
            } catch (const Cancelled&) {
            }
        };
        while (next < todo.size() || !running.empty()) {
            if (cancel_) {
                closeAll();
                throw Cancelled{};
            }
            while ((int)running.size() < opt_.jobs && next < todo.size()) {
                const Unit& u = todo[next];
                std::wstring cmd = quoteArg(tc->cl.wstring()) + L" " + kClFlags + L" /I" + quoteArg(genDir.wstring()) +
                                   L" /I" + quoteArg(sdkDir.wstring()) + L" /Fo" + quoteArg(u.tmp.wstring()) + L" " +
                                   quoteArg(u.src.wstring());
                Running r{next, {}, now()};
                std::string err;
                if (!spawnProcess(cmd, &tc->envBlock, buildDir, u.log, group, r.proc, &err)) {
                    closeAll();
                    return fail("Could not start cl.exe: " + err);
                }
                running.push_back(r);
                next++;
            }
            {
                std::lock_guard<std::mutex> lk(mtx_);
                st_.activeJobs = (int)running.size();
            }
            std::vector<HANDLE> hs;
            for (auto& r : running) hs.push_back((HANDLE)r.proc.process);
            DWORD w = WaitForMultipleObjects((DWORD)hs.size(), hs.data(), FALSE, 100);
            if (w == WAIT_TIMEOUT) {
                compileProgress(nullptr);  // refreshes the ETA
                continue;
            }
            if (w == WAIT_FAILED) {
                closeAll();
                return fail("Waiting for the compiler failed: " + lastErrorString(GetLastError()));
            }
            size_t k = w - WAIT_OBJECT_0;
            Running r = running[k];
            running.erase(running.begin() + (long long)k);
            DWORD code = 1;
            GetExitCodeProcess((HANDLE)r.proc.process, &code);
            closeProcess(r.proc);
            const Unit& u = todo[r.idx];
            if (code != 0) {
                std::string tail = readTail(u.log, 25);
                closeAll();
                log(LogLevel::Error, tail);
                return fail("Compiling " + u.name + " failed (exit code " + std::to_string(code) + "). Full log: " +
                            pathUtf8(u.log));
            }
            fs::rename(u.tmp, u.obj, ec);
            if (ec) {
                closeAll();
                return fail("Cannot store " + pathUtf8(u.obj) + ": " + ec.message());
            }
            completed++;
            bytesDone += u.size;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                st_.filesCompiled = completed;
            }
            char line[256];
            snprintf(line, sizeof line, "[%d/%zu] %s  %.1f s", completed, todo.size(), u.name.c_str(), now() - r.start);
            log(LogLevel::Detail, line);
            std::string d = "Compiled " + std::to_string(completed) + " of " + std::to_string(todo.size()) + " files";
            compileProgress(&d);
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            st_.activeJobs = 0;
        }
        log(LogLevel::Good, "Compiled " + std::to_string(todo.size()) + " files in " + formatDuration(now() - stageStart_));
    }
    // prune objects that no longer belong to the current sources
    for (auto& e : fs::directory_iterator(objDir, ec))
        if (e.path().extension() == ".obj" && !liveObjs.count(e.path())) fs::remove(e.path(), ec);

    // ---------------------------------------------------------------- icon resource
    setStage(Stage::Resource, "Converting the disc icon");
    fs::path resFile;
    std::string resKey = "none";
    if (!tc->rc.empty() && !disc.iconPng.empty()) {
        Image icon;
        std::string err;
        fs::path ico = resDir / "game.ico", rcFile = resDir / "game.rc", res = resDir / "game.res";
        if (!decodeImage(disc.iconPng.data(), disc.iconPng.size(), icon, &err)) {
            log(LogLevel::Warn, "The disc icon could not be decoded (" + err + "); continuing without an icon.");
        } else if (!writeIcoFromImage(icon, kIcoSizes, ico, &err)) {
            log(LogLevel::Warn, "Could not write the icon (" + err + "); continuing without an icon.");
        } else {
            std::string rc =
                "// Generated by the launcher at recompile time. Lives only in the install folder.\n"
                "1 ICON \"game.ico\"\n"
                "1 VERSIONINFO\nFILEVERSION 1,0,0,0\nPRODUCTVERSION 1,0,0,0\nFILEOS 0x40004\nFILETYPE 0x1\nBEGIN\n"
                "  BLOCK \"StringFileInfo\"\n  BEGIN\n    BLOCK \"040904b0\"\n    BEGIN\n"
                "      VALUE \"FileDescription\", \"PSP static recompilation (built locally from your own disc)\"\n"
                "      VALUE \"ProductName\", \"Valkyria Chronicles Recompiled\"\n"
                "      VALUE \"OriginalFilename\", \"" + narrow(gameExeName()) + "\"\n"
                "      VALUE \"FileVersion\", \"1.0.0.0\"\n"
                "    END\n  END\n  BLOCK \"VarFileInfo\"\n  BEGIN\n    VALUE \"Translation\", 0x409, 1200\n  END\nEND\n";
            writeWholeFile(rcFile, rc.data(), rc.size());
            std::wstring cmd = quoteArg(tc->rc.wstring()) + L" /nologo /fo game.res game.rc";
            ProcessGroup group;
            ChildProcess p;
            if (!spawnProcess(cmd, &tc->envBlock, resDir, logDir / "rc.log", group, p, &err)) {
                log(LogLevel::Warn, "Could not run rc.exe (" + err + "); continuing without an icon.");
            } else {
                int code = waitProcess(p, cancel_, group);
                closeProcess(p);
                if (cancel_) throw Cancelled{};
                if (code == 0 && fs::is_regular_file(res, ec)) {
                    resFile = res;
                    Sha256 h;
                    h.updateFile(ico);
                    h.update(rc);
                    resKey = h.hex();
                    log(LogLevel::Good, "Icon resource built from the disc's ICON0 (" + std::to_string(icon.w) + "x" +
                                            std::to_string(icon.h) + " -> 256/64/48/32/16)");
                } else {
                    log(LogLevel::Warn, "rc.exe failed; continuing without an icon:\n" + readTail(logDir / "rc.log", 10));
                }
            }
        }
    } else if (disc.iconPng.empty()) {
        log(LogLevel::Warn, "The disc has no ICON0.PNG; the executable will use the default icon.");
    }

    // ---------------------------------------------------------------- link
    setStage(Stage::Link, "Linking the game executable");
    const fs::path exe = gameExePath(out);
    const fs::path runtimeLib = sdkDir / "vc3runtime.lib";
    std::string linkKey;
    {
        Sha256 h;
        h.update(narrow(kLinkFlags));
        h.update(narrow(kLinkLibs));
        h.update(pathUtf8(tc->link));
        for (auto& u : all) h.update(pathUtf8(u.obj.filename()));
        h.updateFile(runtimeLib);
        h.update(resKey);
        linkKey = h.hex();
    }
    const fs::path keyFile = buildDir / "link.key";
    std::vector<uint8_t> oldKey;
    bool linkUpToDate = !opt_.forceRebuild && fs::is_regular_file(exe, ec) && readWholeFile(keyFile, oldKey) &&
                        std::string(oldKey.begin(), oldKey.end()) == linkKey;
    if (linkUpToDate) {
        log(LogLevel::Good, "Game executable is up to date (nothing to link).");
        std::lock_guard<std::mutex> lk(mtx_);
        nominal_[(int)Stage::Link] = 0.05;
    } else {
        fs::remove(keyFile, ec);
        std::wstring rsp = std::wstring(kLinkFlags) + L"\r\n/OUT:" + quoteArg(exe.wstring()) + L"\r\n";
        for (auto& u : all) rsp += quoteArg(u.obj.wstring()) + L"\r\n";
        if (!resFile.empty()) rsp += quoteArg(resFile.wstring()) + L"\r\n";
        rsp += quoteArg(runtimeLib.wstring()) + L"\r\n" + kLinkLibs + L"\r\n";
        fs::path rspFile = buildDir / "link.rsp";
        if (!writeUtf16File(rspFile, rsp)) return fail("Cannot write " + pathUtf8(rspFile));
        std::wstring cmd = quoteArg(tc->link.wstring()) + L" @" + quoteArg(rspFile.wstring());
        ProcessGroup group;
        ChildProcess p;
        std::string err;
        fs::path linkLog = logDir / "link.log";
        if (!spawnProcess(cmd, &tc->envBlock, buildDir, linkLog, group, p, &err)) return fail("Could not start link.exe: " + err);
        int code = waitProcess(p, cancel_, group);
        closeProcess(p);
        if (cancel_) throw Cancelled{};
        if (code != 0) {
            std::string tail = readTail(linkLog, 25);
            log(LogLevel::Error, tail);
            if (tail.find("LNK1104") != std::string::npos && tail.find(narrow(gameExeName())) != std::string::npos)
                return fail("Cannot write " + pathUtf8(exe) + ". Is the game still running? Close it and try again.");
            return fail("Linking failed (exit code " + std::to_string(code) + "). Full log: " + pathUtf8(linkLog));
        }
        writeWholeFile(keyFile, linkKey.data(), linkKey.size());
        log(LogLevel::Good, "Linked " + pathUtf8(exe.filename()) + " (" + formatBytes(fs::file_size(exe, ec)) + ")");
    }

    // ---------------------------------------------------------------- install runtime files
    setStage(Stage::Install, "Copying runtime files");
    int copied = 0, total = 0;
    for (auto& e : fs::directory_iterator(opt_.runtimeDir, ec)) {
        if (!e.is_regular_file() || _wcsicmp(e.path().extension().c_str(), L".dll") != 0) continue;
        total++;
        fs::path dst = out / e.path().filename();
        if (sameFile(e.path(), dst)) continue;
        std::error_code cec;
        fs::copy_file(e.path(), dst, fs::copy_options::overwrite_existing, cec);
        if (cec)
            return fail("Cannot copy " + pathUtf8(e.path().filename()) + " to the install folder (" + cec.message() +
                        "). Is the game still running?");
        copied++;
    }
    {
        std::string ini = "; Written by the launcher. Path to your own game disc image (UTF-8).\r\nIsoPath=" + pathUtf8(opt_.iso) +
                          "\r\n";
        std::vector<uint8_t> old;
        if (!readWholeFile(out / "game.ini", old) || std::string(old.begin(), old.end()) != ini)
            if (!writeWholeFile(out / "game.ini", ini.data(), ini.size())) return fail("Cannot write game.ini");
    }
    log(LogLevel::Info, "Runtime files: " + std::to_string(copied) + " of " + std::to_string(total) + " DLL(s) updated");
    {
        std::lock_guard<std::mutex> lk(mtx_);
        st_.upToDate = todo.empty() && linkUpToDate && copied == 0;
    }
    setFraction(1.0f);
    log(LogLevel::Good, "Ready to play: " + pathUtf8(exe) + "  (total " + formatDuration(now()) + ")");
    return true;
}

}  // namespace vc2l
