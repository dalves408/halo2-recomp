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
   then `cmake --build halo2-native-tu15\build-ninja --target halo2_recomp halo2_downloader halo2_config`).
5. **Run** one of the launchers in `halo2-native-tu15\`. F1 in game (or
   `build-ninja\Config.exe`) opens the settings.

| Launcher | What it is |
| --- | --- |
| `Launch-Halo2.cmd` | Offline: campaign and split screen |
| `Launch-Halo2-SystemLink.cmd` | System Link over the LAN, and over Tailscale when it is connected |
| `Launch-Halo2-Insignia.cmd` | Xbox Live through [Insignia](https://insignia.live), with your own console identity (below) |

The game runs in one window from start to exit, including the hand-over to the content
downloader and back. All saves and settings stay in the install folder (`SaveData\`).

## Xbox Live (Insignia)

Xbox Live needs the identity of a console you own. Put these in
`halo2-native-tu15\SaveData\Identity\`:

- `eeprom.bin`, your console's EEPROM.
- `hddinfo.txt`, holding a line with your console's `XboxHDKey`.
- `titlekeys.bin`, Halo 2's per-title keys derived from your own BIOS, MCPX ROM and
  EEPROM:
  `python halo2-native-tu15\installer\derive_title_keys.py --bios ... --mcpx ... --eeprom ... --xbe halo2-native-tu15\game\default.xbe --out halo2-native-tu15\SaveData\Identity\titlekeys.bin`

Nothing is built in, and no key is ever logged. Only one copy of a console identity may
be online at a time: keep the console, xemu and other copies off.

- **Existing account:** `Recover-Insignia-Account.ps1` recovers it into the install,
  through the game's own Xbox Live library, with the recovery number from insignia.live.
  The number goes to the game over a pipe and is never written anywhere.
- **Downloadable content:** the in-game content downloader installs the map packs from
  Insignia.
- **No EEPROM, no contact:** without a console identity the game never contacts
  Insignia. That means no DNS lookups and no packets to the internet. System Link still
  works.
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
