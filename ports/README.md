# Animal Crossing Console Ports

This directory is the starting point for native homebrew ports derived from the
existing PC port runtime layer.

The PC port is currently the practical base for console work because it already
replaces the original GameCube SDK calls with host-side implementations for
graphics, audio, input, save data, and disc image reads. The upstream
`ACreTeam/ac-decomp` tree is valuable reference material, but it is primarily a
matching decompilation build and still targets the GameCube binary layout.

## Layout

- `3ds/` - Nintendo 3DS homebrew target using devkitARM/libctru.
- `switch/` - Nintendo Switch homebrew target using devkitA64/libnx.

## Major Shared Work

1. Split the current `pc/src/pc_*.c` runtime layer into reusable interfaces and
   per-platform backends. The current code mixes SDL2, OpenGL, desktop files,
   and platform-neutral replacements in the same files.
2. Replace OpenGL 3.3 rendering with platform renderers:
   - 3DS: citro3d/citro2d-friendly renderer, with strong VRAM budgeting.
   - Switch: deko3d or EGL/OpenGL ES path, depending on the chosen homebrew
     stack.
3. Replace SDL input/audio/window handling with native platform services.
4. Move ROM/disc-image and save paths to SD-card-friendly locations.
5. Audit 32-bit pointer assumptions before the Switch port can execute game
   code. The PC build currently refuses 64-bit builds because JSystem stores
   pointers in `u32`; this is naturally compatible with 3DS, but not Switch.
6. Reduce memory pressure for 3DS. The generated runtime asset table and
   GameCube-era scene data may exceed comfortable limits unless assets are
   streamed aggressively.

## Legal/Asset Note

No game assets should be committed here. Both ports should keep the PC port's
model of reading user-provided disc data from the SD card.

