# Sheep, Dog 'n' Wolf: Rewooled for the Nintendo Switch

A homebrew build (`.nro`) for consoles running Atmosphère. As on the PC, **you need your own copy of the game**: its
data is read from the SD card at run time and is not part of this repository or of any release.

## The SD card

```
/switch/sheepdogwolf/
    sheepdogwolf.nro
    environment.txt        the settings (below)
    Levels/  Musics/  Voices/  References/  Bonus/      from your PC game's folder
    saves/                 made by the game: the settings and the saved progress
    log.txt                the last run's log (log-previous.txt: the run before)
```

Start it from the Homebrew Menu. **Open the Homebrew Menu through a game (hold R while starting one)**, not from the
album: an applet gets about 440 MB of memory, which the levels and the music can outgrow. The log says so when it
runs as an applet.

## Controls

The game reads a PlayStation pad. The face buttons go by position, as SDL maps every controller on the PC:

| Switch | Game |
|---|---|
| B / A / Y / X | Cross / Circle / Square / Triangle |
| L / R / ZL / ZR | L1 / R1 / L2 / R2 |
| − / + | Select / Start |
| Left stick, D-pad | movement, menus |
| Right stick, stick clicks | right stick, L3 / R3 |

`SDW_SWAP_AB=1` makes A Cross and B Circle. Handheld mode, a pair of Joy-Con and the Pro Controller all work as
player 1. A USB keyboard works too, with the PC's keys (F3 toggles the performance overlay).

## Settings

`environment.txt`, `NAME=value` lines read at start-up (the file in this folder lists them all with comments):

| Name | Default | |
|---|---|---|
| `SDW_CPU_BOOST` | `1785` | the CPU clock in MHz while the game runs, put back when it quits; `0` leaves it alone. A clock already higher (an overclocking tool) is left alone |
| `SDW_GPU_BOOST` | `0` | the GPU clock in MHz (`0`: left alone) |
| `SDW_SWAP_INTERVAL` | `1` | `1`: 60 fps; `2`: 30 fps |
| `SDW_SWAP_AB` | `0` | `1`: A is Cross, B is Circle |
| `SDW_PERF_OVERLAY` | `0` | `1`: the performance overlay from the start |
| `SDW_NXLINK` | `0` | `1`: the log to `nxlink -s` too |
| `SDW_ARGS` | | the game's flags (`src/platform/options.h`), e.g. `--scale=75 --level=3` |

The picture follows the console: 1920 × 1080 docked, 1280 × 720 handheld, switching when it is docked or undocked.
The pause menu's **Screen %** (25 to 200) scales that, **Anti-aliasing** turns on MSAA (2x / 4x / 8x) and **Texture
Filtering** chooses bilinear or sharp textures; they are saved. Lower Screen % first if a level runs slow.

## Building

devkitPro with `switch-dev`, `switch-sdl2`, `switch-mesa`, `switch-libdrm_nouveau`, `switch-libpng` and
`switch-zlib` (`sudo dkp-pacman -S switch-dev switch-sdl2 switch-mesa switch-libdrm_nouveau switch-libpng
switch-zlib`). Then:

```sh
switch/build.sh                       # build-switch/SheepD3D.nro
switch/package.sh --data "<your PC game folder>"   # build-switch/sdcard/, for the SD card's root
```

`switch/package.sh` without `--data` stages only the `.nro` and `environment.txt`. During development,
`nxlink -s build-switch/SheepD3D.nro` sends the game to a console on the Homebrew Menu's network mode and shows its log
(`SDW_NXLINK=1` in `environment.txt`, and the game's folder still holds the data).

## How it is made

The game is the same code as on the PC: only the platform layer and the renderer are the Switch's own.

- `switch/switch.cmake`: the build, included by `CMakeLists.txt` with devkitPro's toolchain. No launcher. GCC (the
  desktop builds use Clang) builds the game code with inlining and `-fkeep-inline-functions` instead of the desktop's
  `-fno-inline` (`-DSDW_SWITCH_NO_INLINE=ON` goes back).
- `src/render/gl/`: an OpenGL ES 3 renderer (Mesa's nouveau driver through EGL) behind the same `RenderDevice` as the
  SDL3 GPU one, with its draw batching, CPU culling and shaders. Textures are uploaded in their 16-bit formats.
- `src/platform/switch/`: `platform.h` with libnx (display, controllers, keyboard, clocks, language, error dialog) and
  SDL2 (audio at 48 kHz, `.wav` files, threads). libnx's `u64` is not the game's, so files that include `switch.h`
  never include the game's headers.
