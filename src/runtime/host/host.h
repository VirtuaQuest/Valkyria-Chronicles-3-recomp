#pragma once
// Host platform layer: SDL3 window + OpenGL context, input mapping, audio device.
#include <functional>
#include <string>
#include <vector>

#include "../core/runtime.h"

struct SDL_Window;

namespace vc2rt::host {

// PSP controller bits (SceCtrlData.Buttons)
enum : u32 {
    PSP_SELECT = 0x000001,
    PSP_START = 0x000008,
    PSP_UP = 0x000010,
    PSP_RIGHT = 0x000020,
    PSP_DOWN = 0x000040,
    PSP_LEFT = 0x000080,
    PSP_LTRIGGER = 0x000100,
    PSP_RTRIGGER = 0x000200,
    PSP_TRIANGLE = 0x001000,
    PSP_CIRCLE = 0x002000,
    PSP_CROSS = 0x004000,
    PSP_SQUARE = 0x008000,
    PSP_HOME = 0x010000,
};

struct PadState {
    u32 buttons = 0;
    u8 lx = 128, ly = 128;
};

bool init(const std::string& title);
void shutdown();
SDL_Window* window();

// Processes OS events and refreshes input. Returns false once the user closed the window.
bool pumpEvents();
PadState pad();
bool fastForwardHeld();

// Window drawable size in pixels.
void drawableSize(int& w, int& h);
// Framebuffer the final image (game + overlay) is drawn into: 0 (the window) when presenting on
// the emulation thread, otherwise an offscreen target handed to the presentation thread.
unsigned windowFramebuffer();
void swapBuffers();
u32 presentedFrames();  // frames actually shown in the window (swap count)
void applyDisplaySettings();  // window mode / vsync from config()

// Overlay (settings menu, FPS counter) drawn on top of the game image before swap.
void overlayFrame();
bool overlayWantsInput();
void setGameFps(float fps);  // rate at which the game flips framebuffers (shown in the FPS overlay)

// Additional overlay windows (e.g. the PSP system dialogs). `draw` runs inside overlayFrame()
// between ImGui::NewFrame() and ImGui::Render(). `wantsInput` (optional) is polled every input
// update; while it returns true, keyboard and gamepad input is withheld from the game.
void addOverlayDrawer(std::function<void()> draw, std::function<bool()> wantsInput = {});

// Audio: interleaved stereo s16 at 44100 Hz pushed by the audio mixer.
void audioInit();
void audioPush(const s16* samples, size_t frames);
size_t audioQueuedFrames();
bool audioDeviceOpen();  // false if no playback device could be opened (mixer paces by wall clock)

// Window icon decoded from the disc's ICON0.PNG (never stored on disk by the runtime).
void setIconFromPng(const std::vector<u8>& png);

}  // namespace vc2rt::host
