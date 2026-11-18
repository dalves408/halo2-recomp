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
| `xboxrecomp-toolkit/` | Vendored toolkit: kernel, NV2A, APU, USB and input runtime, plus the Python lifter (`tools/`) |

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
3. **Build.** Run `halo2-native-tu15\build.cmd` (or
   `cmake -S halo2-native-tu15 -B halo2-native-tu15\build-ninja -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DXBOXRECOMP_BUILD_TESTS=OFF`
   then `cmake --build halo2-native-tu15\build-ninja --target halo2_recomp halo2_config`).
4. **Run** `halo2-native-tu15\Launch-Halo2.cmd`. F1 in game (or `build-ninja\Config.exe`)
   opens the settings.

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
| Depth buffer read as a colour texture (patchy fog) | `RECOMP_GPU_DEPTH_ARGB=0` |
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
