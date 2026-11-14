// Opt-in self test of the sceUtility / sceLibFont HLE (set VC2_UTIL_SELFTEST=1).
//
// Runs once from inside the game's first sceUtilityGetSystemParamInt call (savedata + OSK) and its
// first sceFontOpen (font rendering), driving the real HLE entry points with request blocks built in
// guest memory and checking PSP status/result semantics and data round-trips. Results are logged
// under the "TEST" tag; test saves use the game name VC2TEST00 and are deleted afterwards.
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <filesystem>
#include <fstream>
#include <functional>

#include "../../common/sfo.h"
#include "../kernel/kernel.h"
#include "hle.h"
#include "utility.h"

namespace vc2rt::utility {

namespace {

int g_fail = 0, g_pass = 0;

void check(bool ok, const char* what, u32 got = 0, u32 want = 0) {
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        LOG_ERROR("TEST", "FAIL: %s (got %08x, want %08x)", what, got, want);
    }
}

u32 call(PspContext* base, const char* lib, u32 nid, std::initializer_list<u32> args) {
    const HleFunction* f = findHle(lib, nid);
    if (!f) {
        LOG_ERROR("TEST", "missing HLE %s:%08x", lib, nid);
        return 0xFFFFFFFF;
    }
    PspContext ctx = *base;
    int i = 4;
    for (u32 a : args) ctx.r[i++] = a;
    f->fn(&ctx);
    return ctx.r[2];
}

enum : u32 {
    NID_SD_INIT = 0x50C4CD57, NID_SD_STATUS = 0x8874DBE0, NID_SD_SHUTDOWN = 0x9790B33C, NID_SD_UPDATE = 0xD4B95FFB,
    NID_OSK_INIT = 0xF6269B82, NID_OSK_STATUS = 0xF3F76017, NID_OSK_UPDATE = 0x4B85C861, NID_OSK_SHUTDOWN = 0x3DFAEBA9,
};

// 1x1 PNG
const u8 kPng[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
                   0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
                   0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
                   0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99, 0x3D, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
                   0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

struct Mem {
    u32 base = 0, uid = 0, top = 0;
    u32 alloc(u32 n) {
        u32 a = top;
        top += (n + 15) & ~15u;
        memset(memPtr(a), 0, n);
        return a;
    }
};

// Runs a dialog through its whole life cycle; `onRunning` is invoked once the dialog is RUNNING
// (it calls Update as needed). Returns common.result.
u32 runSavedata(PspContext* c, u32 req, const std::function<void()>& onRunning = {}) {
    u32 r = call(c, "sceUtility", NID_SD_INIT, {req});
    check(r == 0, "SavedataInitStart", r, 0);
    if (r != 0) return 0xDEAD;
    u32 st = call(c, "sceUtility", NID_SD_STATUS, {});
    check(st == ST_INIT, "status after InitStart is INIT", st, ST_INIT);
    u32 busy = call(c, "sceUtility", NID_SD_INIT, {req});
    check(busy == ERR_UTILITY_INVALID_STATUS, "second InitStart while busy", busy, ERR_UTILITY_INVALID_STATUS);
    u32 early = call(c, "sceUtility", NID_SD_UPDATE, {1});
    check(early == ERR_UTILITY_INVALID_STATUS, "Update during INIT", early, ERR_UTILITY_INVALID_STATUS);
    u32 sd = call(c, "sceUtility", NID_SD_SHUTDOWN, {});
    check(sd == ERR_UTILITY_INVALID_STATUS, "ShutdownStart before FINISHED", sd, ERR_UTILITY_INVALID_STATUS);
    u32 osk = call(c, "sceUtility", NID_OSK_STATUS, {});
    check(osk == ERR_UTILITY_WRONG_TYPE, "OSK GetStatus while savedata active", osk, ERR_UTILITY_WRONG_TYPE);
    for (int i = 0; i < 100 && st != ST_RUNNING; i++) {
        Sleep(10);
        st = call(c, "sceUtility", NID_SD_STATUS, {});
    }
    check(st == ST_RUNNING, "status reaches RUNNING", st, ST_RUNNING);
    if (onRunning) onRunning();
    else call(c, "sceUtility", NID_SD_UPDATE, {1});
    st = call(c, "sceUtility", NID_SD_STATUS, {});
    check(st == ST_FINISHED, "status FINISHED after Update", st, ST_FINISHED);
    r = call(c, "sceUtility", NID_SD_SHUTDOWN, {});
    check(r == 0, "ShutdownStart", r, 0);
    st = call(c, "sceUtility", NID_SD_STATUS, {});
    check(st == ST_SHUTDOWN || st == ST_NONE, "status SHUTDOWN after ShutdownStart", st, ST_SHUTDOWN);
    for (int i = 0; i < 100 && st != ST_NONE; i++) {
        Sleep(2);
        st = call(c, "sceUtility", NID_SD_STATUS, {});
    }
    check(st == ST_NONE, "status back to NONE", st, ST_NONE);
    return rd32(req + 0x1C);
}

void buildRequest(u32 req, u32 mode, const char* save, const char* file, u32 dataBuf, u32 bufSize, u32 dataSize) {
    memset(memPtr(req), 0, 1536);
    wr32(req, 1536);
    wr32(req + 4, 1);  // English
    wr32(req + 8, 1);  // Cross confirms
    wr32(req + 0x30, mode);
    writeCString(req + 0x3C, "VC2TEST00", 13);
    writeCString(req + 0x4C, save, 20);
    writeCString(req + 0x64, file, 13);
    wr32(req + 0x74, dataBuf);
    wr32(req + 0x78, bufSize);
    wr32(req + 0x7C, dataSize);
    wr32(req + 0x5EC, 1);  // secureVersion 1: no key needed
}

void savedataTests(PspContext* c, Mem& m) {
    namespace fs = std::filesystem;
    const u32 req = m.alloc(1536), data = m.alloc(8192), back = m.alloc(8192), icon = m.alloc(256),
              iconBack = m.alloc(256), list = m.alloc(1024), files = m.alloc(1024), sizes = m.alloc(256);
    for (u32 i = 0; i < 5000; i++) wr8(data + i, (u8)(i * 7 + 3));
    memcpy(memPtr(icon), kPng, sizeof kPng);

    // AUTOSAVE (secure, stored plain)
    buildRequest(req, 1, "0000", "DATA.BIN", data, 8192, 5000);
    writeCString(req + 0x80, "Valkyria Chronicles II", 0x80);
    writeCString(req + 0x100, "Self test save", 0x80);
    writeCString(req + 0x180, "Squad G\nChapter 1", 0x400);
    wr32(req + 0x584, icon);
    wr32(req + 0x588, 256);
    wr32(req + 0x58C, sizeof kPng);
    u32 r = runSavedata(c, req);
    check(r == 0, "AUTOSAVE result", r, 0);
    fs::path dir = paths().saveDir / "PSP" / "SAVEDATA" / "VC2TEST000000";
    check(fs::exists(dir / "DATA.BIN") && fs::file_size(dir / "DATA.BIN") == 5000, "DATA.BIN written plain");
    check(fs::exists(dir / "ICON0.PNG") && fs::file_size(dir / "ICON0.PNG") == sizeof kPng, "ICON0.PNG written");
    {
        std::ifstream f(dir / "PARAM.SFO", std::ios::binary);
        ::Bytes b((std::istreambuf_iterator<char>(f)), {});
        vc2::SfoData sfo;
        bool ok = vc2::parseSfo(b, sfo);
        check(ok, "PARAM.SFO parses");
        check(sfo.get("TITLE") == "Valkyria Chronicles II", "SFO TITLE");
        check(sfo.get("SAVEDATA_TITLE") == "Self test save", "SFO SAVEDATA_TITLE");
        check(sfo.get("SAVEDATA_DETAIL") == "Squad G\nChapter 1", "SFO SAVEDATA_DETAIL");
        check(sfo.get("SAVEDATA_DIRECTORY") == "VC2TEST000000", "SFO SAVEDATA_DIRECTORY");
        check(sfo.get("CATEGORY") == "MS", "SFO CATEGORY");
        check(sfo.get("SAVEDATA_FILE_LIST").rfind("DATA.BIN", 0) == 0, "SFO SAVEDATA_FILE_LIST has DATA.BIN");
    }

    // AUTOLOAD round trip
    buildRequest(req, 0, "0000", "DATA.BIN", back, 8192, 0);
    wr32(req + 0x584, iconBack);
    wr32(req + 0x588, 256);
    r = runSavedata(c, req);
    check(r == 0, "AUTOLOAD result", r, 0);
    check(rd32(req + 0x7C) == 5000, "AUTOLOAD dataSize", rd32(req + 0x7C), 5000);
    check(memcmp(memPtr(back), memPtr(data), 5000) == 0, "AUTOLOAD data round-trips");
    check(readCString(req + 0x100) == "Self test save", "AUTOLOAD sfoParam.savedataTitle");
    check(rd32(req + 0x34) == 1021, "AUTOLOAD bind", rd32(req + 0x34), 1021);
    check(rd32(req + 0x58C) == sizeof kPng && memcmp(memPtr(iconBack), kPng, sizeof kPng) == 0, "AUTOLOAD icon0");

    // missing data
    buildRequest(req, 0, "9999", "DATA.BIN", back, 8192, 0);
    r = runSavedata(c, req);
    check(r == SAVE_ERR_LOAD_NO_DATA, "AUTOLOAD missing -> LOAD_NO_DATA", r, SAVE_ERR_LOAD_NO_DATA);
    buildRequest(req, 15, "9999", "DATA.BIN", back, 8192, 0);
    r = runSavedata(c, req);
    check(r == SAVE_ERR_RW_NO_DATA, "READDATASECURE missing -> RW_NO_DATA", r, SAVE_ERR_RW_NO_DATA);
    buildRequest(req, 0, "0000", "NOPE.BIN", back, 8192, 0);
    r = runSavedata(c, req);
    check(r == SAVE_ERR_LOAD_FILE_NOT_FOUND, "AUTOLOAD missing file", r, SAVE_ERR_LOAD_FILE_NOT_FOUND);
    buildRequest(req, 0, "0000", "DATA.BIN", back, 100, 0);
    r = runSavedata(c, req);
    check(r == SAVE_ERR_LOAD_DATA_BROKEN, "AUTOLOAD buffer too small", r, SAVE_ERR_LOAD_DATA_BROKEN);

    // WRITEDATA (normal file) + FILES
    buildRequest(req, 18, "0000", "EXTRA.BIN", data, 8192, 100);
    r = runSavedata(c, req);
    check(r == 0, "WRITEDATA result", r, 0);
    buildRequest(req, 12, "0000", "", 0, 0, 0);
    wr32(files + 0, 5);
    wr32(files + 4, 5);
    wr32(files + 8, 5);
    u32 ents = m.alloc(80 * 15);
    wr32(files + 24, ents);
    wr32(files + 28, ents + 80 * 5);
    wr32(files + 32, ents + 80 * 10);
    wr32(req + 0x5F8, files);
    r = runSavedata(c, req);
    check(r == 0, "FILES result", r, 0);
    check(rd32(files + 12) == 1 && readCString(ents + 64) == "DATA.BIN" && rd64(ents + 8) == 5000, "FILES secure entry",
          rd32(files + 12), 1);
    check(rd32(files + 16) == 1 && readCString(ents + 80 * 5 + 64) == "EXTRA.BIN", "FILES normal entry",
          rd32(files + 16), 1);
    check(rd32(files + 20) == 2, "FILES system entries (ICON0, PARAM.SFO)", rd32(files + 20), 2);

    // ERASE the normal file
    buildRequest(req, 20, "0000", "EXTRA.BIN", 0, 0, 0);
    r = runSavedata(c, req);
    check(r == 0 && !fs::exists(dir / "EXTRA.BIN"), "ERASE", r, 0);

    // LIST
    buildRequest(req, 11, "<>", "", 0, 0, 0);
    u32 ids = m.alloc(72 * 4);
    wr32(list, 4);
    wr32(list + 8, ids);
    wr32(req + 0x5F4, list);
    r = runSavedata(c, req);
    check(r == 0 && rd32(list + 4) == 1 && readCString(ids + 52) == "0000", "LIST finds 0000", rd32(list + 4), 1);
    check(rd32(ids) == 0x11FF && rd16(ids + 36) >= 2024, "LIST entry mode/mtime");

    // SIZES
    buildRequest(req, 8, "0000", "DATA.BIN", data, 8192, 5000);
    u32 msData = sizes + 64;
    wr32(req + 0x5D0, sizes);
    writeCString(msData, "VC2TEST00", 13);
    writeCString(msData + 16, "0000", 20);
    wr32(req + 0x5D4, msData);
    wr32(req + 0x5D8, sizes + 160);
    r = runSavedata(c, req);
    check(r == 0, "SIZES result", r, 0);
    check(rd32(sizes) == 0x8000 && rd32(sizes + 8) > 0, "SIZES msFree");
    check(rd32(msData + 36) >= 3, "SIZES msData clusters", rd32(msData + 36), 3);
    check(rd32(sizes + 160) == 3, "SIZES utilityData clusters", rd32(sizes + 160), 3);
    LOG_INFO("TEST", "SIZES: free %u KB \"%.8s\", used %u KB \"%.8s\", needed %u KB \"%.8s\"", rd32(sizes + 8),
             (const char*)memPtr(sizes + 12), rd32(msData + 40), (const char*)memPtr(msData + 44), rd32(sizes + 164),
             (const char*)memPtr(sizes + 168));

    // GETSIZE
    buildRequest(req, 22, "0000", "", 0, 0, 0);
    u32 si = sizes + 200, se = m.alloc(24);
    wr64(se, 5000);
    writeCString(se + 8, "DATA.BIN", 16);
    wr32(si, 1);
    wr32(si + 8, se);
    wr32(req + 0x5FC, si);
    r = runSavedata(c, req);
    check(r == 0 && rd32(si + 16) == 0x8000 && rd32(si + 36) == 0, "GETSIZE", r, 0);

    // LISTSAVE through the UI: pick the second (empty) slot, then confirm the message
    u32 names = m.alloc(80);
    writeCString(names, "0000", 20);
    writeCString(names + 20, "0001", 20);
    buildRequest(req, 5, "0000", "DATA.BIN", data, 8192, 1234);
    writeCString(req + 0x80, "Valkyria Chronicles II", 0x80);
    writeCString(req + 0x100, "Slot two", 0x80);
    wr32(req + 0x60, names);
    r = runSavedata(c, req, [&] {
        call(c, "sceUtility", NID_SD_UPDATE, {1});
        check(ui().screen == UiScreen::List && ui().slots.size() == 2, "LISTSAVE shows 2 slots", (u32)ui().slots.size(), 2);
        check(ui().slots.size() == 2 && ui().slots[0].exists && !ui().slots[1].exists, "LISTSAVE slot states");
        ui().selected = 1;
        ui().event = UiEvent::Select;
        call(c, "sceUtility", NID_SD_UPDATE, {1});
        check(ui().screen == UiScreen::Message, "LISTSAVE completion message");
        ui().event = UiEvent::Ok;
        call(c, "sceUtility", NID_SD_UPDATE, {1});
    });
    check(r == 0 && readCString(req + 0x4C) == "0001", "LISTSAVE saved into 0001", r, 0);
    check(fs::exists(paths().saveDir / "PSP" / "SAVEDATA" / "VC2TEST000001" / "DATA.BIN"), "LISTSAVE file exists");

    // LISTSAVE onto the existing slot: overwrite confirmation, answer No -> back to list, cancel
    r = runSavedata(c, req, [&] {
        call(c, "sceUtility", NID_SD_UPDATE, {1});
        ui().selected = 0;
        ui().event = UiEvent::Select;
        call(c, "sceUtility", NID_SD_UPDATE, {1});
        check(ui().screen == UiScreen::Confirm && !ui().yes, "overwrite confirmation defaults to No");
        ui().event = UiEvent::No;
        call(c, "sceUtility", NID_SD_UPDATE, {1});
        check(ui().screen == UiScreen::List, "No returns to the list");
        ui().event = UiEvent::Cancel;
        call(c, "sceUtility", NID_SD_UPDATE, {1});
    });
    check(r == DIALOG_RESULT_CANCEL, "LISTSAVE cancelled -> result 1", r, DIALOG_RESULT_CANCEL);

    // LISTLOAD: choose 0001
    buildRequest(req, 4, "0000", "DATA.BIN", back, 8192, 0);
    wr32(req + 0x60, names);
    wr32(req + 0x5C8, 3);  // focus latest
    memset(memPtr(back), 0, 8192);
    r = runSavedata(c, req, [&] {
        call(c, "sceUtility", NID_SD_UPDATE, {1});
        check(ui().selected == 1, "LISTLOAD focus LATEST picks newest slot", ui().selected, 1);
        ui().event = UiEvent::Select;
        call(c, "sceUtility", NID_SD_UPDATE, {1});
        ui().event = UiEvent::Ok;
        call(c, "sceUtility", NID_SD_UPDATE, {1});
    });
    check(r == 0 && rd32(req + 0x7C) == 1234 && readCString(req + 0x4C) == "0001", "LISTLOAD loads 0001", rd32(req + 0x7C), 1234);

    // DELETEDATA both
    buildRequest(req, 21, "0001", "", 0, 0, 0);
    r = runSavedata(c, req);
    check(r == 0, "DELETEDATA 0001", r, 0);
    buildRequest(req, 21, "0000", "", 0, 0, 0);
    r = runSavedata(c, req);
    check(r == 0 && !fs::exists(dir), "DELETEDATA 0000", r, 0);
    buildRequest(req, 21, "0000", "", 0, 0, 0);
    r = runSavedata(c, req);
    check(r == SAVE_ERR_RW_NO_DATA, "DELETEDATA missing", r, SAVE_ERR_RW_NO_DATA);
}

void oskTests(PspContext* c, Mem& m) {
    const u32 params = m.alloc(0x40), field = m.alloc(0x34), desc = m.alloc(64), in = m.alloc(64), out = m.alloc(64);
    const char16_t d[] = u"Name", i[] = u"Welkin";
    memcpy(memPtr(desc), d, sizeof d);
    memcpy(memPtr(in), i, sizeof i);
    wr32(params, 0x40);
    wr32(params + 0x30, 1);
    wr32(params + 0x34, field);
    wr32(field + 0x1C, desc);
    wr32(field + 0x20, in);
    wr32(field + 0x24, 17);  // 16 chars + terminator
    wr32(field + 0x28, out);
    wr32(field + 0x30, 10);  // limit 10
    wr32(field + 0x14, 1);
    u32 r = call(c, "sceUtility", NID_OSK_INIT, {params});
    check(r == 0, "OskInitStart", r, 0);
    u32 st = call(c, "sceUtility", NID_OSK_STATUS, {});
    check(st == ST_INIT, "OSK INIT", st, ST_INIT);
    u32 sdst = call(c, "sceUtility", NID_SD_STATUS, {});
    check(sdst == ERR_UTILITY_WRONG_TYPE, "Savedata GetStatus while OSK active", sdst, ERR_UTILITY_WRONG_TYPE);
    for (int k = 0; k < 100 && st != ST_RUNNING; k++) {
        Sleep(10);
        st = call(c, "sceUtility", NID_OSK_STATUS, {});
    }
    check(st == ST_RUNNING, "OSK RUNNING", st, ST_RUNNING);
    call(c, "sceUtility", NID_OSK_UPDATE, {1});
    check(ui().screen == UiScreen::Osk && ui().oskText == "Welkin" && ui().oskMaxChars == 10, "OSK UI shown");
    ui().oskText = "Avan Hardins";  // longer than the 10 char limit: must be truncated
    ui().event = UiEvent::Ok;
    call(c, "sceUtility", NID_OSK_UPDATE, {1});
    st = call(c, "sceUtility", NID_OSK_STATUS, {});
    check(st == ST_FINISHED, "OSK FINISHED", st, ST_FINISHED);
    std::u16string got = readGuestUtf16(out);
    check(got == u"Avan Hardi", "OSK output truncated to the limit (UTF-16)");
    check(rd32(field + 0x2C) == 2, "OSK field result CHANGED", rd32(field + 0x2C), 2);
    r = call(c, "sceUtility", NID_OSK_SHUTDOWN, {});
    check(r == 0, "OskShutdownStart", r, 0);
    for (int k = 0; k < 100 && st != ST_NONE; k++) {
        Sleep(5);
        st = call(c, "sceUtility", NID_OSK_STATUS, {});
    }
    check(st == ST_NONE, "OSK back to NONE", st, ST_NONE);
}

}  // namespace

// Renders a line of text through sceFontGetFontInfo/GetCharInfo/GetCharGlyphImage the way VC2 lays
// glyphs out, in 8bpp and 4bpp, and writes the buffers as PGM images next to the log.
void runFontSelfTest(PspContext* c, u32 font) {
    char buf[8];
    if (!GetEnvironmentVariableA("VC2_UTIL_SELFTEST", buf, sizeof buf)) return;
    static int run = 0;
    if (run >= 2) return;
    const int W = 320, H = 24;
    u32 addr = 0;
    u32 uid = kernel::allocUserMemory(0x4000, 1, 0, "font selftest", &addr);
    if (!uid) return;
    const u32 fi = addr, ci = addr + 0x200, img = addr + 0x300, pix = addr + 0x400;
    const u32 NID_INFO = 0x0DA7535E, NID_CHAR = 0xDCC80C2F, NID_GLYPH = 0x980F4895;
    u32 r = call(c, "sceLibFont", NID_INFO, {font, fi});
    check(r == 0, "sceFontGetFontInfo", r, 0);
    const s32 baseY = (s32)rd32(fi + 20), desc = (s32)rd32(fi + 12);
    const int cellH = rd16(fi + 82) + 1;
    for (int fmt : {2, 0}) {  // 8bpp, 4bpp
        memset(memPtr(pix), 0, W * H);
        const int bpl = fmt == 2 ? W : W / 2;
        s32 pen = 2 * 64;
        for (const char* s = "Valkyria Chronicles II - Squad 7: gjpq"; *s; s++) {
            r = call(c, "sceLibFont", NID_CHAR, {font, (u32)(u8)*s, ci});
            if (r != 0) continue;
            const s32 top = (s32)rd32(ci + 12), left = (s32)rd32(ci + 8);
            // VC2: y = round((maxGlyphBaseY + maxGlyphDescender) / 64) - bitmapTop + 2
            const s32 y = (((baseY + desc) + 32) >> 6) - top + 2;
            wr32(img, (u32)fmt);
            wr32(img + 4, (u32)(pen + left * 64));
            wr32(img + 8, (u32)(y * 64));
            wr16(img + 12, W);
            wr16(img + 14, (u16)std::min(H, cellH));
            wr16(img + 16, (u16)bpl);
            wr32(img + 20, pix);
            call(c, "sceLibFont", NID_GLYPH, {font, (u32)(u8)*s, img});
            pen += (s32)rd32(ci + 48);
        }
        std::filesystem::path out = paths().logFile.parent_path() / ("font_selftest_" + std::to_string(run) + (fmt == 2 ? "_8bpp.pgm" : "_4bpp.pgm"));
        std::ofstream f(out, std::ios::binary);
        f << "P5 " << W << " " << H << " 255\n";
        int nonzero = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                u8 v = fmt == 2 ? rd8(pix + y * bpl + x) : (u8)(((rd8(pix + y * bpl + x / 2) >> ((x & 1) * 4)) & 0xF) * 17);
                nonzero += v != 0;
                f.put((char)v);
            }
        check(nonzero > 200, "font rendered ink", nonzero, 200);
        LOG_INFO("TEST", "font self test: cell %d px, baseline row %d, %d inked pixels -> %s", cellH,
                 (((baseY + desc) + 32) >> 6) + 2, nonzero, pathToUtf8(out).c_str());
    }
    run++;
    kernel::freeUserMemory(uid);
    LOG_INFO("TEST", "sceLibFont self test: %d passed, %d failed (cumulative)", g_pass, g_fail);
}

void runUtilitySelfTest(PspContext* c) {
    static bool done = false;
    char buf[8];
    if (done || !GetEnvironmentVariableA("VC2_UTIL_SELFTEST", buf, sizeof buf)) return;
    done = true;
    Mem m;
    m.uid = kernel::allocUserMemory(0x10000, 1, 0, "utility selftest", &m.base);
    if (!m.uid) {
        LOG_ERROR("TEST", "no memory for the self test");
        return;
    }
    m.top = m.base;
    savedataTests(c, m);
    oskTests(c, m);
    kernel::freeUserMemory(m.uid);
    ui().screen = UiScreen::Hidden;
    LOG_INFO("TEST", "sceUtility self test: %d passed, %d failed", g_pass, g_fail);

    // VC2_UTIL_SELFTEST=list|confirm|osk: leave that dialog screen up (fed with the saves on the
    // memory stick) so the overlay can be inspected; the game keeps running underneath.
    std::string mode(buf);
    if (mode != "list" && mode != "confirm" && mode != "osk") return;
    namespace fs = std::filesystem;
    UiState& u = ui();
    u.slots.clear();
    std::error_code ec;
    for (auto& d : fs::directory_iterator(paths().saveDir / "PSP" / "SAVEDATA", ec)) {
        SlotView v;
        auto n = d.path().filename().u8string();
        v.saveName.assign(n.begin(), n.end());
        v.exists = true;
        std::ifstream sf(d.path() / "PARAM.SFO", std::ios::binary);
        ::Bytes b((std::istreambuf_iterator<char>(sf)), {});
        vc2::SfoData sfo;
        vc2::parseSfo(b, sfo);
        v.title = sfo.get("TITLE");
        v.savedataTitle = sfo.get("SAVEDATA_TITLE");
        v.detail = sfo.get("SAVEDATA_DETAIL");
        v.dateText = "2026/11/08  16:35";
        v.sizeText = "448 KB";
        std::ifstream ic(d.path() / "ICON0.PNG", std::ios::binary);
        v.iconPng = std::make_shared<std::vector<u8>>(std::istreambuf_iterator<char>(ic), std::istreambuf_iterator<char>());
        v.iconId = uiNewIconId();
        u.slots.push_back(v);
    }
    SlotView fresh;
    fresh.saveName = "0001";
    fresh.savedataTitle = "New Saved Data";
    fresh.iconId = uiNewIconId();
    u.slots.push_back(fresh);
    u.heading = mode == "osk" ? "Enter a name for your squad" : "Save";
    u.gameTitle = u.slots.size() > 1 ? u.slots[0].title : "";
    u.selected = 0;
    u.focusSlot = 0;
    u.message = "Do you want to overwrite the saved data?";
    u.yes = false;
    u.oskText = "Squad G";
    u.oskMaxChars = 16;
    u.oskInputType = 0;
    u.oskLines = 1;
    u.event = UiEvent::None;
    uiEnsureRegistered();
    u.screen = mode == "list" ? UiScreen::List : mode == "confirm" ? UiScreen::Confirm : UiScreen::Osk;
}

}  // namespace vc2rt::utility
