# Nintendo 3DS Port

This folder contains the native Nintendo 3DS homebrew port.

## Toolchain

Install devkitPro with the 3DS packages:

```sh
pacman -S 3ds-dev citro3d citro2d
```

Build from this directory with:

```sh
make
```

For an installable Old 3DS extended-memory build, install `makerom` v0.18
from Project_CTR and `bannertool` v1.2.0, then run:

```sh
make cia
```

This creates `acgc_3ds_extended.cia` with the 80 MiB (Dev2) Old 3DS system
mode in its exheader. Install it with FBI. Launching or exiting this build may
reboot an Old 3DS as the system switches memory layouts; this is expected.
The native runtime uses an 8 MiB MEM1 arena because most DOL/REL data is already
compiled into the process image. It reserves 12 MiB of linear memory for the
renderer and audio, leaving the ordinary heap for MEM1, the 16 MiB ARAM
emulation, and decoded assets. Use the extended-memory CIA on Old 3DS hardware.

Compile the native game-entry and frame-loop boundary with:

```sh
make runtime-smoke
```

`runtime/CMakeLists.txt` builds the full selected decompiled source set for ARM.
The normal `make` target bundles those objects, the native service replacements,
and the Citro3D GX translator into `acgc_3ds.3dsx`.

## Launching

1. Put the disc image under `sdmc:/3ds/acgc/rom/`.
   Dolphin-format DDS texture packs may be placed under
   `sdmc:/3ds/acgc/textures/`; subdirectories are scanned recursively.
2. Start `acgc_3ds.3dsx`.
3. Wait for the bottom-screen probe to report `Disc reader: OK`.
4. Press X to load the ROM assets and enter the decompiled game runtime.

SELECT rescans the ROM folder, Y dumps the extracted DOL/REL, and START exits
while the probe is active.

## Intended Runtime Mapping

- Video: replace `pc_gx*.c` OpenGL calls with citro3d/citro2d-backed GX-style
  translation.
- Input: replace SDL gamepad/keyboard polling with `hidScanInput`.
- Audio: replace SDL audio with ndsp.
- Files: load the user's disc image from `sdmc:/3ds/acgc/rom/`.
- Saves: write GCI-compatible saves under `sdmc:/3ds/acgc/save/`.
- Textures: load Dolphin `tex1_...` DDS replacements from
  `sdmc:/3ds/acgc/textures/` (uncompressed RGBA/BGRA, BC1/DXT1, and BC3/DXT5).

## First Milestones

1. Compile a tiny 3DS executable that initializes services and exits cleanly.
   Done.
2. Bring up ROM file reads from SD.
   Initial ISO/GCM/CISO reader is wired to `sdmc:/3ds/acgc/rom/`.
3. Load the DOL and decoded REL from the ROM.
   Done as a runtime probe, including optional dumps to `sdmc:/3ds/acgc/dump/`.
4. Parse DOL/REL section maps and prove ACPC-style asset byte ranges can be
   copied and byte-swapped from those buffers.
   Initial probe is done for representative REL, DOL, and vertex samples.
5. Replace PC SDL/OpenGL shell systems with native 3DS services.
   Initial native modules are in place:
   - `acgc_3ds_input.c`: maps 3DS controls to a GameCube-style pad status.
   - `acgc_3ds_audio.c`: queues the game's 32 kHz stereo DMA blocks through
     persistent ndsp wave buffers.
   - `acgc_3ds_save.c`: creates SD save slot directories.
   - `acgc_3ds_video.c`: owns a Citro3D shader, dynamic vertex buffer, GPU
     state, and top-screen triangle submission path.
   - The original HUD, dialogs, inventory, and map are composited at 4:3 on
     the top screen. The bottom screen is a touch control deck with Map,
     Pockets, Options, and a virtual C-Stick look pad for older systems.
6. Move platform-neutral PC replacement code behind shared headers.
7. Compile non-rendering game/runtime units with `TARGET_3DS`.
8. Implement enough GX translation to show the boot/logo path.
   The runtime now translates projection/model matrices, immediate-mode vertex
   colors, triangles, strips, fans, and quads into Citro3D draws.

## Current Limitation

The complete native runtime now links and can be launched, but the renderer is
still an early compatibility implementation. Texture decoding/binding, TEV
combiner behavior, fog, EFB copies, lines/points, and display-list recording are
currently reduced or ignored. The bundled NES emulator is also disabled. Expect
incorrect or missing surfaces until those GX features are implemented and
validated in Azahar and on hardware.

The 3DS is a 32-bit platform, so it avoids the PC port's largest Switch blocker:
pointer-to-`u32` storage in JSystem. The harder 3DS problem is memory and GPU
feature fit.
