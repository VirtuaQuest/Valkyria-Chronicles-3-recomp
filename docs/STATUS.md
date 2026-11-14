# Status

## Valkyria Chronicles III (ULJM05781)

The recompilation targets the Japanese release (v1.01) with the English fan translation applied.
On those discs the game executable is the decrypted `EBOOT.OLD`; the patch's custom-firmware
loader (`EBOOT.BIN` + `PROMETHEUS.PRX`) is not needed, and the English text comes from the
patched data. Unpatched discs (encrypted `EBOOT.BIN` only) are not supported yet.

| | |
|---|---|
| Analysis | 18,610 functions, 600 jump tables, 0 unresolved jumps |
| Build | `VC3Recompiled.exe` via the launcher (about 1:50) |
| Verified | boots, notice screens, save creation (`NPJH50491`), title, menus, new-game prompt in English, story prologue with English subtitles, logo and opening movies (the game's own CRI movie path; it never uses the YCbCr API), in-game BGM through the native ATRAC3+ decoder, 10-minute runs without errors, rendering at 3x; title, world map, mission briefing and deployment, and the first battle (tactical map and 3D action phase) played through 11+ turns in a 9-minute run without errors; Chapter 1 story scenes, story map, in-game save from the system menu (Select > Save) and loading it again from the title screen |
| Added for VC3 | game-data install (served from disc `INSDIR`, no 1 GB copy), screenshot utility, YCbCr movie API (advances frames, no pixel output yet), `sceAtracResetPlayPosition`, `Kernel_Library` memset/memcpy/GetThreadId, thunks for imported functions called through pointers |
| Not yet verified | later chapters, long play sessions, unpatched Japanese discs |

## Recompiler and runtime

The recompiler and runtime were first developed for *Valkyria Chronicles II* (ULUS10515) and are
shared with that project; the table below was verified with both games. Last verified on:
Windows 11, MSVC 14.44, GeForce GTX 1660 SUPER.

## Works (verified)

| Area | State | How it was verified |
|---|---|---|
| Executable loading, analysis | ✅ complete | SHA-1 check of the executable; 18,610 functions (VC3), 0 unresolved jumps |
| Disc images | ✅ | `.iso` and `.cso` (CISO v1, own deflate decoder): every file on the VC3 disc reads back bit-identical from a CSO (`vc2tool hashes`), and the game builds and plays from it |
| MIPS → C translation | ✅ complete | every instruction handled (0 `psp_unhandled` sites); compiles warning-free |
| Launcher | ✅ complete | full build 1:48, incremental ~4 s, cancel, icon, settings |
| Kernel (threads, sync, memory, I/O) | ✅ working | game runs its 60 fps loop, streams from disc, creates and reloads saves |
| VFPU | ✅ complete | self-test (`vfpu_test`) passes; all 481 encodings the game uses |
| GE renderer | ✅ working | menus, movies, story scenes, tactical map and 3D battles rendered and compared against expected output |
| Upscaling and graphics options | ✅ | 1–8× internal resolution switchable live; MSAA, FXAA, xBR-style texture upscaling, anisotropy |
| Movies (video) | ✅ | all 28 PMF movies decode via Windows Media Foundation (H.264) |
| Sound effects (SAS) | ✅ | bit-exact path from SAS to the mixer; no underruns at steady state |
| Music and movie audio (ATRAC3+) | ✅ | the runtime's own decoder (`src/runtime/media/atrac3plus/`); `atrac_test` decodes `SND0.AT3`, all movie soundtracks and VC3's BGM files at ~300× real time, matching FFmpeg's decoder at ~135 dB SNR (≤ 1 LSB) |
| Savedata, OSK, fonts | ✅ | game creates its save on a fresh memory stick and reloads it on the next boot |
| Input | ✅ | keyboard and SDL gamepads; scripted input drives the game from boot to battles |

## Known issues

1. Music and movie audio are ATRAC3+. The game's BGM (`.atx`) plays through CRI's ADX+ATRAC
   layer and `sceAtrac3plus`; the first cue is BGM 38, about 2 minutes 20 seconds into a new
   game. The runtime decodes it with its own ATRAC3plus decoder (`src/runtime/media/atrac3plus/`,
   installed at boot; its constant tables are LGPL-2.1+, see `LICENSE-NOTICE.md` there).
   ATRAC3 (non-plus) is not implemented and plays as silence; neither disc contains ATRAC3
   streams. Decoded loud passages clip slightly (up to ~0.2% of samples in a few movies), exactly
   as the reference decoder's output does.
2. Sound effects (SAS) and the CRI ADX path (`OD_STREAM.AFS` / `OD_VOICE.AFS`) work. ADX
   streams are used only by some scenes. Debug aids used for this investigation are in
   `core/crash.cpp`: `VC2_WATCH`, `VC2_BREAK`/`VC2_BREAK2` and `VC2_PRINTF` (hardware watch and
   break points on guest code; `VC2_PRINTF=08bbb4f8` captures the game's sound and file log).
3. A few rare GE blend modes are approximated (logged once when used), and dithering is not
   emulated.
4. Local multiplayer (ad hoc) is not supported; the game reports no connection.
5. Saves are stored unencrypted (compatible only with this port, not with a real PSP).

## Debug environment variables

| Variable | Purpose |
|---|---|
| `VC2_DUMP_DIR`, `VC2_DUMP_EVERY`, `VC2_DUMP_AT` | PNG frame dumps |
| `VC2_INPUT=frame:button[:len],...` | scripted pad input (`from-to/period:button` repeats a press) |
| `VC2_INPUT_AFTER=text[#n]`, `VC2_INPUT_THEN=...` | second input script, timed from the n-th log message containing `text` (e.g. a movie's duration) |
| `VC2_INPUT_LIVE=file` | play interactively: the running game reads commands appended to `file` (`cross 6`, `wait 60`, `stick 0 -1 120`, `shot name` -> `<VC2_DUMP_DIR>/name.png`) |
| `VC2_THREAD_DUMP=1` | log kernel thread states every 5 s (with CPU time, kernel calls and the last call per thread, and the scheduler's timer/idle time) |
| `VC2_SYNC_PRESENT=1`, `VC2_PRESENT_CHECK=1` | swap on the emulation thread (old behaviour) / log the window image brightness every 300 swaps |
| `VC2_AUDIO_DUMP`, `VC2_SAS_DUMP` | WAV dumps of the final mix and the SAS output |
| `VC2_WATCH=addr`, `VC2_BREAK=func`, `VC2_PRINTF=func` | hardware data watch, function call counter, and capture of the game's debug printf |
| `VC2_GE_TRACE`, `VC2_GE_LOG`, `VC2_GPU_STATS`, `VC2_GE_SELFTEST` | GPU diagnostics |
| `--trace`, `--debug` | HLE call trace / debug logging |

### Verification notes (2026-11-13)

* VC2 rebuilt with the current toolchain reaches its first battle (town, briefing, squad
  placement, turn-1 tactical map); logo and opening movies play at correct speed with sound.
* VC3 reaches its first battle and plays 11+ turns (tactical map and 3D action phase).
* Frame dumps (`VC2_DUMP_DIR`) are now encoded on a worker thread. Synchronous PNG encoding
  stalled vblank and audio mixing and visibly slowed movie playback during test runs.
* Scripted input: confirmation dialogs that default to "No" need a cursor press that lands after
  the dialog's opening animation (e.g. `...:left`, `+30:left`, `+60:cross`).

### Frame rate (2026-11-13)

* Measured with the new game-FPS counter (F1 > Show FPS shows "Game" and "Display" rates):
  VC3 menus and boot screens run at 60 FPS, movies at their encoded 30 FPS, and the briefing,
  tactical map and 3D battle at a locked 30 FPS (one complete frame every 2 vblanks, exactly as
  on a PSP). The window always presents at the display rate.
* VC3's engine paces itself with a "set frame rate" function (`0x08bc3ebc`) that also derives a
  float time step used by about 70 logic call sites. A new codegen feature (`GameProfile::hooks`
  -> `psp_hook`) lets the runtime rewrite that request, and the **"60 FPS gameplay
  (experimental)"** setting (`HighFrameRate=1`) renders those scenes at a steady 60 FPS.
* It stays **off by default** until it has had more hands-on play testing. (The "dimmed world
  map" seen in early test runs turned out to be the normal Chapter 1 story map waiting for
  input, not a hang.) VC2 has no equivalent hook yet.

### Presentation thread (2026-11-13)

* VC3 boot sometimes sat on a black screen for ~40 s instead of ~4 s. Cause: the window swap
  with vsync ran on the emulation thread. The PSP vblank (59.94 Hz) drifts slowly against a 60 Hz
  monitor, and for long stretches every swap blocked for a whole refresh (16.6 ms of 16.7), so
  guest threads got almost no CPU time; the game's time-budgeted loader crawled.
* The final image (game + overlay) is now drawn into one of three textures shared with a second
  GL context, and a dedicated thread blits the newest one to the window and swaps. Only that
  thread waits for vsync. Present cost on the emulation thread dropped from 5-16.6 ms to ~0.35 ms
  per frame; 12/12 boots reach the font load at ~4.1 s, and VC3 now reaches the SEGA logo
  ~18 s sooner. VC2 runs unchanged at 59.9 FPS.
