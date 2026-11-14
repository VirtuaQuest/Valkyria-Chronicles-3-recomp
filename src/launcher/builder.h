#pragma once
// The recompile pipeline used by both the GUI and the CLI:
//   toolchain -> validate disc -> load/analyze -> translate to C -> parallel cl.exe -> rc.exe -> link.exe
//   -> copy runtime files. Runs on its own worker thread; the UI polls status() and the log.
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vc2l {

namespace fs = std::filesystem;

enum class Stage : int { Idle = 0, Toolchain, Validate, Decrypt, Translate, Compile, Resource, Link, Install, Done, Count };
const char* stageLabel(Stage s);

enum class LogLevel : int { Info = 0, Good, Warn, Error, Detail };

struct LogLine {
    LogLevel level;
    double t;  // seconds since build start
    std::string text;
};

struct BuildStatus {
    bool running = false, finished = false, ok = false, cancelled = false, upToDate = false;
    Stage stage = Stage::Idle;
    std::string detail;            // current activity text
    float progress = 0;            // 0..1 overall (monotonic)
    double elapsed = 0, eta = -1;  // seconds; eta < 0 = unknown
    int filesTotal = 0, filesToCompile = 0, filesCompiled = 0, filesCached = 0, jobs = 0, activeJobs = 0;
    double stageTime[(int)Stage::Count] = {};  // seconds spent per finished stage
    std::string error;
    fs::path exePath;
};

struct BuildOptions {
    fs::path iso;
    fs::path outDir;
    fs::path runtimeDir;  // folder with vc3runtime.dll (+ other DLLs) and sdk/ ; defaults to the launcher folder
    int jobs = 0;         // 0 = number of hardware threads
    bool forceRebuild = false;
};

// Executable name for the detected game (VC3Recompiled.exe); updated whenever a
// disc is probed.
const wchar_t* gameExeName();
void setGameExeName(const std::string& utf8Name);
fs::path gameExePath(const fs::path& outDir);

class Builder {
public:
    ~Builder();
    bool start(const BuildOptions& opt);  // false if a build is already running
    void cancel();
    void join();
    BuildStatus status() const;
    // Copies log lines [from, end) into `out`; returns the new end index.
    size_t fetchLog(size_t from, std::vector<LogLine>& out) const;
    size_t logSize() const;

private:
    void run();
    bool runStages();
    void log(LogLevel lv, std::string text);
    void setStage(Stage s, const std::string& detail);
    void setFraction(float f, const std::string* detail = nullptr);
    void recompute();  // progress + eta (lock held)
    double now() const;

    BuildOptions opt_;
    std::thread thread_;
    std::atomic<bool> cancel_{false};
    mutable std::mutex mtx_;
    BuildStatus st_;
    std::vector<LogLine> log_;
    std::chrono::steady_clock::time_point t0_;
    // progress model
    double nominal_[(int)Stage::Count] = {};  // expected seconds per stage
    float frac_[(int)Stage::Count] = {};
    double stageStart_ = 0;
    float shown_ = 0;
};

}  // namespace vc2l
