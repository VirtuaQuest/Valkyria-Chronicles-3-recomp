#include "toolchain.h"

#include <windows.h>

#include <future>

#include "util.h"

namespace vc2l {

namespace {

struct ICaseLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
};
using EnvMap = std::map<std::wstring, std::wstring, ICaseLess>;

// Runs a command and returns its stdout (raw bytes). Used for vswhere and vcvars.
bool runCapture(const std::wstring& cmdline, std::string& out, DWORD timeoutMs, std::string* err) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        if (err) *err = lastErrorString(GetLastError());
        return false;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0,
                             nullptr);

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    auto* attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
    InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
    HANDLE inherit[2] = {wr, nul};
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = nul;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdline;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                             nullptr, nullptr, &si.StartupInfo, &pi);
    DWORD le = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        if (err) *err = lastErrorString(le);
        return false;
    }
    char buf[4096];
    DWORD n;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, timeoutMs);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

fs::path findInPath(const EnvMap& env, const wchar_t* exe) {
    auto it = env.find(L"PATH");
    if (it == env.end()) return {};
    const std::wstring& p = it->second;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find(L';', i);
        if (j == std::wstring::npos) j = p.size();
        std::wstring dir = p.substr(i, j - i);
        if (!dir.empty()) {
            if (dir.front() == L'"' && dir.size() >= 2 && dir.back() == L'"') dir = dir.substr(1, dir.size() - 2);
            std::error_code ec;
            fs::path cand = fs::path(dir) / exe;
            if (fs::is_regular_file(cand, ec)) return cand;
        }
        i = j + 1;
    }
    return {};
}

std::string envValue(const EnvMap& env, const wchar_t* k) {
    auto it = env.find(k);
    if (it == env.end()) return {};
    std::string s = narrow(it->second);
    while (!s.empty() && (s.back() == '\\' || s.back() == ' ')) s.pop_back();
    return s;
}

const char* kNoMsvcMessage =
    "The Microsoft C++ Build Tools (MSVC) were not found on this PC.\n\n"
    "The recompiler turns the game's code into C, which then has to be compiled by Microsoft's free C++ compiler. "
    "Install \"Build Tools for Visual Studio\" with the workload \"Desktop development with C++\" (free, about 2 GB), "
    "then click \"Check again\".";

std::shared_ptr<const Toolchain> doDetect() {
    auto tc = std::make_shared<Toolchain>();
    std::string err;
    if (GetEnvironmentVariableW(L"VC2_SIMULATE_NO_MSVC", nullptr, 0) > 0) {  // debug aid for the "not installed" UI
        tc->error = kNoMsvcMessage;
        return tc;
    }

    // 1) vswhere
    wchar_t pf86[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"ProgramFiles(x86)", pf86, MAX_PATH);
    fs::path vswhere = (n ? fs::path(pf86) : fs::path(L"C:\\Program Files (x86)")) / L"Microsoft Visual Studio" /
                       L"Installer" / L"vswhere.exe";
    std::error_code ec;
    fs::path install;
    if (fs::is_regular_file(vswhere, ec)) {
        std::string out;
        std::wstring cmd = quoteArg(vswhere.wstring()) +
                           L" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
                           L" -property installationPath -utf8 -nologo";
        if (runCapture(cmd, out, 30000, &err)) {
            std::string line = out.substr(0, out.find_first_of("\r\n"));
            if (!line.empty()) install = pathFromUtf8(line);
        }
    }

    EnvMap env;
    fs::path vcvars;
    if (!install.empty()) vcvars = install / L"VC" / L"Auxiliary" / L"Build" / L"vcvars64.bat";
    if (!vcvars.empty() && fs::is_regular_file(vcvars, ec)) {
        // 2) capture the environment produced by vcvars64.bat. /u makes `set` write UTF-16.
        wchar_t comspec[MAX_PATH] = L"cmd.exe";
        GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
        std::wstring cmd = quoteArg(comspec) + L" /d /u /s /c \"" + quoteArg(vcvars.wstring()) + L" >nul 2>&1 && set\"";
        std::string out;
        if (!runCapture(cmd, out, 120000, &err)) {
            tc->error = "Could not run vcvars64.bat: " + err;
            return tc;
        }
        std::wstring w((const wchar_t*)out.data(), out.size() / 2);
        size_t i = 0;
        while (i < w.size()) {
            size_t j = w.find(L'\n', i);
            if (j == std::wstring::npos) j = w.size();
            std::wstring line = w.substr(i, j - i);
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            size_t eq = line.find(L'=');
            if (eq != std::wstring::npos && eq > 0) env[line.substr(0, eq)] = line.substr(eq + 1);
            i = j + 1;
        }
        tc->vsInstall = install;
    } else {
        // 3) fallback: already running inside a developer command prompt
        LPWCH block = GetEnvironmentStringsW();
        for (LPWCH p = block; p && *p; p += wcslen(p) + 1) {
            std::wstring line = p;
            size_t eq = line.find(L'=', 1);
            if (eq != std::wstring::npos) env[line.substr(0, eq)] = line.substr(eq + 1);
        }
        if (block) FreeEnvironmentStringsW(block);
        if (env.find(L"VCToolsInstallDir") == env.end() || findInPath(env, L"cl.exe").empty()) {
            tc->error = kNoMsvcMessage;
            return tc;
        }
        tc->vsInstall = pathFromUtf8(envValue(env, L"VSINSTALLDIR"));
    }

    tc->cl = findInPath(env, L"cl.exe");
    if (tc->cl.empty()) {
        tc->error = "Visual Studio was found but cl.exe (the C++ x64 compiler) is missing. Open the Visual Studio "
                    "Installer, modify your installation and add \"MSVC v143 - VS 2022 C++ x64/x86 build tools\".";
        return tc;
    }
    tc->link = tc->cl.parent_path() / L"link.exe";  // never pick up another link.exe (e.g. Git's coreutils)
    if (!fs::is_regular_file(tc->link, ec)) {
        tc->error = "link.exe was not found next to cl.exe (" + pathUtf8(tc->cl.parent_path()) + ").";
        return tc;
    }
    tc->rc = findInPath(env, L"rc.exe");
    if (envValue(env, L"INCLUDE").empty() || envValue(env, L"LIB").empty()) {
        tc->error = "The MSVC environment is incomplete (INCLUDE/LIB not set). Is the Windows SDK installed?";
        return tc;
    }
    tc->vsVersion = envValue(env, L"VSCMD_VER");
    tc->msvcVersion = envValue(env, L"VCToolsVersion");
    tc->sdkVersion = envValue(env, L"WindowsSDKVersion");

    // environment block for CreateProcessW (sorted, double-NUL terminated)
    for (auto& [k, v] : env) {
        std::wstring kv = k + L"=" + v;
        tc->envBlock.insert(tc->envBlock.end(), kv.begin(), kv.end());
        tc->envBlock.push_back(L'\0');
    }
    tc->envBlock.push_back(L'\0');
    tc->ok = true;
    return tc;
}

std::mutex g_tcMutex;
std::shared_future<std::shared_ptr<const Toolchain>> g_tcFuture;

std::shared_future<std::shared_ptr<const Toolchain>> toolchainFuture() {
    std::lock_guard<std::mutex> lk(g_tcMutex);
    if (!g_tcFuture.valid()) g_tcFuture = std::async(std::launch::async, doDetect).share();
    return g_tcFuture;
}

}  // namespace

std::shared_ptr<const Toolchain> detectToolchain() { return toolchainFuture().get(); }

std::shared_ptr<const Toolchain> toolchainIfReady() {
    auto f = toolchainFuture();
    if (f.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
    return f.get();
}

void resetToolchainDetection() {
    std::lock_guard<std::mutex> lk(g_tcMutex);
    if (g_tcFuture.valid() && g_tcFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) g_tcFuture = {};
}

// ------------------------------------------------------------------ processes
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            bs++;
            continue;
        }
        if (c == L'"') r.append(bs * 2 + 1, L'\\');
        else r.append(bs, L'\\');
        bs = 0;
        r.push_back(c);
    }
    r.append(bs * 2, L'\\');
    r.push_back(L'"');
    return r;
}

ProcessGroup::ProcessGroup() {
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
}

ProcessGroup::~ProcessGroup() {
    if (job_) CloseHandle(job_);  // kills anything still running
}

void ProcessGroup::terminateAll() {
    if (job_) TerminateJobObject(job_, 1);
}

bool spawnProcess(const std::wstring& cmdline, const std::vector<wchar_t>* env, const fs::path& cwd,
                  const fs::path& logFile, ProcessGroup& group, ChildProcess& out, std::string* err) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE log = CreateFileW(logFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        if (err) *err = "cannot create " + pathUtf8(logFile) + ": " + lastErrorString(GetLastError());
        return false;
    }
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    auto* attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
    InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
    HANDLE inherit[2] = {log, nul};
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                              (nul != INVALID_HANDLE_VALUE ? 2 : 1) * sizeof(HANDLE), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdInput = nul != INVALID_HANDLE_VALUE ? nul : nullptr;
    si.StartupInfo.hStdOutput = log;
    si.StartupInfo.hStdError = log;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdline;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT |
                                 BELOW_NORMAL_PRIORITY_CLASS,
                             env && !env->empty() ? (LPVOID)env->data() : nullptr, cwd.empty() ? nullptr : cwd.c_str(),
                             &si.StartupInfo, &pi);
    DWORD le = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(log);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        if (err) *err = lastErrorString(le);
        return false;
    }
    if (group.job()) AssignProcessToJobObject(group.job(), pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    out.process = pi.hProcess;
    out.pid = pi.dwProcessId;
    return true;
}

int waitProcess(ChildProcess& p, const std::atomic<bool>& cancel, ProcessGroup& group) {
    if (!p.process) return -1;
    for (;;) {
        DWORD r = WaitForSingleObject(p.process, 50);
        if (r == WAIT_OBJECT_0) break;
        if (r == WAIT_FAILED) return -1;
        if (cancel) {
            group.terminateAll();
            WaitForSingleObject(p.process, 5000);
            return -1;
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(p.process, &code);
    return (int)code;
}

void closeProcess(ChildProcess& p) {
    if (p.process) CloseHandle(p.process);
    p.process = nullptr;
}

}  // namespace vc2l
