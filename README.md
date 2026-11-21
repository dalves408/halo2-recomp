# Halo 2 static recompilation (Xbox to native Windows x64)

A static recompilation of the original Xbox Halo 2 into a native Windows x64 program,
built on the xboxrecomp toolkit. The x86 game code is lifted to C, and a reimplemented
Xbox kernel, NV2A GPU (Direct3D 11 backend), APU and USB stack run it on a PC.

**Version 1.0** is out: get `Halo2-Recomp-Setup-1.0.0.exe` from
[Releases](https://github.com/dalves408/halo2-recomp/releases/latest).

**No game content is in this repository.** You need your own retail Halo 2 disc. The
recompiled C (`src/recomp/gen/`) is generated from your copy of `default.xbe` and is not
committed.

## Installing

Download `Halo2-Recomp-Setup-<version>.exe` from the latest release and run it. You need:

- **Your Halo 2 disc** (USA, Europe): a disc image (`.iso`/`.xiso`, a full Redump image or a
  trimmed XISO) or a folder with its files.
- **Title update 1.5 from your own console:** the folder `E:\TDATA\4D530064\$u` from its
  hard drive (it holds `default.xbe` and `downloader.xbe`). Leave Setup's update field empty
  if the folder you give already has the update applied, or if the update's two files sit
  beside the disc image.

Setup copies the 79 files the game uses (about 4.7 GB) and checks each one against its
pinned size and SHA-256. It fails with a clear message (wrong disc, damaged dump, missing
title update, folder not writable). No game files (maps, sounds, videos, the disc's
executables) are in the Setup or the release ZIP.

- **Portable:** everything is in the install folder (default `C:\Games\Halo 2 Recomp`):
  the programs, `game_files\` (the disc's files) and `data\` (settings, key map, saves,
  console identity, logs, downloaded content). There is no uninstaller, no Apps entry and
  nothing in the registry or your profile; delete the folder and the shortcuts to remove
  it. Setup refuses a folder it cannot write to, such as Program Files.
- **Update or repair:** run a newer Setup and choose the same folder. It shows the
  installed version and replaces only the program files: no disc needed, and saves,
  settings and the identity are kept. The same version offers Repair or Quit; an older
  Setup warns before downgrading.
- **Shortcuts:** Start menu entries for the game, its settings program and Insignia Setup,
  and an optional desktop shortcut.
- **RecompLauncher:** the release also has `Halo2-Recomp-v<version>-Windows-x64.zip` for
  [RecompLauncher](https://recomplauncher.com/sdk) (level 3). It asks for the disc and
  the title update folder, imports them with `halo2_import.exe --prepare`, and uses the
  same `data\settings.ini`. Its Big Picture mode always starts the game fullscreen.

## Settings

`config.exe` beside the game (Start menu: Halo 2 Recomp Settings) and F1 in the game edit
the same `data\settings.ini` and `data\keymap.ini`:

- **VIDEO:** render scale, fullscreen, VSync, anisotropic filtering, FXAA, MSAA,
  widescreen.
- **CONTROLS:** every action with up to three keys or mouse buttons, plus mouse aim,
  sensitivity and invert (below).
- The F1 menu works with the keyboard, a controller or the mouse: click a tab, a row or its
  arrows, the wheel moves between rows, right click steps a setting back or clears a
  binding.
- F11 or Alt+Enter switches fullscreen in the game and keeps the choice. Esc asks to quit.
- **Updates:** the F1 menu and `config.exe` check this repository's latest release (at most
  once every two hours) and show an UPDATE banner when there is a newer one. Choosing it
  runs `Halo2 Updater.exe`, which downloads the release ZIP, copies it over the install and
  starts the game again. `data\` and `game_files\` are never touched, so saves, settings,
  the key map and the console identity stay.

## Reporting a problem

Click **Report a problem** in `config.exe`. It opens:

- the logs folder (`data\logs`), with the newest log selected;
- a new GitHub issue filled in with the game version, Windows, CPU, GPU and memory.

Describe what happened and drag the log onto the issue (GitHub needs a free account).

The game writes one log per run, keeping the last three and stopping routine lines at
20 MB. The log contains:

- a `[BUILD]` line (version, symbol id, Windows, CPU, GPU, memory);
- `[VIDEO]` lines when it starts and when a setting changes;
- a `[FREEZE]` report with every thread's call stack when no frame comes for 10 seconds;
- a `[CRASH]` report for a fatal error.

Your Windows user name is blanked out. The `.pdb` files stay with the developer:
`scripts/symbolize_log.py` turns the offsets in a report into function names.

## Layout

| Folder | What it is |
| --- | --- |
| `halo2-native-tu15/` | The game: the 1.5 (title update 5) build. Runtime, D3D11 GPU backend, settings and overlay, installer and lift scripts |
| `xboxrecomp-toolkit/` | Vendored toolkit: kernel, NV2A, APU, USB, input and network (NVNET, System Link, the Xbox Live router) runtime, plus the Python lifter (`tools/`) |

## Building (Windows)

Requirements: Visual Studio 2022 with the C++ x64 tools, CMake, Ninja and Python 3.

1. **Game files.** Point the installer at your retail Halo 2 disc (an .iso/.xiso image, a
   drive, or an extracted folder). It checks the retail executable, copies the disc's game
   data, and patches it to title update 1.5 automatically, verifying every result by SHA-256:
   `python halo2-native-tu15\installer\tu15_install.py --disc "<path to Halo 2 disc>" --dest halo2-native-tu15\game`
2. **Analyze and lift `default.xbe`.** Copy `halo2-native-tu15\game\default.xbe` to
   `halo2-native-tu15\analysis\default.xbe`, then run, in PowerShell:
   ```
   halo2-native-tu15\analysis\run-pipeline.ps1
   halo2-native-tu15\analysis\run-lift.ps1
   python halo2-native-tu15\analysis\apply-gen-patches.py
   ```
   This writes the generated C to `halo2-native-tu15\src\recomp\gen\` and reapplies the
   hand edits to it.
3. **Lift the content downloader** (`downloader.xbe`, the disc's Xbox Live content
   installer). Copy `halo2-native-tu15\game\downloader.xbe` to
   `halo2-native-tu15\analysis-downloader\downloader.xbe`, then run:
   ```
   halo2-native-tu15\analysis-downloader\run-pipeline.ps1
   python halo2-native-tu15\analysis-downloader\apply-gen-patches.py
   ```
   This writes `halo2-native-tu15\src\downloader\gen\`.
4. **Build.** Run `halo2-native-tu15\build.cmd` (or
   `cmake -S halo2-native-tu15 -B halo2-native-tu15\build-ninja -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DXBOXRECOMP_BUILD_TESTS=OFF`
   then `cmake --build halo2-native-tu15\build-ninja --target halo2_recomp halo2_downloader halo2_config halo2_insignia_setup`).
5. **Run** one of the launchers in `halo2-native-tu15\`. F1 in game (or
   `build-ninja\Config.exe`) opens the settings.

| Launcher | What it is |
| --- | --- |
| `Launch-Halo2.cmd` | Offline: campaign and split screen |
| `Launch-Halo2-SystemLink.cmd` | System Link over the LAN, and over Tailscale when it is connected |
| `Launch-Halo2-Insignia.cmd` | Xbox Live through [Insignia](https://insignia.live), with your own console identity (below) |

The game runs in one window from start to exit, including the hand-over to the content
downloader and back. All saves and settings stay in the install folder: `SaveData\`,
with the settings in `SaveData\settings.ini`. Every launcher loads the console identity
when one has been set up, so the game's disk cache matches whichever launcher is used.
The offline launcher has no network; the System Link launcher's internet router is off.

### Release (Setup and RecompLauncher ZIP)

`python halo2-native-tu15\scripts\build_installer.py --version X.Y.Z` builds the player release in
`build-release\`:

- **Build settings:** the C runtime linked in, a `.pdb` for every program, the Windows
  subsystem, and no developer switches.
- **Version:** it sets the version resources (`res\*.rc`) and `src\version.h`.
- **Symbols:** it keeps each program and its `.pdb` in `out\symbols\X.Y.Z` (never shipped).
- **Setup:** it draws the Setup's pictures from `res\halo2.png` (`scripts\make_icon.py`
  makes the icon) and compiles `installer\halo2.iss` with Inno Setup 6.6 or newer (found by
  itself, or `--iscc`).
- **ZIP:** it writes the RecompLauncher ZIP (`scripts\recomplauncher_manifest.py`).

Everything lands in `installer\Output` with `.sha256` files. `installer\manifest.h`
(sizes and hashes only) comes from `installer\gen_manifest.py <game folder> <disc image>`.
Both outputs contain code translated from the game, so they are never committed.

## Mouse and keyboard

Turn on **Mouse aim** in the F1 menu, or in `config.exe` (CONTROLS).

- **Aim:** the mouse turns the player directly, with no stick acceleration. The mouse is
  added to the arguments of the game's own turn function, so the pitch and vehicle-seat
  limits still apply.
  - Sensitivity and invert are in the same menus. F9 frees the pointer, which is hidden
    while you aim.
  - Relative and absolute mouse input both work (absolute: remote desktop, KVMs,
    tablets).
- **Menus:** outside play (intros, menus, cutscenes, pause), left click is Start and
  right click is B.
- **In play:** the mouse buttons and the wheel are bindings in the key map. Rebind them
  in F1 (applied at once) or in Config.exe.
- **Keys:** `keymap.ini` has Halo 2 for Windows' layout. The player's copy, with any changes,
  is `data\keymap.ini` (`SaveData\keymap.ini` in a development tree).
- **Controller only on Xbox Live:** sign-in is never blocked. The mouse turns itself off
  (aim, buttons, menu clicks, the captured pointer) when the player picks XBOX LIVE in the
  main menu, and back on with Campaign, Split Screen or System Link.
  - These are entry hooks on the four main-menu choices, matched to the retail functions
    the Halo 2 decompilation documents.
  - An Xbox Live match's traffic also turns the mouse off, for games joined from an
    invite.

## Widescreen

True widescreen, 16:9 by default. Change it in F1 or Config.exe: Off (4:3), Auto (the
monitor's shape), 16:9 or 16:10. It takes effect at the next start.

- **3D view:** wider (Hor+), with more at the sides and the same vertical view. The
  projection's aspect is multiplied in the game's own camera code (a hand patch on the
  generated C), and the game itself stays in its 4:3 mode, so nothing is stretched.
- **HUD, menus and text:** kept at 4:3 in the middle of the screen. A geometry shader
  squeezes flat 2D triangles drawn into the 640x480 screen with depth testing off;
  full-width ones (fades, full-screen effects) are left alone.
- Halo 2's own Xbox widescreen mode is not used: it is anamorphic and stretches the HUD.
- **Xbox Live is played at 4:3, stretched across the screen (anamorphic),** so no one sees
  more at the sides than players on a 4:3 TV. It switches when the player picks XBOX LIVE
  in the main menu, and back to full widescreen with Campaign, Split Screen or System Link.
  An Xbox Live match's traffic also switches it, for games joined from an invite (not in
  the first minute after sign-in, whose traffic to Insignia looks like a match).
- `RECOMP_VIDEO_WIDESCREEN=0` forces 4:3 everywhere.

## Xbox Live (Insignia)

Xbox Live needs the identity of a console you own. Run Insignia Setup (Start menu:
Halo 2 Recomp Insignia Setup; built from `src/insignia_setup.c`).

1. Choose your console's EEPROM.
2. Choose your console's BIOS image and its MCPX boot ROM. This proves the EEPROM is a
   real console's: only that console's BIOS opens its sealed section. It also makes
   Halo 2's per-title keys.
3. Give the hard-drive key, typed or from `hddinfo.txt`, or let the BIOS fill it in. It
   must be the key sealed in that EEPROM.
4. Save, then recover your account from Insignia (gamertag plus the 16-digit number from
   insignia.live), or load the accounts from a dump of your console's drive (raw image or
   xemu `.qcow2`).

Setup writes `eeprom.bin`, `hddinfo.txt`, `eepromkey.bin` and `titlekeys.bin` to
`data\Identity\` (`SaveData\Identity\` in a development tree). Save and Recover stay locked until steps 1–3 are verified, and
Setup itself has no network code. Nothing is built in, and no key is ever logged. Only
one copy of a console identity may be online at a time: keep the console, xemu and other
copies off.

- **Verified identity only:** the game checks the identity again at start against the
  BIOS key that Setup saved. Without a verified identity its router lets nothing reach
  the internet: no DNS lookups and no packets to Insignia. System Link still works.
- **Account recovery:** it runs through the game's own Xbox Live library. The recovery
  number goes to the game over a pipe and is never written anywhere.
- **Downloadable content:** the in-game content downloader installs the map packs from
  Insignia.
- **30 fps in matches only:** Xbox Live matches run at 30 fps, one frame per simulation
  tick, because 60 fps jitters against the 30 Hz network game. The game is back at 60 fps
  10 seconds after a match ends. Being signed in alone, System Link and the campaign stay
  at 60. `RECOMP_HALO2_LIVE_60=1` keeps 60 fps in matches too.

## Hi-res textures

Put DDS replacements in `halo2-native-tu15\textures\`. Name each one
`<hash16>_<W>x<H>_<anything>.dds`, keeping the extracted texture's name. A file can be
any power-of-two multiple of the original size (2x, 4x and so on), in BC7, BC1–BC5 or
RGBA, with mipmaps. Cube maps are a single DDS cubemap.

- **Extract the originals:** `analysis\textures\tools\extract_textures.py` exports every
  texture in your maps as PNG, already named.
- **Distant objects:** `make_mip_aliases.py` writes `mip_aliases.txt`. Keep it in the pack
  folder so the replacement also shows on distant objects, which the engine draws from a
  smaller mip.
- **Build the pack:** `build_pack.ps1` converts upscaled PNGs to BC7 (needs DirectXTex
  `texconv`).
- **Switches:** `RECOMP_TEX_PACK=<dir>` uses another folder, `RECOMP_TEX_PACK=0` turns the
  pack off, and `RECOMP_TEX_PACK_LOG=1` logs each texture as it loads.

## Status

Campaign boots and plays at 60 FPS (Halo 2's own 30 FPS cadence is lifted; heavy open
scenes can still drop below 60). Resolution scale, MSAA up to 8x, anisotropic filtering
and FXAA are available. Third-party pads work through SDL3.

Recent rendering fixes, each on by default with a switch to turn it off:

| Fix | Off switch |
| --- | --- |
| Cinematic depth-of-field split (missing hangar marines, see-through drone) | `RECOMP_GPU_ZCLIP=0` |
| Hard, flickering edges on light and decal passes | `RECOMP_GPU_DECAL_BIAS=0` |
| W-buffer depth stored as on the hardware (patchy fog, depth-correct floors) | `RECOMP_GPU_WBUFFER=0` |
| Depth buffer read as a colour texture (patchy fog), only while that memory is still the depth buffer (the loading screen reuses it) | `RECOMP_GPU_DEPTH_ARGB=0` |
| Perspective-correct colour and fog interpolation | `RECOMP_GPU_AFFINE_COLOR=1` |
| 60 FPS frame target | `RECOMP_HALO2_SINGLE_VBLANK=0` |

Known issue: the HUD is missing from DEL debug screenshots (development builds) when MSAA
is on.

## Licenses

Halo 2 is (c) Microsoft/Bungie. This repository contains no game code or assets. The
release programs are built from code translated from the game's executable; they contain
no game assets, and the game files come from your own disc.
The xboxrecomp toolkit is MIT (`xboxrecomp-toolkit/LICENSE`, with `LICENSES/` and `NOTICE`
for bundled components). SDL3 is zlib (`halo2-native-tu15/third_party/SDL3-3.4.20/LICENSE.txt`).
The GPU backend's register-combiner shader generator is adapted from MIT-licensed code
(`src/gpu/NOTICE.txt`).
