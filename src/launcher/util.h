#pragma once
// Small Windows helpers shared by the launcher (paths, strings, ini, hashing, shell).
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace vc2l {

namespace fs = std::filesystem;

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& w);
std::string pathUtf8(const fs::path& p);
fs::path pathFromUtf8(const std::string& s);

fs::path launcherExePath();
fs::path launcherDir();
fs::path localAppDataDir();          // %LOCALAPPDATA%
fs::path defaultInstallDir();        // %LOCALAPPDATA%\VC3Recompiled

// Tiny "key=value" file (UTF-8). Section headers and comments are ignored.
std::map<std::string, std::string> readKeyValueFile(const fs::path& file);
bool writeKeyValueFile(const fs::path& file, const std::map<std::string, std::string>& kv, const char* header);

bool readWholeFile(const fs::path& file, std::vector<uint8_t>& out);
bool writeWholeFile(const fs::path& file, const void* data, size_t size);

// SHA-256 (BCrypt), incremental.
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(const void* data, size_t len);
    void update(const std::string& s) { update(s.data(), s.size()); }
    bool updateFile(const fs::path& file);
    std::string hex();  // finalizes

private:
    void* alg_ = nullptr;
    void* hash_ = nullptr;
};

std::string formatDuration(double seconds);  // "1:05" or "1:02:03"
std::string formatBytes(uint64_t bytes);

void openUrl(const std::string& url);
void openFolder(const fs::path& dir);
// Starts a detached process with the given working directory. Returns false with an error message on failure.
bool launchDetached(const fs::path& exe, const fs::path& cwd, std::string* err);

std::string lastErrorString(unsigned long code);
unsigned hardwareThreads();

}  // namespace vc2l
