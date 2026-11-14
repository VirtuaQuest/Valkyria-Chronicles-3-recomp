// Small system libraries: utils, power, rtc, umd, stdio, impose, wlan, npdrm, psid, net stubs.
#include <windows.h>

#include <ctime>

#include "../hle/hle.h"
#include "kernel.h"

namespace vc2rt {

u64 pspTimeMicros() { return hostMicros() + 1000000ull; }

namespace {
// ---------------------------------------------------------------- UtilsForUser
void sceKernelLibcTime(PspContext* c) {
    u32 t = (u32)time(nullptr);
    if (A0) wr32(A0, t);
    RET(t);
}
void sceKernelLibcGettimeofday(PspContext* c) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    u64 t = (((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10 - 11644473600000000ull;
    if (A0) {
        wr32(A0, (u32)(t / 1000000));
        wr32(A0 + 4, (u32)(t % 1000000));
    }
    if (A1) {
        wr32(A1, 0);
        wr32(A1 + 4, 0);
    }
    RET(0);
}
void sceKernelLibcClock(PspContext* c) { RET((u32)pspTimeMicros()); }
void retZero(PspContext* c) { RET(0); }

const HleFunction kUtils[] = {
    {0x27CC57F0, "sceKernelLibcTime", sceKernelLibcTime},
    {0x71EC4271, "sceKernelLibcGettimeofday", sceKernelLibcGettimeofday},
    {0x91E4F6A7, "sceKernelLibcClock", sceKernelLibcClock},
    {0x6AD345D7, "sceKernelSetGPO", retZero},
    {0x37FB5C42, "sceKernelGetGPI", retZero},
    {0x34B9FA9E, "sceKernelDcacheWritebackInvalidateRange", retZero},
    {0x3EE30821, "sceKernelDcacheWritebackRange", retZero},
    {0x79D1C3FA, "sceKernelDcacheWritebackAll", retZero},
    {0xB435DEC5, "sceKernelDcacheWritebackInvalidateAll", retZero},
};

// ---------------------------------------------------------------- sceSuspendForUser
bool g_volatileLocked = false;
void sceKernelVolatileMemLock(PspContext* c) {
    // The 4MB "volatile" region the PSP lends to games.
    if (g_volatileLocked) {
        RET(0x802B0200);
        return;
    }
    g_volatileLocked = true;
    if (A1) wr32(A1, 0x08400000);
    if (A2) wr32(A2, 0x00400000);
    RET(0);
}
void sceKernelVolatileMemTryLock(PspContext* c) { sceKernelVolatileMemLock(c); }
void sceKernelVolatileMemUnlock(PspContext* c) {
    g_volatileLocked = false;
    RET(0);
}
const HleFunction kSuspend[] = {
    {0x090CCB3F, "sceKernelPowerTick", retZero},
    {0x3E0271D3, "sceKernelVolatileMemLock", sceKernelVolatileMemLock},
    {0xA14F40B2, "sceKernelVolatileMemTryLock", sceKernelVolatileMemTryLock},
    {0xA569E425, "sceKernelVolatileMemUnlock", sceKernelVolatileMemUnlock},
    {0xEADB1BD7, "sceKernelPowerLock", retZero},
    {0x3AEE7261, "sceKernelPowerUnlock", retZero},
};

// ---------------------------------------------------------------- scePower
void scePowerGetBatteryLifePercent(PspContext* c) { RET(100); }
const HleFunction kPower[] = {
    {0x04B7766E, "scePowerRegisterCallback", retZero},
    {0xDFA8BAF8, "scePowerUnregisterCallback", retZero},
    {0xEBD177D6, "scePowerSetClockFrequency350", retZero},
    {0x737486F2, "scePowerSetClockFrequency", retZero},
    {0x2085D15D, "scePowerGetBatteryLifePercent", scePowerGetBatteryLifePercent},
    {0x87440F5E, "scePowerIsPowerOnline", [](PspContext* c) { RET(1); }},
};

// ---------------------------------------------------------------- sceRtc
void sceRtcGetCurrentClockLocalTime(PspContext* c) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    u32 p = A0;
    wr16(p, st.wYear);
    wr16(p + 2, st.wMonth);
    wr16(p + 4, st.wDay);
    wr16(p + 6, st.wHour);
    wr16(p + 8, st.wMinute);
    wr16(p + 10, st.wSecond);
    wr32(p + 12, st.wMilliseconds * 1000u);
    RET(0);
}
const HleFunction kRtc[] = {
    {0xE7C27D1B, "sceRtcGetCurrentClockLocalTime", sceRtcGetCurrentClockLocalTime},
};

// ---------------------------------------------------------------- sceUmdUser
u32 g_umdCallback = 0;
void sceUmdActivate(PspContext* c) {
    if (g_umdCallback) kernel::notifyCallback(g_umdCallback, 0x32);
    RET(0);
}
void sceUmdGetDriveStat(PspContext* c) { RET(0x32); }
void sceUmdCheckMedium(PspContext* c) { RET(1); }
void sceUmdRegisterUMDCallBack(PspContext* c) {
    g_umdCallback = A0;
    RET(0);
}
void sceUmdUnRegisterUMDCallBack(PspContext* c) {
    g_umdCallback = 0;
    RET(0);
}
void sceUmdGetDiscInfo(PspContext* c) {
    if (A0) {
        wr32(A0, 8);
        wr32(A0 + 4, 0x10);
    }
    RET(0);
}
const HleFunction kUmd[] = {
    {0xC6183D47, "sceUmdActivate", sceUmdActivate},
    {0x6B4A146C, "sceUmdGetDriveStat", sceUmdGetDriveStat},
    {0x46EBB729, "sceUmdCheckMedium", sceUmdCheckMedium},
    {0x8EF08FCE, "sceUmdWaitDriveStat", retZero},
    {0x4A9E5E29, "sceUmdWaitDriveStatCB", retZero},
    {0x56202973, "sceUmdWaitDriveStatWithTimer", retZero},
    {0x6AF9B50A, "sceUmdCancelWaitDriveStat", retZero},
    {0xAEE7404D, "sceUmdRegisterUMDCallBack", sceUmdRegisterUMDCallBack},
    {0xBD2BDE07, "sceUmdUnRegisterUMDCallBack", sceUmdUnRegisterUMDCallBack},
    {0x340B7686, "sceUmdGetDiscInfo", sceUmdGetDiscInfo},
    {0xE83742BA, "sceUmdDeactivate", retZero},
};

// ---------------------------------------------------------------- StdioForUser
const HleFunction kStdio[] = {
    {0x172D316E, "sceKernelStdin", [](PspContext* c) { RET(0); }},
    {0xA6BAB2E9, "sceKernelStdout", [](PspContext* c) { RET(1); }},
    {0xF78BA90A, "sceKernelStderr", [](PspContext* c) { RET(2); }},
};

// ---------------------------------------------------------------- misc
const HleFunction kImpose[] = {
    {0x36AA6E91, "sceImposeSetLanguageMode", retZero},
};
void sceWlanGetEtherAddr(PspContext* c) {
    const u8 mac[6] = {0x02, 0x56, 0x43, 0x32, 0x00, 0x01};  // locally administered address
    memcpy(memPtr(A0), mac, 6);
    RET(0);
}
const HleFunction kWlan[] = {
    {0x0C622081, "sceWlanGetEtherAddr", sceWlanGetEtherAddr},
    {0xD7763699, "sceWlanGetSwitchState", retZero},
};
const HleFunction kNpDrm[] = {
    {0xA1336091, "sceNpDrmSetLicenseeKey", retZero},
    {0x08D98894, "sceNpDrmEdataSetupKey", retZero},
};
void sceOpenPSIDGetOpenPSID(PspContext* c) {
    for (u32 i = 0; i < 16; i++) wr8(A0 + i, (u8)(0x10 + i));
    RET(0);
}
const HleFunction kPsid[] = {
    {0xC69BEBCE, "sceOpenPSIDGetOpenPSID", sceOpenPSIDGetOpenPSID},
};

// Local wireless multiplayer is not supported: initialisation succeeds, everything that would
// create a session fails with a generic ad hoc error so the game reports "no connection".
constexpr u32 kAdhocErr = 0x80410701;
void adhocFail(PspContext* c) { RET(kAdhocErr); }
void sceNetGetMallocStat(PspContext* c) {
    if (A0) {
        wr32(A0, 0x20000);
        wr32(A0 + 4, 0x20000);
        wr32(A0 + 8, 0x20000);
    }
    RET(0);
}
const HleFunction kNet[] = {
    {0x39AF39A6, "sceNetInit", retZero},
    {0x281928A9, "sceNetTerm", retZero},
    {0xCC393E48, "sceNetGetMallocStat", sceNetGetMallocStat},
};
const HleFunction kAdhoc[] = {
    {0xE1D621D7, "sceNetAdhocInit", retZero},
    {0xA62C6F57, "sceNetAdhocTerm", retZero},
    {0x6F92741B, "sceNetAdhocPdpCreate", adhocFail},
    {0x7F27BB5E, "sceNetAdhocPdpDelete", retZero},
    {0xABED3790, "sceNetAdhocPdpSend", adhocFail},
    {0xDFE53E03, "sceNetAdhocPdpRecv", adhocFail},
    {0x877F6D66, "sceNetAdhocPtpOpen", adhocFail},
    {0xFC6FC07B, "sceNetAdhocPtpConnect", adhocFail},
    {0xE08BDAC1, "sceNetAdhocPtpListen", adhocFail},
    {0x9DF81198, "sceNetAdhocPtpAccept", adhocFail},
    {0x4DA4C788, "sceNetAdhocPtpSend", adhocFail},
    {0x8BEA2B3E, "sceNetAdhocPtpRecv", adhocFail},
    {0x157E6225, "sceNetAdhocPtpClose", retZero},
};
const HleFunction kAdhocctl[] = {
    {0xE26F226E, "sceNetAdhocctlInit", retZero},
    {0x9D689E13, "sceNetAdhocctlTerm", retZero},
    {0x0AD043ED, "sceNetAdhocctlConnect", adhocFail},
    {0xEC0635C1, "sceNetAdhocctlCreate", adhocFail},
    {0x5E7F79C9, "sceNetAdhocctlJoin", adhocFail},
    {0x08FFF7A0, "sceNetAdhocctlScan", adhocFail},
    {0x34401D65, "sceNetAdhocctlDisconnect", retZero},
    {0x20B317A0, "sceNetAdhocctlAddHandler", [](PspContext* c) { RET(1); }},
    {0x6402490B, "sceNetAdhocctlDelHandler", retZero},
    {0x81AEE1BE, "sceNetAdhocctlGetScanInfo", adhocFail},
    {0xE162CB14, "sceNetAdhocctlGetPeerList", adhocFail},
};
const HleFunction kMatching[] = {
    {0x2A2A1E07, "sceNetAdhocMatchingInit", retZero},
    {0x7945ECDA, "sceNetAdhocMatchingTerm", retZero},
    {0xCA5EDA6F, "sceNetAdhocMatchingCreate", adhocFail},
    {0x93EF3843, "sceNetAdhocMatchingStart", adhocFail},
    {0x32B156B3, "sceNetAdhocMatchingStop", retZero},
    {0xF16EAF4F, "sceNetAdhocMatchingDelete", retZero},
    {0x5E3D4B79, "sceNetAdhocMatchingSelectTarget", adhocFail},
    {0xEA3C6108, "sceNetAdhocMatchingCancelTarget", retZero},
    {0xB58E61B7, "sceNetAdhocMatchingSetHelloOpt", retZero},
};

}  // namespace

HLE_MODULE("UtilsForUser", kUtils);
HLE_MODULE("sceSuspendForUser", kSuspend);
HLE_MODULE("scePower", kPower);
HLE_MODULE("sceRtc", kRtc);
HLE_MODULE("sceUmdUser", kUmd);
HLE_MODULE("StdioForUser", kStdio);
HLE_MODULE("sceImpose", kImpose);
HLE_MODULE("sceWlanDrv", kWlan);
HLE_MODULE("scePspNpDrm_user", kNpDrm);
HLE_MODULE("sceOpenPSID", kPsid);
HLE_MODULE("sceNet", kNet);
HLE_MODULE("sceNetAdhoc", kAdhoc);
HLE_MODULE("sceNetAdhocctl", kAdhocctl);
HLE_MODULE("sceNetAdhocMatching", kMatching);

}  // namespace vc2rt
