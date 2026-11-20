# Halo 2 static recompilation (Xbox to native Windows x64)

A static recompilation of the original Xbox Halo 2 into a native Windows x64 program,
built on the xboxrecomp toolkit. The x86 game code is lifted to C, and a reimplemented
Xbox kernel, NV2A GPU (Direct3D 11 backend), APU and USB stack run it on a PC.

**No game content is in this repository.** You need your own retail Halo 2 disc. The
recompiled C (`src/recomp/gen/`) is generated from your copy of `default.xbe` and is not
committed.

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

## Mouse and keyboard

Turn on **Mouse aim** in the F1 menu, or in `build-ninja\Config.exe`.

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
- **Keys:** `keymap.ini` has an FPS layout. The player's copy, with any changes, is
  `SaveData\keymap.ini`.
- **Controller only on Xbox Live:** sign-in is never blocked. The mouse turns itself off
  (aim, buttons, menu clicks, the captured pointer) when the player picks XBOX LIVE in the
  main menu, and back on with Campaign, Split Screen or System Link.
  - These are entry hooks on the four main-menu choices, matched to the retail functions
    the Halo 2 decompilation documents.
  - An Xbox Live match's traffic also turns the mouse off, for games joined from an
    invite.

## Xbox Live (Insignia)

Xbox Live needs the identity of a console you own. Run
`halo2-native-tu15\build-ninja\Insignia Setup.exe` (built from `src/insignia_setup.c`).

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
`SaveData\Identity\`. Save and Recover stay locked until steps 1–3 are verified, and
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

Known issues: a dark rectangle next to the shield meter, and the HUD missing from DEL
debug screenshots when MSAA is on.

## Licenses

Halo 2 is (c) Microsoft/Bungie. This project contains no game code or assets.
The xboxrecomp toolkit is MIT (`xboxrecomp-toolkit/LICENSE`, with `LICENSES/` and `NOTICE`
for bundled components). SDL3 is zlib (`halo2-native-tu15/third_party/SDL3-3.4.20/LICENSE.txt`).
The GPU backend's register-combiner shader generator is adapted from MIT-licensed code
(`src/gpu/NOTICE.txt`).
