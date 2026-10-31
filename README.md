# Harvest Moon: Back to Nature — Native PC Port

An unofficial, native **Windows and Linux** version of *Harvest Moon: Back to Nature*
(PlayStation, USA, SLUS-01115), made by **static recompilation**: the game's original
PlayStation program is translated ahead of time into C and compiled into a normal x86-64
executable. No emulator runs the game, and no PlayStation BIOS is needed.

This is a fork of [GabrielBigardi/HarvestMoonBTN-Recomp](https://github.com/GabrielBigardi/HarvestMoonBTN-Recomp)
that adds optional graphics enhancements: 2–4× internal resolution, PGXP, perspective-correct
textures, smoothly placed and depth-scaled 2D objects, sharp-bilinear scaling and a settings
launcher. See [docs/GRAPHICS.md](docs/GRAPHICS.md).

> **No game files are included in this repository.** This is a "bring your own copy"
> project: the build reads the game from **your own disc image** and creates the native
> version on your computer. See [Why you need your own copy](#why-you-need-your-own-copy).

- Plays like the original: same game logic, same graphics, music and timing (NTSC, 59.29 fps)
- Keyboard and game controller support (Xbox / XInput and other SDL-supported controllers), fully remappable
- Window or fullscreen, scaled from the original 320×240 (sharp-bilinear, sharp or smooth, 4:3 or square pixels)
- Optional higher internal resolution (2×–4×), precise 3D vertices (PGXP), perspective-correct textures,
  2D objects that scale with depth, a widescreen view (16:9, 16:10, 21:9), and in-between pictures that move
  the camera and the 3D scenery at 60 fps
- Saving works as on the PlayStation, on a standard memory card file usable in emulators too
- Optional hold-to-fast-forward, screenshots

## Contents

- [Why you need your own copy](#why-you-need-your-own-copy)
- [Bring your own copy](#bring-your-own-copy)
- [Building](#building)
- [Playing](#playing)
- [Controls](#controls)
- [Configuration](#configuration)
- [Saves](#saves)
- [How it was ported](#how-it-was-ported)
- [Project layout](#project-layout)
- [Troubleshooting](#troubleshooting)
- [Status](#status)
- [License and legal](#license-and-legal)

## Why you need your own copy

*Harvest Moon: Back to Nature* is a commercial game whose program code, graphics, music
and text are copyrighted by their owners. This repository contains **only original work**:
the recompiler, a native reimplementation of the PlayStation hardware and BIOS, and the
build scripts. Nothing from the game is in it.

Everything that comes from the game is created locally, from your disc image, when you
build, and is excluded from git by [`.gitignore`](.gitignore):

| Created by the build | Contains | In the repository? |
|---|---|---|
| `extract/` | the game's executable and code files, read from your disc | no |
| `generated/` | the game's program, translated to C | no |
| `build/`, `dist/` | the compiled port: **the executable contains the game's translated code** | no |
| `HarvestMoonBTN.pak` (optional) | the game's data (graphics, music, text), repacked from your disc | no |

This is also why there are **no ready-made downloads**: a pre-built executable would
contain the game's program code (translated, but still the game's), and the data pack would
contain the game's data. Instead, one command turns your copy into the native version on
your own machine, usually in under a minute. Please don't share your build output or your
data pack.

## Bring your own copy

You need a disc image of **Harvest Moon: Back to Nature, USA version (SLUS-01115)** in
`.cue` + `.bin` format (raw Mode2/2352) made from a disc you own. Any dumping tool that
creates a raw BIN/CUE image works: for example ImgBurn or Redump's
[MPF](https://github.com/SabreTools/MPF) on Windows, or `cdrdao` on Linux. Images in
other formats (CHD, PBP, ISO with 2048-byte sectors) need to be converted back to
`.bin`/`.cue` first.

Only the USA release is supported, because the recompiled code is specific to that exact
program. `build.py` checks the files it reads from your disc against these SHA-256 hashes
and stops with a clear message if they don't match:

| File on the disc | SHA-256 |
|---|---|
| `SLUS_011.15` | `34e3b35dcd3fb1cd9a7d4813b8a9ef987915826e3d436f1908ecbe2d4fdf1444` |
| `A_FILE.HDT` | `0cae44d6bdb29e078a182d7cd9a780c21e35dc54f55eb94c2d9ea8deb20c82dd` |
| `A_FILE.BIN` | `612f9ac8279a364c2fdd19cb75e63df2e892a6f52b6f1d8081ea8560b18decd4` |

Put the `.cue` and `.bin` into the [`disc/`](disc/) folder of this repository (any file
names; git ignores everything in it), or pass the path with `--disc`.

## Building

### Windows, without Python: the launcher

`dist-template/HarvestMoonBTN-Launcher.exe` carries the build scripts and the runtime sources
(nothing of the game). Put it in an empty folder and open it: its **Build from my disc** page
asks for your disc image and builds the game into that folder. The first time it downloads
only the parts of the compiler the build uses (about 65 MB, each file checked against
`tools/toolchain.txt`; `python tools/fetch_deps.py --min` does the same). Your disc's files and
the build stay in `build-files/` there. To give the game to a friend, give them the launcher;
they build from their own disc. After changing the launcher, the runtime or the tools, rebuild
the exe with `python tools/build_launcher.py` (needs `pip install pyinstaller`), and after
changing an `#include` or the toolchain, rewrite the file list with `python tools/min_toolchain.py`.

**Updates.** The launcher checks GitHub for a newer release when it opens. **Update** replaces
the exe with the new one (checked against the SHA-256 GitHub gives for it) and rebuilds the game
with the new sources, from the files it took from your disc earlier, so the disc image isn't
needed again. Your saves, settings and textures stay. Both co-op players get the same version
this way. To publish one: raise `VERSION` in `dist-template/launcher.pyw`, run
`python tools/build_launcher.py`, commit, merge into main, then `python tools/release.py "what changed"`.

### Windows

Requirements: [Python 3.8+](https://www.python.org/downloads/) and about 1 GB of disk
space for the compiler. (On Windows the command may be `py` instead of `python`.)

```bat
git clone https://github.com/GabrielBigardi/HarvestMoonBTN-Recomp.git
cd HarvestMoonBTN-Recomp
python tools\fetch_deps.py
python build.py
```

1. `tools/fetch_deps.py` downloads the toolchain into `deps/`:
   [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (clang) and the
   [SDL2](https://libsdl.org) development files. Each download is checked against a pinned
   SHA-256. Nothing is installed system-wide.
2. `build.py` reads your disc image from `disc/`, recompiles the game code, compiles it and
   puts a ready-to-play folder in **`dist/HarvestMoonBTN-Windows-x64/`**.

### Linux

Requirements: Python 3.8+, a C compiler and the SDL2 development package.

```sh
sudo apt install build-essential python3 libsdl2-dev      # Debian / Ubuntu
sudo dnf install gcc python3 SDL2-devel                   # Fedora
sudo pacman -S base-devel python sdl2                     # Arch

git clone https://github.com/GabrielBigardi/HarvestMoonBTN-Recomp.git
cd HarvestMoonBTN-Recomp
python3 build.py
```

The result is in **`dist/HarvestMoonBTN-Linux-x64/`**. To build the Linux version on
Windows instead (cross-compiling with [Zig](https://ziglang.org)), run
`python tools\fetch_deps.py --linux` once and then `python build.py --linux`.

### Build options

```
python build.py [--disc PATH] [--pak] [--zip] [--into DIR] [--linux] [--debug] [--jobs N] [--clean]
```

| Option | |
|---|---|
| `--disc PATH` | your disc image (`.cue` or `.bin`); default: the first one in `disc/` |
| `--pak` | also create the data pack (see [Playing](#playing)) so the game runs without the disc image |
| `--zip` | also create a `.zip` of the dist folder |
| `--into DIR` | put the game into `DIR` (a folder you play from) instead of `dist/`; nothing there is deleted |
| `--linux` | on Windows: cross-build the Linux version |
| `--debug` | unoptimised build with a console window, for development |
| `--jobs N` | number of parallel compiler processes (default: all CPU cores) |
| `--clean` | rebuild everything from scratch |

A full build takes under a minute on a modern PC; later builds only redo the steps whose
inputs changed.

## Playing

Run `HarvestMoonBTN.exe` (Windows) or `./HarvestMoonBTN` (Linux) in the dist folder. The
game needs its data (graphics, music, text), which comes from your own copy, in one of two
ways:

- **Disc image (default).** The game reads your `.cue`/`.bin`. `hmbtn.ini` remembers the
  image the folder was built from; if you move it, put the `.cue` and `.bin` into the
  `disc` folder next to the executable, set `disc =` in `hmbtn.ini`, or pass the path on
  the command line.
- **Data pack (`python build.py --pak`).** Your disc image is repacked into
  `HarvestMoonBTN.pak` (58 MB instead of 214 MB) next to the executable, and the disc image
  is no longer needed. The pack keeps the disc's exact sector layout (the game streams its
  music straight from the disc, interleaved with other data), and every block is checked
  against a checksum of your original disc on load, so the game sees exactly the same data.
  `HarvestMoonBTN --verify-data` checks the whole file.

## Controls

|  | Keyboard | Xbox controller |
|---|---|---|
| D-pad | Arrow keys | D-pad / left stick |
| ✕ Cross | X / Space / K | A |
| ○ Circle | Z / L | B |
| □ Square | S / J | X |
| △ Triangle | A / I | Y |
| L1 / R1 | Q / W | LB / RB |
| L2 / R2 | 1 / 2 | LT / RT |
| Start | Enter | Start (Menu) |
| Select | Right Shift / Backspace | Back (View) |

| Key | |
|---|---|
| F11 or Alt+Enter | toggle fullscreen (Esc leaves fullscreen) |
| F12 | screenshot (saved in `screenshots/`) |
| F9 | in-between pictures on / off (with `interp = 1`), to compare |
| F1–F4 | put memory card 1–4 in slot 1 (3 diaries each; shown in the window title) |
| L1 / R1 on a diary list | previous / next memory card (shown above the list); when loading, only cards that have a diary |
| Triangle (hold 2 s) on a diary list | delete the diary under the cursor |
| Tab (hold) | fast-forward (can be disabled) |
| START (hold, during a cutscene) | fast-forward the cutscene (`skipscenes`) |

Controllers can be plugged in or out at any time. Any controller that SDL2 recognises as a
game controller works (Xbox, PlayStation, Switch Pro and many others) with the Xbox-style
layout above. All keys and buttons can be changed in `hmbtn.ini`.

## Configuration

`hmbtn.ini` next to the executable is a commented settings file; remove the `#` in front of
an option to change it. Or double-click `launcher.pyw` in the same folder (needs Python): it
edits the same settings and starts the game. How the graphics options work, and what was
learned about the game while making them: [docs/GRAPHICS.md](docs/GRAPHICS.md).

| Section | Setting | Default | |
|---|---|---|---|
| `[general]` | `disc` | (automatic) | path to your `.cue`/`.bin` (or a `.pak`) |
|  | `savedir` | `saves` | folder for the memory card file |
| `[video]` | `scale` | `3` | initial window size as a multiple of 320×240 |
|  | `fullscreen` | `0` | start in fullscreen |
|  | `scaling` | `sharp-bilinear` | `sharp-bilinear` = sharp pixels that stay even at any size, `sharp` = nearest pixel, `smooth` = bilinear |
|  | `upscale` | `1` | internal resolution 1–4 (2–4 = smoother 3D edges, more CPU) |
|  | `pgxp` | `0` | `1` = precise 3D vertex positions, no wobble; 2D objects get smaller further away, like the 3D ground (needs `upscale` 2+) |
|  | `crackfix` | `1` | `1` = close the thin cracks between models that `pgxp` opens; `0` is about 1 ms a frame faster at upscale 4 (needs `pgxp`) |
|  | `perspective` | `0` | `1` = perspective-correct textures (needs `pgxp`) |
|  | `widescreen` | `off` | `16:9`, `16:10` or `21:9` = wider 3D view; 2D objects and the HUD keep their shape with `upscale` 2+ |
|  | `interp` | `0` | `1` = in-between pictures: the camera and the 3D scenery move at 60 fps while the game runs at 30 (needs `pgxp`; 3 extra CPU threads) |
|  | `texturepack` | `0` | `1` = draw 2D sprites (people, animals, field objects, text, HUD) with the replacement images in `textures/` next to the game ([texture packs](docs/GRAPHICS.md#texture-packs); needs `upscale` 2+) |
|  | `dumptextures` | `0` | `1` = write each 2D sprite texture the game shows to `textures/dump/`, to draw a texture pack from (needs `upscale` 2+) |
|  | `aspect43` | `1` | `1` = 4:3 picture like a TV, `0` = square pixels |
| `[behaviour]` | `fastforward` | `1` | allow holding Tab to fast-forward |
|  | `fastcd` | `0` | shorter loading pauses: `1` = disc data 4x faster, `2` or more = that many times (music streaming is unaffected) |
|  | `record` | `0` | `1` = record every session to `recordings/` (button presses and the memory card at the start), to replay a problem exactly |
|  | `autoslot` | `1` | saving and loading use MEMORY CARD slot 1 without the "Select a MEMORY CARD slot" menu; `0` = ask |
|  | `fastcard` | `1` | the game runs at full speed while the memory card is searched, loaded or saved; `0` = original wait |
|  | `quickui` | `1` | menu and message windows open and close at once, fades to and from black (doors too) are 4× shorter; `0` = original |
|  | `fastload` | `1` | the game runs at full speed while "Now Loading" or the black screen between places is shown; the music waits meanwhile, so it does not jump ahead; `0` = original wait |
|  | `overclock` | `200` | CPU speed in percent (`100` = the console, up to `400`): the game no longer drops frames when a lot is going on, e.g. running around the farm while switching tools; video, sound and timing stay the same |
|  | `emptybag` | `1` | R1 / R2 do nothing when there is no tool / item to switch to, so they no longer stop the player for a moment while running; `0` = original |
|  | `textspeed` | `3` | characters the dialogue types at a time: `1` = original speed, `2`–`16` = faster |
|  | `skipscenes` | `1` | hold START during a cutscene to run it at full speed; it pauses while a dialogue box is open, so choices are never made for you; `0` = off |
| `[keyboard]` | `key_up` … `key_select` |  | comma-separated SDL key names, e.g. `key_cross = X,Space,K` |
| `[controller]` | `pad_up` … `pad_select` |  | SDL controller button names: `a b x y back start guide leftshoulder rightshoulder leftstick rightstick dpup dpdown dpleft dpright lefttrigger righttrigger` |
|  | `stick_as_dpad` | `1` | the left analog stick also works as the D-pad |
| `[launcher]` | `language` |  | the launcher's language, `en`, `pt` or `es` (its flags set it); not set: the system's |

Command line options: `--fullscreen`, `--scale N`, `--scaling MODE`, `--upscale N`, `--pgxp`, `--no-crackfix`, `--perspective`, `--no-sprite-depth`, `--widescreen W:H`, `--interp`, `--texturepack`, `--dumptextures`, `--fastcd`,
`--no-autoslot`, `--card N`, `--overclock N`, `--savedir DIR`, `--verify-data`, `--log 0..4`, and a disc image or data pack path. Testing
options are described in [docs/TECHNICAL.md](docs/TECHNICAL.md#testing-and-debugging).

## Saves

Saving works exactly as on the PlayStation: go to bed in your house and pick one of the
three diaries; **Continue** on the title screen loads them. The memory card is
`saves/card1.mcd`, a standard 128 KB raw memory card image as used by DuckStation, ePSXe,
PCSX and others, so you can bring an existing save from an emulator or take yours to one.
If the game folder is not writable, the card is kept in `%APPDATA%\HarvestMoonBTN`
(Windows) or `~/.local/share/HarvestMoonBTN` (Linux).

## How it was ported

**Static recompilation.** An emulator reads the game's machine code while the game runs
and interprets (or JIT-compiles) it instruction by instruction. This port translates the
whole program **once, before it runs**:

1. **Extract.** `tools/iso.py` reads the ISO 9660 file system of your disc image and copies
   out the main executable (`SLUS_011.15`) and the archive that holds the game's 13 code
   overlays (title screen, farm, festivals and minigames, save/load and others, which the
   game swaps in and out of memory).
2. **Analyse.** `tools/recomp.py` decodes the MIPS R3000A machine code and finds every
   function: call targets, code addresses built in registers, function-pointer tables,
   switch jump tables. Analysis repeats until nothing new turns up (about 3,800 functions).
3. **Translate.** Each MIPS function becomes a C function. Registers become fields of a
   CPU structure, memory accesses go through small inline helpers, branches (including
   MIPS branch delay slots) become `goto`s, and jump tables become `switch` statements.
   Every block also counts the CPU cycles the original would take, because the game's
   timing depends on it.
4. **Compile.** The ~380,000 lines of generated C are compiled with clang/gcc into a native
   x86-64 program, together with the runtime.

**The runtime.** The translated game still believes it is running on a PlayStation, so
`runtime/` provides everything it talks to, written natively from public hardware
documentation: the graphics chip (a software renderer reproducing the PS1 GPU's exact
drawing rules), the geometry coprocessor (GTE), the sound chip (24 ADPCM voices with
envelopes, interpolation and reverb), the CD drive (including XA streamed music), the
controller and memory card port, DMA, timers and interrupts, plus the BIOS kernel
functions the game calls (file system, events, memory card files). This hardware layer is
a reimplementation, like the one an emulator contains, but the game's own code runs as
native machine code. SDL2 provides the window, input and audio.

**Checking it.** The hardware pieces have unit tests (`python tools/run_tests.py`). The
whole game was checked by running millions of frames of random input, by comparing runs
frame by frame (screenshots, memory, audio and saves bit-identical between debug and
release builds and between disc image and data pack), and by playing: title screen, new
game, farm and town, saving and loading.

Details for developers: [docs/TECHNICAL.md](docs/TECHNICAL.md).

## Project layout

```
build.py              one-step build: disc image -> native game in dist/
disc/                 put your .cue/.bin here (ignored by git)
tools/
  fetch_deps.py       downloads the Windows toolchain into deps/ (--min: only what the build reads)
  toolchain.txt       those files with their SHA-256 (written by min_toolchain.py)
  build_launcher.py   builds dist-template/HarvestMoonBTN-Launcher.exe, with the sources in it
  release.py          publishes the launcher as a GitHub release (launchers update from there)
  iso.py              disc image + ISO 9660 reader
  mips.py, mdis.py    MIPS decoder, disassembler / cross-reference helper
  recomp.py           the static recompiler (MIPS -> C)
  overlays.json       the 13 code overlays: archive index, load address, code range
  extra_entries.json  entry points that can't be found statically
  compile.py          compiles runtime + generated code
  makepak.py          builds the optional data pack from your disc image
  package.py          assembles the dist folders
  run_tests.py        runtime unit tests
  sheet.py            screenshot contact sheets (testing)
runtime/              the native PlayStation hardware + BIOS reimplementation, frontend
  tests/              unit tests (GTE, GPU, SPU, memory card file system)
dist-template/        hmbtn.ini, README.txt, launcher.pyw and licence text copied into each build
docs/TECHNICAL.md     internals
docs/GRAPHICS.md      the graphics enhancements (upscaling, PGXP, 2D objects) and findings
LICENSE               MIT license for this port's source code
```

Created locally and ignored by git: `deps/` (toolchain), `extract/`, `generated/`,
`build/`, `dist/`.

## Troubleshooting

- **"Game data not found"**: the game can't find your disc image. Put the `.cue` and
  `.bin` into the `disc` folder next to the executable, or set `disc =` in `hmbtn.ini`.
- **"does not match the USA release"** when building: your image is a different region or
  revision, or a bad dump. Only SLUS-01115 is supported.
- **The `.bin` named in the `.cue` is missing**: the `.cue` refers to the `.bin` by file
  name; keep both together and don't rename one without editing the other.
- **No sound**: the game opens the default audio device when it starts and picks up a
  device plugged in later. Check `HarvestMoonBTN.log` next to the executable.
- **Controller not working**: make sure it works in other games; SDL2 must recognise it as
  a game controller. Buttons can be remapped in `hmbtn.ini`.
- Anything else: the log file `HarvestMoonBTN.log` next to the executable usually says what
  went wrong.

## Status

The whole game is recompiled. Tested so far: boot, intro, title screen, new game, farm,
house, town and map, menus and tools, music and sound effects, saving and loading, and long
random-input runs without errors. The festivals and minigames are recompiled like the rest
of the game but haven't been played through extensively yet. The Linux version builds
(natively or cross-compiled) but has had less testing than the Windows version. Bug
reports are welcome in the [issue tracker](https://github.com/GabrielBigardi/HarvestMoonBTN-Recomp/issues); please attach `HarvestMoonBTN.log`.

## License and legal

The source code in this repository (the recompiler, the runtime and the scripts) is released
under the [MIT License](LICENSE). The license covers only this port's own code. It grants no
rights to *Harvest Moon: Back to Nature*, and it does not cover what the build generates from
your disc (the extracted files, the translated game code, the executable and the data pack),
which is derived from the game.

This is an unofficial fan project, not affiliated with or endorsed by Marvelous Inc.,
Natsume Inc., Victor Interactive Software or Sony Interactive Entertainment. *Harvest
Moon* and *Back to Nature* are trademarks of their respective owners. This repository
contains no code, graphics, music, text or other data from the game; you need your own
legally obtained copy, and what the build produces from it is for your personal use only.

The port uses [SDL2](https://libsdl.org) (zlib license, see
`dist-template/LICENSE-SDL2.txt`). The toolchain downloaded by `tools/fetch_deps.py`
(llvm-mingw, SDL2, Zig) is used only to build and is not part of this repository.
Hardware information came from the public PlayStation documentation, above all
[psx-spx](https://psx-spx.consoledev.net/).
