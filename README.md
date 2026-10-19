# OGL3D

**English** · [한국어](README.ko.md)

OGL3D renders **OpenGL** games in stereo 3D, the way geo-11 does for DirectX 11 games. It is an `opengl32.dll` proxy: placed next to a game's exe, it makes the game draw every frame for both eyes, then weaves the two images for a lenticular (glasses-free) SR panel such as Acer SpatialLabs or a Leia SR display, using the SR SDK. Without an SR panel it outputs side-by-side or top-and-bottom for any other 3D display. 64-bit and 32-bit games are supported.

The games tested so far needed no per-game shader fixes:

- **Shader injection:** vertex shaders are rewritten as the game compiles them. Fixed-function drawing, immediate mode and display lists get a per-eye projection instead.
- **Right-eye targets:** render targets with the window's aspect ratio are duplicated for the right eye.
- **2D stays flat:** HUD and other 2D elements stay at screen depth.
- **Auto-convergence (default):** measures the two eyes and keeps the nearest content at the screen surface, following camera cuts.

DirectX games are covered by the sister project [geo-11 SR](https://github.com/nautymac/geo-11_SR). From geo-11 SR v0.3.0, its installer also installs OGL3D (kind **OpenGL**).

## Download and install

Download **`OGL3D-v1.0.0.zip`** from [Releases](../../releases):

```
OGL3D\
  x64\opengl32.dll     for 64-bit games
  x86\opengl32.dll     for 32-bit games
  OGL3D.ini            settings (every option is explained inside)
  README.md, README.ko.md, LICENSE
```

1. Find the folder with the game's exe. Task Manager shows "(32 bit)" after the name of a 32-bit game.
2. Copy `opengl32.dll` from `x64` (64-bit game) or `x86` (32-bit game), together with `OGL3D.ini`, into that folder.
3. Start the game as usual. `OGL3D.log` appears next to the exe.

To uninstall, delete `opengl32.dll`, `OGL3D.ini` and `OGL3D.log`.

The [geo-11 SR installer](https://github.com/nautymac/geo-11_SR) (v0.3.0 and later) does this for you. It detects OpenGL games, picks 32/64-bit and sets upscale on or off.

Is the game OpenGL? Its exe imports `opengl32.dll`, or the game offers an OpenGL renderer. The installer checks this automatically. If the wrapper never writes `OGL3D.log`, the game is not using OpenGL.

## Requirements

| Item | Needed? |
|---|---|
| OpenGL game | OpenGL 3.x/4.x. Compatibility profile tested (all verified games); core profile is handled but untested. DirectX games: use geo-11 SR. |
| GPU | Tested on NVIDIA (RTX 4080 Laptop, driver 610.88). Uses framebuffer objects and framebuffer blits. AMD and Intel are untested. |
| SR panel + SR runtime | Only for SR weaving. On Acer SpatialLabs devices, **SpatialLabs Experience Center** alone is enough: it installs the 64-bit runtime (`C:\Program Files\Acer\SpatialLabs\Platform\bin`) and the 32-bit runtime (`C:\Program Files (x86)\Simulated Reality\Platform\bin`). Without the runtime the output falls back to half side-by-side. |
| Visual C++ redistributable | Not needed (statically linked). |
| Visual Studio, CMake, Python | Only to build from source. |

Run games at the panel's native resolution (for example 3840×2160), fullscreen or borderless, or the lenticular pattern will not line up. A game that is not DPI-aware is made DPI-aware so it renders at native resolution. If its UI then becomes too small, use **upscale** mode (below).

## Hotkeys

| Keys | Action |
|---|---|
| Ctrl+F3 / Ctrl+F4 | separation − / + |
| Ctrl+F5 / Ctrl+F6 | convergence − / + (switches auto-convergence off) |
| Ctrl+F2 | auto-convergence on/off |
| Ctrl+F7 | save separation, convergence and shader choices to `OGL3D.ini` |
| Ctrl+F1 | keep the values on screen (otherwise they show only while you adjust) |
| Ctrl+T | 3D on/off |
| Ctrl+Alt+W | SR weaving on/off (off shows side-by-side) |
| Ctrl+F12 | save both eyes as a BMP and log a depth report |
| Ctrl+F9, Ctrl+F10 / F11, Ctrl+F8 | hunting mode: walk through the shaders on screen and set the selected one to stereo, mono (screen depth) or skip |

All keys can be changed in `[Keys]` of `OGL3D.ini`.

## Settings

The main options in `OGL3D.ini`. The file itself explains every option.

| Option | Default | Meaning |
|---|---|---|
| `[Stereo] separation` | 50 | depth strength in percent |
| `[Stereo] convergence` | auto | `auto` follows the scene, or a number (the depth that sits on the screen) |
| `[Stereo] auto_popout` | 0 | auto: how far the nearest content may come out of the screen, % of separation |
| `[Stereo] auto_speed` | 1 | auto: how quickly it follows scene changes (2 = faster, 0.5 = calmer) |
| `[Stereo] output` | auto | `auto` (SR weaving if available, else SBS), `sr`, `sbs`, `tab`, `left`, `right` |
| `[Stereo] swap_eyes` | 0 | swap left and right |
| `[Display] upscale` | 0 | render at the game's resolution, show it woven at native resolution |
| `[Display] dpi_aware` | 1 | make a DPI-unaware game render at native resolution |
| `[Display] virtual_backbuffer` | 1 | the game draws off-screen and the window only gets finished frames (prevents flicker) |
| `[Overlay] show_values` | 0 | values always on screen |
| `[Overlay] cursor` | 1 | SBS / top-and-bottom: the mouse cursor is drawn into each eye (Windows' cursor is hidden over the game) |

### Upscale mode

At 3840×2160 many games' menus and text become tiny. With `upscale = 1`, set the game itself to 1920×1080. The game renders both eyes at that resolution, and OGL3D shows them woven at the panel's native resolution in its own window over the game. Mouse input passes through to the game, and display mode changes are blocked so the panel stays native.

## Verified (2026-10-14)

| Game | Notes |
|---|---|
| Curse of the Dead Gods (64-bit) | Real depth, HUD at screen depth. Manual values work well, e.g. separation 18, convergence 21.7. |
| Amnesia: The Dark Descent (32-bit) | Deferred renderer, fixed-function and immediate mode all in stereo. Recommended: `upscale = 1` with the game at 1920×1080. |
| Darkest Dungeon (64-bit, `_windows\win64`) | Dungeon and town layers have real depth. The intro movie is flat. |
| Broken Age (32-bit) | 2.5D layers with a different depth scale per shot. Auto-convergence follows the cuts. |

Fixed during testing: flicker caused by Windows showing a half-drawn back buffer (virtual back buffer), and a black or white screen when a game left a multiply or additive blend mode on at the end of a frame (OGL3D's own drawing now starts from a neutral state).

## How it works

1. **Proxy.** `opengl32.dll` loads the real one from System32 and forwards every export. It intercepts context creation, `SwapBuffers`, draw calls, clears, framebuffer and texture functions.
2. **Shaders.** Each vertex shader's `main()` is renamed and a new `main()` adds `eye × separation × (w − convergence)` to `gl_Position.x`. Geometry with `w = 1` (2D, HUD, full-screen passes) stays at screen depth. Programs without an injected shader get a per-eye projection matrix.
3. **Right eye.** Every draw into a stereo target (the default framebuffer, or a render target with the window's aspect ratio) is drawn again into a right-eye copy, with the right-eye copies of the textures it reads.
4. **Composition.** At `SwapBuffers` both eyes go side by side into the SR SDK's OpenGL weaver (DirectX 11 weaver in upscale mode). Without SR they go out as SBS or TAB.
5. **Auto-convergence.** Both eyes are shrunk on the GPU, read back without stalling, and block-matched on a worker thread. The nearest well-matched content defines the convergence.

## Troubleshooting

- **The log:** `OGL3D.log` next to the exe records the GL version, injected shaders, render targets, SR status, auto-convergence decisions, frame statistics, crashes (module + offset + stack) and stalls (`HANG?`). `EXCEPTION` lines from `DimencoWeaving*.dll` (or an address without a module) right after start-up come from the SR runtime testing CPU features; it handles them itself, so they matter only if the game then closes.
- **Frame dumps:** **Ctrl+F12** writes `OGL3D_frameNNNNN.bmp` (both eyes) and `_srinput.bmp` (what the weaver received), plus a disparity grid in the log.
- **One eye wrong, or an effect at the wrong depth:** use hunting mode (Ctrl+F9). Step to the shader (the selected one disappears), set it to mono or skip with Ctrl+F8, and save with Ctrl+F7.
- **Flicker hunting:** `[Debug] flicker_probe = 1`. `bin\tools\flickercap.exe <seconds> <folder>` records what the display really shows (DXGI desktop duplication) and saves sheets of single-frame flashes.

## Building

- **Prerequisites:** Visual Studio 2022 or later with C++ and CMake, and Python 3. `tools\gen_exports.py` generates the forwarding stubs from the system's `opengl32.dll`; `build.ps1` runs it.
- **SR SDK:** not included. Point the `SR_SDK` option at the extracted SR SDK (1.34.x) of the matching architecture: `build.ps1 -Arch x64 -DSR_SDK=<win64 SDK>`, `build.ps1 -Arch x86 -DSR_SDK=<win32 SDK>`. Without `-DSR_SDK`, the defaults in `CMakeLists.txt` are used.
- **Build:** `build.ps1 -Arch all` → `bin\x64\opengl32.dll`, `bin\x86\opengl32.dll`.
- **Package:** `package.ps1` → `dist\OGL3D-v<version>.zip`.
- **flickercap:** `cmake -S tools\flickercap -B build-flickercap -A x64` and `cmake --build build-flickercap --config Release` → `bin\tools\flickercap.exe`.

## Credits and license

- **helifax's [OGL-3DVision-Wrapper](https://github.com/helifax/OGL-3DVision-Wrapper)**, the OpenGL 3D Vision wrapper, studied as a reference for OpenGL stereo injection and its game profiles. No code was taken from it.
- **SR SDK / SR Platform:** Leia Inc. / Acer SpatialLabs. Not included; loaded at run time from the installed SR runtime.
- **[geo-11 SR](https://github.com/nautymac/geo-11_SR):** the sister project for DirectX games; its installer also installs OGL3D.

The code in this repository is MIT licensed, see `LICENSE`.

Community: [3D Vision Discord](https://discord.gg/zTnnMrA4GE) · [HelixMod](https://helixmod.blogspot.com/)
