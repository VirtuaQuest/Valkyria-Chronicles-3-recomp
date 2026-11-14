# Valkyria Chronicles III — PC Recompilation

<a href="https://recomplauncher.com"><picture><source media="(prefers-color-scheme: dark)" srcset="https://recomplauncher.com/assets/press/launcher/badge-light.svg"><img alt="Get it on RecompLauncher" src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" height="48"></picture></a>

A native Windows port of the PSP game *Valkyria Chronicles III* (Japan v1.01, **ULJM05781**, with
the English fan-translation patch applied), produced by **static recompilation**: the game's MIPS
code is translated to C on your machine and compiled into a native executable that runs on a
re-implementation of the PSP system software. It adds modern PC features: rendering at up to 8×
the PSP resolution, keyboard and gamepad controls, borderless or exclusive fullscreen, V-Sync,
anisotropic filtering, texture upscaling, FXAA and an optional 60 FPS mode for battles.

> **You must own the game.** This repository contains no game code, data, artwork or audio. The
> launcher reads everything from *your* ISO dump of the UMD and builds the executable locally.
> Nothing derived from the game (generated code, icons, objects) is ever written into the repo.

## How to play

1. Dump your UMD to an `.iso` (for example with a PSP running custom firmware) and apply the
   English fan-translation patch to it. The recompilation uses the patch's decrypted
   `SYSDIR/EBOOT.OLD`; unpatched Japanese discs are not supported yet.
2. Install the free **Visual Studio 2022 Build Tools** with the *Desktop development with C++*
   workload. The launcher uses it to compile the translated game.
3. Download the latest `VC3Recompiled-<version>-Windows-x64.zip` from
   [Releases](https://github.com/VirtuaQuest/Valkyria-Chronicles-3-recomp/releases), unzip it and
   run `VC3Launcher.exe`. Pick your ISO and an install folder and press **Recompile**. A progress
   bar follows the steps: load → analyze → translate → compile → link. The first build takes
   a few minutes; later runtime updates only relink.
4. Press **Play**.

### With RecompLauncher

[RecompLauncher](https://recomplauncher.com) can install this recomp and set it up from your disc
image: it runs the same recompile without a window (`VC3Launcher.exe --prepare`, with a progress
bar on its setup screen), shows the graphics settings on the game's page, and starts the game with
`VC3Launcher.exe --play`. The setup is described in [`recomplauncher.json`](recomplauncher.json),
which ships at the top of every release. You still need the C++ Build Tools from step 2.

## Controls

| PSP | Keyboard | Gamepad (XInput/SDL) |
|---|---|---|
| Analog stick | W A S D (hold Shift to walk slowly) | Left stick |
| D-pad | Arrow keys | D-pad / right stick |
| ✕ (confirm) | Space / Enter | A (South) |
| ◯ (cancel) | Esc / Backspace | B (East) |
| □ | F | X (West) |
| △ | R | Y (North) |
| L / R | Q / E | LB / RB (or triggers) |
| START / SELECT | Tab / C | Start / Back |

Hotkeys: **F1** opens the settings overlay (or the gamepad Guide button), **F11** or
**Alt+Enter** toggles fullscreen, and holding **`** (backquote) fast-forwards. Keys can be
rebound in `vc3recomp.ini` or the launcher's settings tab. In the story map, **Select** opens the
game's system menu (save, load, options). **Quit game** at the bottom of the F1 menu closes the
game without a keyboard (open the menu with the controller's Guide button).

In battle:

| Mode | Buttons |
|---|---|
| Command mode (map) | ✕ select the unit under the cursor, □ unit list, △ commands, **START** ends your phase early |
| Movement | analog stick moves, □ aims, ◯ crouches behind sandbags, △ swaps the camera, **START** ends the unit's action |
| Aiming | analog stick moves the sights (aim for the head), ✕ fires, □ swaps weapons |

When the game is started from RecompLauncher's Big Picture mode it opens in fullscreen for that
session (F11 returns to your usual window mode).

## Graphics settings

* **Internal resolution:** 1× (480×272, the PSP's native resolution) up to 8× (3840×2176). 3D
  scenes render at the higher resolution instead of being stretched.
* **Aspect:** keep 16:9 / stretch / integer scaling.
* **Texture filtering**, **anisotropic filtering (up to 16×)**, **texture upscaling (2–4×)**,
  **FXAA**, **MSAA**, **V-Sync**, frame limiter, FPS counter.
* **60 FPS gameplay (experimental):** battles and maps run at 30 FPS on the PSP; this draws them
  at 60 FPS without changing the game's speed.

## Files in the install folder

| File | What it is |
|---|---|
| `VC3Recompiled.exe` | the game, built from your disc |
| `vc3runtime.dll` | the PSP system re-implementation the game runs on |
| `vc3recomp.ini` | settings (also edited by the launcher's Settings tab) |
| `vc3recomp.log` | log of the last run (`vc3recomp.1.log` and `.2.log` keep the two before); attach it to bug reports |
| `memstick/PSP/SAVEDATA/` | your saves |

## Build information

Every build records its version, git commit, branch, commit date, build date and compiler:

* the launcher shows the version under its title and the full details on the **About** tab
  (**Copy build information** puts them on the clipboard for bug reports);
* `VC3Launcher.exe --version` prints them;
* Explorer shows them under **Properties > Details** of `VC3Launcher.exe` and `vc3runtime.dll`;
* the game writes them at the top of `vc3recomp.log` and shows the version in the F1 overlay.

The version is the release tag for release builds (`v1.2.0`), `git describe` output between
releases (`v1.2.0-3-gabc1234`), or `v<base>-dev+<commit>` before the first tag; `-dirty` marks
builds with uncommitted changes.

## Building from source

Requirements: Windows 10/11 x64, Visual Studio 2022 (or Build Tools) with CMake and Ninja,
internet access on first configure (SDL3 and Dear ImGui are fetched by CMake).

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Outputs in `build/bin/`: `VC3Launcher.exe`, `vc3runtime.dll`, `sdk/` (import library and headers
the launcher compiles against) and `vc2tool.exe` (developer CLI: `info`, `decrypt` (extracts the plain executable), `imports`,
`analyze`, `dis`, `recomp`).

`tools/package.ps1 -BuildDir build -Version v1.2.3` writes the release zip
(`dist/VC3Recompiled-v1.2.3-Windows-x64.zip`, with `recomplauncher.json` at its top). Pushing a
`v*` tag runs the same build and packaging in GitHub Actions and attaches the zip to the release.

## How it works

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). In short: the game executable is loaded,
relocated and analyzed. Function boundaries and every jump table are recovered from the
relocation table (18,610 functions, with no unresolved indirect jumps), and each MIPS function
becomes a C function. The runtime DLL provides the PSP kernel (threads as fibers), file system
on top of the ISO, GE graphics on OpenGL, audio mixing, input, savedata and the rest of the
system libraries.

## Status

This is a work in progress. See [docs/STATUS.md](docs/STATUS.md) for what works today and what
is still missing.

## Credits

* This project grew out of a Valkyria Chronicles II recompilation; the recompiler and runtime are
  shared with it.
* PSP hardware and OS behaviour was studied from public documentation and from the PPSSPP
  project's source as reference. No PPSSPP code is included.
* [SDL3](https://libsdl.org) (zlib license) and [Dear ImGui](https://github.com/ocornut/imgui)
  (MIT license).
* The ATRAC3plus decoder (`src/runtime/media/atrac3plus/`) is the project's own code, but its
  constant tables (`atrac3plus_tables.cpp`) are derived from [FFmpeg](https://ffmpeg.org)'s ATRAC3+
  decoder by Maxim Poliakovski and are therefore LGPL-2.1-or-later. See
  [LICENSE-NOTICE.md](src/runtime/media/atrac3plus/LICENSE-NOTICE.md).
