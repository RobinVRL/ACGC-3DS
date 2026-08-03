# ACGC-3DS

ACGC-3DS is an experimental native Nintendo 3DS port of *Animal Crossing*
for the GameCube. It is based on
[ACreTeam's `ac-decomp`](https://github.com/ACreTeam/ac-decomp) and the
[ACGC PC port](https://github.com/flyngmt/ACGC-PC-Port).

The decompiled game code is built for ARM and connected to native 3DS services:
Citro3D/libctru for graphics and system integration, `hid` for input, `ndsp`
for audio, and the SD card for disc-image access and saves.

> [!IMPORTANT]
> This port is a work in progress. The complete selected runtime builds and
> launches, but rendering and gameplay are not yet fully correct. Expect
> missing or incorrect surfaces, incomplete GameCube GX behavior, and other
> compatibility issues.

This repository does not include Nintendo game data. You must supply your own
legally obtained disc image. The currently supported revision is
`GAFE01_00` (USA, Rev 0) in ISO, GCM, or CISO format.

## Current Features

- Native Nintendo 3DS executable; no emulator is bundled
- Full selected decompiled runtime linked into the 3DS application
- ISO, GCM, and CISO disc-image reading from the SD card
- Citro3D-backed translation for the implemented GameCube GX paths
- Native Circle Pad, C-Stick, button, and trigger input
- Touch control deck for Map, Pockets, Options, and camera look; the original
  game UI remains centered and undistorted on the top screen
- `ndsp` audio output
- GCI-compatible save directories on the SD card
- Dolphin-style DDS replacement textures, including uncompressed RGBA/BGRA,
  BC1/DXT1, and BC3/DXT5
- Homebrew Launcher (`.3dsx`) and extended-memory CIA build targets

## Requirements

- [devkitPro](https://devkitpro.org/) with the 3DS development packages
- CMake (provided by the devkitPro environment)
- A legally obtained `GAFE01_00` Animal Crossing disc image

Install the required devkitPro packages from its MSYS2 shell:

```sh
pacman -S 3ds-dev citro3d citro2d
```

Creating an installable CIA also requires `makerom` v0.18 from Project_CTR
and `bannertool` v1.2.0 to be available on `PATH`.

## Building

Clone the repository and build from `ports/3ds` inside the devkitPro shell:

```sh
git clone https://github.com/RobinVRL/ACGC-3DS.git
cd ACGC-3DS/ports/3ds
make
```

The normal build produces `acgc_3ds.3dsx`. To compile only the native
game-entry and frame-loop boundary as a quick toolchain check, run:

```sh
make runtime-smoke
```

To build the extended-memory CIA:

```sh
make cia
```

This produces `acgc_3ds_extended.cia` using the Old 3DS Dev2 80 MiB memory
layout. Install it with a compatible title manager such as FBI. Entering or
leaving this memory mode may reboot an Old 3DS; that behavior is expected.

## SD Card Layout

Copy `acgc_3ds.3dsx` to the location used by your Homebrew Launcher. The port
creates and uses the following directories:

```text
sdmc:/3ds/acgc/
|-- rom/       # Place one supported ISO, GCM, or CISO image here
|-- save/      # GCI-compatible card_a and card_b save directories
|-- textures/  # Optional Dolphin-format DDS texture replacements
`-- dump/      # Optional DOL/REL diagnostic dumps
```

Texture subdirectories are scanned recursively. Disc images, extracted game
data, save files, and locally built executables must not be committed to this
repository.

## Launching

1. Put your supported disc image in `sdmc:/3ds/acgc/rom/`.
2. Launch `acgc_3ds.3dsx`, or start the installed extended-memory CIA.
3. Wait for the bottom-screen probe to display `Disc reader: OK`.
4. Press X to load the disc assets and enter the game runtime.

Controls on the startup probe:

| Button | Action |
|---|---|
| X | Load the detected disc image and start the runtime |
| Y | Dump the extracted DOL and REL to `sdmc:/3ds/acgc/dump/` |
| Select | Rescan the ROM directory |
| Start | Exit |

## In-Game Controls

| Nintendo 3DS input | GameCube input |
|---|---|
| Circle Pad | Control Stick |
| C-Stick | C-Stick |
| A / B / X / Y | A / B / X / Y |
| L / R | L / R |
| ZL | Z |
| D-Pad | D-Pad |
| Start | Start |

The C-Stick and ZL mappings require hardware that provides those inputs, such
as a New Nintendo 3DS.

## Project Status and Limitations

The port currently compiles the selected native runtime and can enter its frame
loop. The GX compatibility layer handles core matrix, viewport, vertex,
texture, depth, culling, and several TEV paths, but it is not a complete
GameCube GPU implementation. Fog, EFB behavior, display lists, uncommon
primitives, and complex TEV combinations may be reduced, approximated, or
unsupported. The bundled NES emulator is disabled.

Memory and GPU feature limits remain the main constraints, especially on an
Old Nintendo 3DS. Testing in an emulator does not replace testing on hardware.
See [`ports/3ds/README.md`](ports/3ds/README.md) for lower-level implementation
notes and milestones.

## Repository Layout

- `ports/3ds/` - 3DS build system, native platform services, GX translator,
  icons, banner assets, and port documentation
- `src/` and `include/` - decompiled game code plus 3DS compatibility changes
- `pc/` - shared platform-neutral compatibility code inherited from the PC port
- `config/`, `assets/`, and `tools/` - upstream decompilation configuration and
  development tooling

## Credits and License

This project builds on the work of the
[ACreTeam](https://github.com/ACreTeam) decompilation contributors and the
[ACGC-PC-Port](https://github.com/flyngmt/ACGC-PC-Port) contributors. It also
uses devkitPro, libctru, and Citro3D for Nintendo 3DS development.

See [`LICENSE`](LICENSE) for the licenses and attribution that apply to the
decompilation and port code.
