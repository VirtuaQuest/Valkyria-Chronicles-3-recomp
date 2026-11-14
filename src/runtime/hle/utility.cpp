// sceUtility: utility module loading, system parameters, the shared dialog status machine and the
// on-screen keyboard. Savedata lives in savedata.cpp, the ImGui front end in utility_ui.cpp.
#include "utility.h"

#include <windows.h>

#include <algorithm>
#include <map>

#include "../kernel/kernel.h"
#include "hle.h"

namespace vc2rt::utility {

// ------------------------------------------------------------------ dialog status machine
void DialogState::change(Status s, u64 delayUs) {
    if (delayUs == 0) {
        status = s;
        pending = s;
        pendingAt = 0;
    } else {
        pending = s;
        pendingAt = pspTimeMicros() + delayUs;
    }
}

Status DialogState::poll() {
    if (pendingAt && pspTimeMicros() >= pendingAt) {
        status = pending;
        pendingAt = 0;
    }
    return status;
}

s32 checkRequest(u32 addr, std::initializer_list<u32> sizes) {
    if (!memValid(addr) || !memValid(addr + sizeof(DialogCommon) - 1)) return (s32)ERR_UTILITY_INVALID_ADDRESS;
    u32 size = rd32(addr);
    if (std::find(sizes.begin(), sizes.end(), size) == sizes.end()) return (s32)ERR_UTILITY_INVALID_PARAM_SIZE;
    if (!memValid(addr + size - 1)) return (s32)ERR_UTILITY_INVALID_ADDRESS;
    return 0;
}

// ------------------------------------------------------------------ text helpers
std::string utf16ToUtf8(const std::u16string& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t*)s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, (const wchar_t*)s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::u16string utf8ToUtf16(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::u16string out(n, u'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), (wchar_t*)out.data(), n);
    return out;
}

std::u16string readGuestUtf16(u32 addr, size_t maxUnits) {
    std::u16string s;
    if (!addr || !memValid(addr)) return s;
    for (size_t i = 0; i < maxUnits && memValid(addr + (u32)i * 2 + 1); i++) {
        char16_t ch = (char16_t)rd16(addr + (u32)i * 2);
        if (!ch) break;
        s.push_back(ch);
    }
    return s;
}

namespace {

// One dialog of any type may be active at a time (savedata, OSK, msg, ...).
enum class DialogType { None, Savedata, Osk };
DialogType g_current = DialogType::None;

DialogState& stateOf(DialogType t);

bool anyDialogBusy() {
    if (g_current == DialogType::None) return false;
    return stateOf(g_current).busy();
}

// ------------------------------------------------------------------ module loading
struct UtilModule {
    u32 id;
    const char* name;
    u32 dep;  // module that must be loaded first (0 = none)
};
const UtilModule kModules[] = {
    {0x100, "net_common", 0},     {0x101, "net_adhoc", 0},      {0x102, "net_inet", 0},
    {0x103, "net_parse_uri", 0},  {0x104, "net_parse_http", 0}, {0x105, "net_http", 0x102},
    {0x106, "net_ssl", 0x102},    {0x107, "net_upnp", 0},       {0x108, "net_gameupdate", 0},
    {0x200, "usb_pspcm", 0},      {0x201, "usb_mic", 0},        {0x202, "usb_cam", 0},
    {0x203, "usb_gps", 0},        {0x2ff, "usb_unk", 0},        {0x300, "av_avcodec", 0},
    {0x301, "av_sascore", 0},     {0x302, "av_atrac3plus", 0x300}, {0x303, "av_mpegbase", 0x300},
    {0x304, "av_mp3", 0},         {0x305, "av_vaudio", 0},      {0x306, "av_aac", 0},
    {0x307, "av_g729", 0},        {0x308, "av_mp4", 0x300},     {0x3fe, "me_stuff", 0},
    {0x3ff, "me_core", 0},        {0x400, "np_common", 0},      {0x401, "np_service", 0},
    {0x402, "np_matching2", 0},   {0x403, "np_unk", 0},         {0x500, "np_drm", 0},
    {0x600, "irda", 0},           {0x601, "unk_601", 0},
};
std::map<u32, bool> g_loadedModules;
constexpr u32 ERR_LIBRARY_NOTFOUND = 0x8002013a;

const UtilModule* findModule(u32 id) {
    for (auto& m : kModules)
        if (m.id == id) return &m;
    return nullptr;
}

// The libraries themselves are HLE, so "loading" only tracks state for the error semantics games
// rely on (VC2 explicitly accepts 0x80111102 for an already loaded module).
void sceUtilityLoadModule(PspContext* c) {
    const UtilModule* m = findModule(A0);
    s32 r = 0;
    if (!m) r = (s32)ERR_MODULE_BAD_ID;
    else if (g_loadedModules.count(m->id)) r = (s32)ERR_MODULE_ALREADY_LOADED;
    else if (m->dep && !g_loadedModules.count(m->dep)) r = (s32)ERR_LIBRARY_NOTFOUND;
    else g_loadedModules[m->id] = true;
    LOG_DEBUG("UTIL", "sceUtilityLoadModule(0x%x %s) -> %08x", A0, m ? m->name : "?", (u32)r);
    RET(r);
}

void sceUtilityUnloadModule(PspContext* c) {
    const UtilModule* m = findModule(A0);
    s32 r = 0;
    if (!m) r = (s32)ERR_MODULE_BAD_ID;
    else if (!g_loadedModules.erase(m->id)) r = (s32)ERR_MODULE_NOT_LOADED;
    LOG_DEBUG("UTIL", "sceUtilityUnloadModule(0x%x) -> %08x", A0, (u32)r);
    RET(r);
}

// ------------------------------------------------------------------ system parameters
enum : u32 {
    PARAM_NICKNAME = 1,
    PARAM_ADHOC_CHANNEL = 2,
    PARAM_WLAN_POWERSAVE = 3,
    PARAM_DATE_FORMAT = 4,
    PARAM_TIME_FORMAT = 5,
    PARAM_TIMEZONE = 6,
    PARAM_DAYLIGHTSAVINGS = 7,
    PARAM_LANGUAGE = 8,
    PARAM_BUTTON_PREFERENCE = 9,
    PARAM_LOCK_PARENTAL_LEVEL = 10,
};
constexpr u32 LANGUAGE_ENGLISH = 1;
constexpr u32 BUTTON_CROSS = 1;  // 0 = Circle confirms (Japan), 1 = Cross confirms (US/EU)

u32 localeInt(LCTYPE type, u32 fallback) {
    wchar_t buf[16] = {};
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, type, buf, 16) <= 0) return fallback;
    return (u32)_wtoi(buf);
}

bool systemParamInt(u32 id, u32& v) {
    switch (id) {
        case PARAM_ADHOC_CHANNEL: v = 0; return true;   // automatic
        case PARAM_WLAN_POWERSAVE: v = 0; return true;  // off
        case PARAM_DATE_FORMAT: {
            // Windows LOCALE_IDATE: 0 = M/D/Y, 1 = D/M/Y, 2 = Y/M/D.  PSP: 0 = YMD, 1 = MDY, 2 = DMY.
            u32 w = localeInt(LOCALE_IDATE, 0);
            v = w == 0 ? 1 : w == 1 ? 2 : 0;
            return true;
        }
        case PARAM_TIME_FORMAT:
            // Windows LOCALE_ITIME: 0 = 12h, 1 = 24h.  PSP: 0 = 24h, 1 = 12h.
            v = localeInt(LOCALE_ITIME, 0) == 1 ? 0 : 1;
            return true;
        case PARAM_TIMEZONE: {
            TIME_ZONE_INFORMATION tz{};
            GetTimeZoneInformation(&tz);
            v = (u32)(s32)(-tz.Bias);  // minutes east of UTC, standard time
            return true;
        }
        case PARAM_DAYLIGHTSAVINGS: {
            TIME_ZONE_INFORMATION tz{};
            v = GetTimeZoneInformation(&tz) == TIME_ZONE_ID_DAYLIGHT ? 1 : 0;
            return true;
        }
        case PARAM_LANGUAGE: v = LANGUAGE_ENGLISH; return true;
        case PARAM_BUTTON_PREFERENCE: v = BUTTON_CROSS; return true;
        case PARAM_LOCK_PARENTAL_LEVEL: v = 0; return true;
        default: return false;
    }
}

void sceUtilityGetSystemParamInt(PspContext* c) {
    runUtilitySelfTest(c);  // no-op unless VC2_UTIL_SELFTEST is set
    u32 v = 0;
    if (!systemParamInt(A0, v)) {
        RET(ERR_UTILITY_INVALID_SYSTEM_PARAM_ID);
        return;
    }
    if (memValid(A1)) wr32(A1, v);
    LOG_DEBUG("UTIL", "sceUtilityGetSystemParamInt(%u) = %d", A0, (s32)v);
    RET(0);
}

void sceUtilitySetSystemParamInt(PspContext* c) {
    if (A0 == PARAM_ADHOC_CHANNEL) {
        if (A1 != 0 && A1 != 1 && A1 != 6 && A1 != 11) {
            RET(ERR_UTILITY_INVALID_ADHOC_CHANNEL);
            return;
        }
        RET(0);
        return;
    }
    RET(A0 == PARAM_WLAN_POWERSAVE ? 0 : ERR_UTILITY_INVALID_SYSTEM_PARAM_ID);
}

void sceUtilityGetSystemParamString(PspContext* c) {
    if (A0 != PARAM_NICKNAME) {
        RET(ERR_UTILITY_INVALID_SYSTEM_PARAM_ID);
        return;
    }
    // The PSP nickname: use the Windows user name (ASCII/UTF-8), falling back to "PLAYER".
    std::string nick = "PLAYER";
    wchar_t wbuf[64];
    DWORD n = 64;
    if (GetUserNameW(wbuf, &n) && n > 1) nick = utf16ToUtf8(std::u16string((char16_t*)wbuf, n - 1));
    if (nick.size() > 127) nick.resize(127);
    s32 size = (s32)A2;
    if (size <= (s32)nick.size()) {
        RET(ERR_UTILITY_STRING_TOO_LONG);
        return;
    }
    writeCString(A1, nick, (size_t)size);
    RET(0);
}

// ------------------------------------------------------------------ savedata entry points
void sceUtilitySavedataInitStart(PspContext* c) {
    if (anyDialogBusy()) {
        RET(ERR_UTILITY_INVALID_STATUS);
        return;
    }
    s32 r = savedata::initStart(A0);
    if (r >= 0 || savedata::state().busy()) g_current = DialogType::Savedata;
    RET(r);
}
void sceUtilitySavedataGetStatus(PspContext* c) {
    if (g_current != DialogType::Savedata) {
        RET(ERR_UTILITY_WRONG_TYPE);
        return;
    }
    RET(savedata::state().poll());
}
void sceUtilitySavedataUpdate(PspContext* c) {
    if (g_current != DialogType::Savedata) {
        RET(ERR_UTILITY_WRONG_TYPE);
        return;
    }
    RET(savedata::update((s32)A0));
}
void sceUtilitySavedataShutdownStart(PspContext* c) {
    if (g_current != DialogType::Savedata) {
        RET(ERR_UTILITY_WRONG_TYPE);
        return;
    }
    RET(savedata::shutdownStart());
}

// ------------------------------------------------------------------ on-screen keyboard
// SceUtilityOskParams: common(0x30) fieldCount@30 fields@34 state@38 unk@3c
// SceUtilityOskData:   language@08 inputtype@10 lines@14 desc@1c intext@20 outtextlength@24
//                      outtext@28 result@2c outtextlimit@30
enum : u32 { OSK_RESULT_UNCHANGED = 0, OSK_RESULT_CANCELLED = 1, OSK_RESULT_CHANGED = 2 };
enum : u32 { OSKSTATE_NONE = 0, OSKSTATE_INITING = 1, OSKSTATE_INITED = 2, OSKSTATE_VISIBLE = 3, OSKSTATE_QUIT = 4, OSKSTATE_FINISHED = 5 };
constexpr u64 kOskInitDelay = 300000, kOskShutdownDelay = 40000;

struct Osk {
    DialogState st;
    u32 params = 0, field = 0;
    std::u16string desc, intext;
    u32 outLength = 0, outLimit = 0, outPtr = 0;
    bool uiShown = false;
    u32 maxChars() const {
        if (outLength == 0) return 0;
        if (outLimit == 0 || outLimit > outLength - 1) return outLength - 1;
        return outLimit;
    }
} g_osk;

void oskWriteOutput(const std::u16string& text, u32 fieldResult) {
    // Only the text and its terminator are written, not the whole buffer.
    u32 n = std::min<u32>((u32)text.size(), g_osk.maxChars());
    if (g_osk.outLength && memValid(g_osk.outPtr)) {
        for (u32 i = 0; i < n; i++) wr16(g_osk.outPtr + i * 2, (u16)text[i]);
        if (n < g_osk.outLength) wr16(g_osk.outPtr + n * 2, 0);
    }
    wr32(g_osk.field + 0x2c, fieldResult);
    wr32(g_osk.params + 0x1c, 0);  // common.result
}

void sceUtilityOskInitStart(PspContext* c) {
    if (anyDialogBusy() || g_osk.st.busy()) {
        RET(ERR_UTILITY_INVALID_STATUS);
        return;
    }
    s32 chk = checkRequest(A0, {0x40, 0x44});
    if (chk < 0) {
        RET(chk);
        return;
    }
    u32 fields = rd32(A0 + 0x34);
    if (!memValid(fields) || !memValid(fields + 0x33)) {
        RET(-1);
        return;
    }
    if ((s32)rd32(A0 + 0x30) != 1) LOG_WARN("UTIL", "OSK with %d fields: only the first is edited", (s32)rd32(A0 + 0x30));
    g_osk.params = A0;
    g_osk.field = fields;
    g_osk.desc = readGuestUtf16(rd32(fields + 0x1c));
    g_osk.intext = readGuestUtf16(rd32(fields + 0x20));
    g_osk.outLength = rd32(fields + 0x24);
    g_osk.outPtr = rd32(fields + 0x28);
    g_osk.outLimit = rd32(fields + 0x30);
    g_osk.uiShown = false;
    g_osk.st.change(ST_INIT, 0);
    g_osk.st.change(ST_RUNNING, kOskInitDelay);
    wr32(A0 + 0x38, OSKSTATE_INITING);
    g_current = DialogType::Osk;
    uiEnsureRegistered();
    LOG_INFO("UTIL", "OSK start: \"%s\" initial \"%s\" max %u chars, inputtype %08x, lines %d",
             utf16ToUtf8(g_osk.desc).c_str(), utf16ToUtf8(g_osk.intext).c_str(), g_osk.maxChars(),
             rd32(fields + 0x10), (s32)rd32(fields + 0x14));
    RET(0);
}

void sceUtilityOskGetStatus(PspContext* c) {
    if (g_current != DialogType::Osk) {
        RET(ERR_UTILITY_WRONG_TYPE);
        return;
    }
    RET(g_osk.st.poll());
}

void sceUtilityOskUpdate(PspContext* c) {
    if (g_current != DialogType::Osk) {
        RET(ERR_UTILITY_WRONG_TYPE);
        return;
    }
    if (g_osk.st.poll() != ST_RUNNING) {
        RET(ERR_UTILITY_INVALID_STATUS);
        return;
    }
    UiState& u = ui();
    if (!g_osk.uiShown) {
        u.screen = UiScreen::Osk;
        u.heading = utf16ToUtf8(g_osk.desc);
        u.oskText = utf16ToUtf8(g_osk.intext.substr(0, g_osk.maxChars()));
        u.oskMaxChars = g_osk.maxChars();
        u.oskInputType = rd32(g_osk.field + 0x10);
        u.oskLines = std::max(1, (int)rd32(g_osk.field + 0x14));
        u.event = UiEvent::None;
        g_osk.uiShown = true;
        wr32(g_osk.params + 0x38, OSKSTATE_VISIBLE);
    } else if (u.event == UiEvent::Ok || u.event == UiEvent::Cancel) {
        bool ok = u.event == UiEvent::Ok;
        std::u16string text = ok ? utf8ToUtf16(u.oskText) : g_osk.intext;
        oskWriteOutput(text, ok ? OSK_RESULT_CHANGED : OSK_RESULT_CANCELLED);
        LOG_INFO("UTIL", "OSK %s: \"%s\"", ok ? "confirmed" : "cancelled", utf16ToUtf8(text).c_str());
        u.screen = UiScreen::Hidden;
        u.event = UiEvent::None;
        wr32(g_osk.params + 0x38, ok ? OSKSTATE_FINISHED : OSKSTATE_QUIT);
        g_osk.st.change(ST_FINISHED, 0);
    }
    RET(0);
}

void sceUtilityOskShutdownStart(PspContext* c) {
    if (g_current != DialogType::Osk) {
        RET(ERR_UTILITY_WRONG_TYPE);
        return;
    }
    if (g_osk.st.poll() != ST_FINISHED) {
        RET(ERR_UTILITY_INVALID_STATUS);
        return;
    }
    g_osk.st.change(ST_SHUTDOWN, 0);
    g_osk.st.change(ST_NONE, kOskShutdownDelay);
    wr32(g_osk.params + 0x38, OSKSTATE_NONE);
    RET(0);
}

DialogState& stateOf(DialogType t) {
    static DialogState none;
    switch (t) {
        case DialogType::Savedata: return savedata::state();
        case DialogType::Osk: return g_osk.st;
        default: return none;
    }
}

const HleFunction kUtility[] = {
    {0x2A2B3DE0, "sceUtilityLoadModule", sceUtilityLoadModule},
    {0xE49BFE92, "sceUtilityUnloadModule", sceUtilityUnloadModule},
    {0xA5DA2406, "sceUtilityGetSystemParamInt", sceUtilityGetSystemParamInt},
    {0x45C18506, "sceUtilitySetSystemParamInt", sceUtilitySetSystemParamInt},
    {0x34B78343, "sceUtilityGetSystemParamString", sceUtilityGetSystemParamString},
    {0x50C4CD57, "sceUtilitySavedataInitStart", sceUtilitySavedataInitStart},
    {0x8874DBE0, "sceUtilitySavedataGetStatus", sceUtilitySavedataGetStatus},
    {0x9790B33C, "sceUtilitySavedataShutdownStart", sceUtilitySavedataShutdownStart},
    {0xD4B95FFB, "sceUtilitySavedataUpdate", sceUtilitySavedataUpdate},
    {0xF6269B82, "sceUtilityOskInitStart", sceUtilityOskInitStart},
    {0xF3F76017, "sceUtilityOskGetStatus", sceUtilityOskGetStatus},
    {0x4B85C861, "sceUtilityOskUpdate", sceUtilityOskUpdate},
    {0x3DFAEBA9, "sceUtilityOskShutdownStart", sceUtilityOskShutdownStart},
};

}  // namespace

HLE_MODULE("sceUtility", kUtility);

}  // namespace vc2rt::utility
