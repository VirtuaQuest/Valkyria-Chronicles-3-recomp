#include "util.h"

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <fstream>
#include <thread>

namespace vc2l {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string pathUtf8(const fs::path& p) { return narrow(p.wstring()); }
fs::path pathFromUtf8(const std::string& s) { return fs::path(widen(s)); }

fs::path launcherExePath() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    buf.resize(n);
    return fs::path(buf);
}

fs::path launcherDir() { return launcherExePath().parent_path(); }

fs::path localAppDataDir() {
    PWSTR p = nullptr;
    fs::path r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p) r = p;
    CoTaskMemFree(p);
    if (r.empty()) {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) r = buf;
    }
    return r;
}

fs::path defaultInstallDir() {
    fs::path base = localAppDataDir();
    if (base.empty()) return launcherDir() / "VC3Recompiled";
    return base / "VC3Recompiled";
}

static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

std::map<std::string, std::string> readKeyValueFile(const fs::path& file) {
    std::map<std::string, std::string> kv;
    std::ifstream f(file, std::ios::binary);
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        if (first && line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF)
            line = line.substr(3);  // UTF-8 BOM
        first = false;
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return kv;
}

bool writeKeyValueFile(const fs::path& file, const std::map<std::string, std::string>& kv, const char* header) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (header) f << "; " << header << "\r\n";
    for (auto& [k, v] : kv) f << k << "=" << v << "\r\n";
    return (bool)f;
}

bool readWholeFile(const fs::path& file, std::vector<uint8_t>& out) {
    std::ifstream f(file, std::ios::binary | std::ios::ate);
    if (!f) return false;
    auto sz = (std::streamoff)f.tellg();
    out.resize((size_t)sz);
    f.seekg(0);
    return sz == 0 || (bool)f.read((char*)out.data(), sz);
}

bool writeWholeFile(const fs::path& file, const void* data, size_t size) {
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write((const char*)data, (std::streamsize)size);
    return (bool)f;
}

// ------------------------------------------------------------------ SHA-256
Sha256::Sha256() {
    BCRYPT_ALG_HANDLE a = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(a, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        alg_ = a;
        hash_ = h;
    } else if (a) {
        BCryptCloseAlgorithmProvider(a, 0);
    }
}

Sha256::~Sha256() {
    if (hash_) BCryptDestroyHash((BCRYPT_HASH_HANDLE)hash_);
    if (alg_) BCryptCloseAlgorithmProvider((BCRYPT_ALG_HANDLE)alg_, 0);
}

void Sha256::update(const void* data, size_t len) {
    if (!hash_) return;
    const uint8_t* p = (const uint8_t*)data;
    while (len) {
        ULONG n = (ULONG)std::min<size_t>(len, 1u << 30);
        BCryptHashData((BCRYPT_HASH_HANDLE)hash_, (PUCHAR)p, n, 0);
        p += n;
        len -= n;
    }
}

bool Sha256::updateFile(const fs::path& file) {
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) return false;
    std::vector<uint8_t> buf(1 << 20);
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) update(buf.data(), n);
    fclose(f);
    return true;
}

std::string Sha256::hex() {
    uint8_t d[32] = {};
    if (hash_) BCryptFinishHash((BCRYPT_HASH_HANDLE)hash_, d, 32, 0);
    char s[65];
    for (int i = 0; i < 32; i++) snprintf(s + i * 2, 3, "%02x", d[i]);
    return s;
}

// ------------------------------------------------------------------ misc
std::string formatDuration(double seconds) {
    if (seconds < 0) seconds = 0;
    long long t = (long long)(seconds + 0.5);
    char buf[32];
    if (t >= 3600)
        snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
    else
        snprintf(buf, sizeof buf, "%lld:%02lld", t / 60, t % 60);
    return buf;
}

std::string formatBytes(uint64_t b) {
    char buf[32];
    if (b >= (1ull << 30))
        snprintf(buf, sizeof buf, "%.2f GB", b / double(1ull << 30));
    else if (b >= (1ull << 20))
        snprintf(buf, sizeof buf, "%.1f MB", b / double(1ull << 20));
    else
        snprintf(buf, sizeof buf, "%.0f KB", b / 1024.0);
    return buf;
}

void openUrl(const std::string& url) { ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

void openFolder(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    ShellExecuteW(nullptr, L"explore", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::string lastErrorString(unsigned long code) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
                   0, (LPWSTR)&msg, 0, nullptr);
    std::string s = msg ? narrow(msg) : "error " + std::to_string(code);
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.')) s.pop_back();
    return s;
}

bool launchDetached(const fs::path& exe, const fs::path& cwd, std::string* err) {
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(), &si, &pi)) {
        if (err) *err = lastErrorString(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

unsigned hardwareThreads() {
    unsigned n = std::thread::hardware_concurrency();
    return n ? n : 4;
}

}  // namespace vc2l
