#pragma once
// MSVC discovery (vswhere + vcvars64.bat environment capture) and child-process management.
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vc2l {

namespace fs = std::filesystem;

struct Toolchain {
    bool ok = false;
    std::string error;           // user-facing explanation when !ok
    fs::path vsInstall;          // Visual Studio / Build Tools installation folder
    std::string vsVersion;       // e.g. "17.11.5" (VSCMD_VER) when known
    std::string msvcVersion;     // e.g. "14.41.34120" (VCToolsVersion)
    std::string sdkVersion;      // e.g. "10.0.22621.0"
    fs::path cl, link, rc;       // rc may be empty (no icon then)
    std::vector<wchar_t> envBlock;  // sorted, double-NUL terminated environment for children
};

// Detects MSVC once per process (thread-safe, cached). Blocks for ~1-3 s on the first call.
std::shared_ptr<const Toolchain> detectToolchain();
// Non-blocking: returns nullptr while detection has not finished yet (starts it in the background).
std::shared_ptr<const Toolchain> toolchainIfReady();
// Forgets a finished detection result so the next call re-detects (after the user installed MSVC).
void resetToolchainDetection();

// A Win32 job object: every child is assigned to it so Cancel can terminate them all at once.
class ProcessGroup {
public:
    ProcessGroup();
    ~ProcessGroup();
    void* job() const { return job_; }
    void terminateAll();

private:
    void* job_ = nullptr;
};

struct ChildProcess {
    void* process = nullptr;  // HANDLE
    unsigned long pid = 0;
};

// Starts `cmdline` with stdout+stderr redirected to `logFile` (created/truncated), hidden window.
bool spawnProcess(const std::wstring& cmdline, const std::vector<wchar_t>* env, const fs::path& cwd,
                  const fs::path& logFile, ProcessGroup& group, ChildProcess& out, std::string* err);
// Waits for completion while polling `cancel`; returns exit code, or -1 on cancel/failure.
int waitProcess(ChildProcess& p, const std::atomic<bool>& cancel, ProcessGroup& group);
void closeProcess(ChildProcess& p);

std::wstring quoteArg(const std::wstring& a);

}  // namespace vc2l
