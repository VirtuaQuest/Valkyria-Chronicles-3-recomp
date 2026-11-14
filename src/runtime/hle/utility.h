#pragma once
// sceUtility: PSP system dialogs (savedata, on-screen keyboard), utility module loading and system
// parameters. The dialogs are drawn with Dear ImGui on top of the game (utility_ui.cpp).
#include <memory>
#include <string>
#include <vector>

#include "../core/runtime.h"

namespace vc2rt::utility {

// ------------------------------------------------------------------ error codes
enum : u32 {
    ERR_UTILITY_INVALID_STATUS = 0x80110001,
    ERR_UTILITY_INVALID_ADDRESS = 0x80110002,
    ERR_UTILITY_INVALID_PARAM_SIZE = 0x80110004,
    ERR_UTILITY_WRONG_TYPE = 0x80110005,
    ERR_UTILITY_STRING_TOO_LONG = 0x80110102,
    ERR_UTILITY_INVALID_SYSTEM_PARAM_ID = 0x80110103,
    ERR_UTILITY_INVALID_ADHOC_CHANNEL = 0x80110104,
    ERR_MODULE_BAD_ID = 0x80111101,
    ERR_MODULE_ALREADY_LOADED = 0x80111102,
    ERR_MODULE_NOT_LOADED = 0x80111103,

    SAVE_ERR_LOAD_NO_MS = 0x80110301,
    SAVE_ERR_LOAD_ACCESS_ERROR = 0x80110305,
    SAVE_ERR_LOAD_DATA_BROKEN = 0x80110306,
    SAVE_ERR_LOAD_NO_DATA = 0x80110307,
    SAVE_ERR_LOAD_PARAM = 0x80110308,
    SAVE_ERR_LOAD_FILE_NOT_FOUND = 0x80110309,
    SAVE_ERR_RW_NO_MEMSTICK = 0x80110321,
    SAVE_ERR_RW_MEMSTICK_FULL = 0x80110323,
    SAVE_ERR_RW_DATA_BROKEN = 0x80110326,
    SAVE_ERR_RW_NO_DATA = 0x80110327,
    SAVE_ERR_RW_BAD_PARAMS = 0x80110328,
    SAVE_ERR_RW_FILE_NOT_FOUND = 0x80110329,
    SAVE_ERR_RW_BAD_STATUS = 0x8011032c,
    SAVE_ERR_SAVE_NO_MS = 0x80110381,
    SAVE_ERR_SAVE_MS_NOSPACE = 0x80110383,
    SAVE_ERR_SAVE_ACCESS_ERROR = 0x80110385,
    SAVE_ERR_SAVE_PARAM = 0x80110388,
    SAVE_ERR_DELETE_NO_MS = 0x80110341,
    SAVE_ERR_DELETE_ACCESS_ERROR = 0x80110345,
    SAVE_ERR_DELETE_NO_DATA = 0x80110347,
    SAVE_ERR_DELETE_PARAM = 0x80110348,
    SAVE_ERR_SIZES_NO_DATA = 0x801103c7,
};

// Dialog result values written to pspUtilityDialogCommon.result
enum : s32 { DIALOG_RESULT_SUCCESS = 0, DIALOG_RESULT_CANCEL = 1, DIALOG_RESULT_ABORT = 2 };

// ------------------------------------------------------------------ dialog status machine
enum Status : u32 { ST_NONE = 0, ST_INIT = 1, ST_RUNNING = 2, ST_FINISHED = 3, ST_SHUTDOWN = 4 };

struct DialogState {
    Status status = ST_NONE;
    Status pending = ST_NONE;
    u64 pendingAt = 0;  // pspTimeMicros() when `pending` becomes current; 0 = none
    void change(Status s, u64 delayUs);
    Status poll();  // applies a due pending change, returns the current status
    bool busy() { return poll() != ST_NONE; }
};

// Common header of every utility dialog parameter block (pspUtilityDialogCommon, 0x30 bytes).
struct DialogCommon {
    u32 size;
    s32 language, buttonSwap, graphicsThread, accessThread, fontThread, soundThread;
    s32 result;
    s32 reserved[4];
};
static_assert(sizeof(DialogCommon) == 0x30);

// Validates a dialog request block the way the utility driver does (address, accepted sizes, range).
s32 checkRequest(u32 addr, std::initializer_list<u32> sizes);

// ------------------------------------------------------------------ savedata (savedata.cpp)
namespace savedata {
s32 initStart(u32 paramAddr);
s32 update(s32 animSpeed);
s32 shutdownStart();
DialogState& state();
}  // namespace savedata

// ------------------------------------------------------------------ host UI (utility_ui.cpp)
struct SlotView {
    std::string saveName;      // directory suffix (e.g. "0000")
    bool exists = false;
    bool broken = false;
    std::string title, savedataTitle, detail;
    std::string dateText, sizeText;
    std::shared_ptr<std::vector<u8>> iconPng;  // ICON0.PNG bytes (save's own, or the new-data icon)
    u64 iconId = 0;                            // unique per distinct icon blob (texture cache key)
};

enum class UiScreen { Hidden, List, Confirm, Message, Osk };
enum class UiEvent { None, Select, Cancel, Yes, No, Ok };

struct UiState {
    UiScreen screen = UiScreen::Hidden;
    std::string heading;   // "Load" / "Save" / "Delete" / OSK description
    std::string gameTitle; // shown in the header
    // list
    std::vector<SlotView> slots;
    int selected = 0;
    // confirm / message
    std::string message;
    int focusSlot = -1;    // slot shown next to the confirmation text (-1 = none)
    bool yes = true;       // current yes/no choice
    // OSK
    std::string oskText;   // UTF-8 edit buffer
    u32 oskMaxChars = 0;   // in UTF-16 units
    u32 oskInputType = 0;
    int oskLines = 1;
    // output, consumed by the HLE side on its next Update call
    UiEvent event = UiEvent::None;
};

UiState& ui();
void uiEnsureRegistered();  // registers the overlay drawer once (idempotent)
u64 uiNewIconId();

// opt-in self tests (VC2_UTIL_SELFTEST=1), utility_selftest.cpp
void runUtilitySelfTest(PspContext* c);
void runFontSelfTest(PspContext* c, u32 fontHandle);

// text helpers
std::string utf16ToUtf8(const std::u16string& s);
std::u16string utf8ToUtf16(const std::string& s);
std::u16string readGuestUtf16(u32 addr, size_t maxUnits = 4096);

}  // namespace vc2rt::utility
