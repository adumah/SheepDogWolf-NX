<p align="center">
    <img src="res/banner.png?raw=true" style="width: 512px;" />
</p>

<h1 align="center">Sheep, Dog 'n' Wolf: Rewooled NX</h1>

<h3 align="center">A Nintendo Switch homebrew port of <a href="https://github.com/SDW-Community/Sheep-Dog-n-Wolf-Rewooled">Sheep, Dog 'n' Wolf: Rewooled</a>, the open-source implementation of Sheep, Dog 'n' Wolf PC</h3>

---

### What it is

The game's decompiled code from [Rewooled](https://github.com/SDW-Community/Sheep-Dog-n-Wolf-Rewooled), running natively
on the Switch as a `.nro` for consoles running Atmosphère: 60 fps, docked (1080p) and handheld (720p), with the
Joy-Con, a Pro Controller or a USB keyboard.

- **You need your own copy of the PC game: no game data or assets are in this repository or its releases.**
- An OpenGL ES 3 renderer (Mesa / nouveau) in place of SDL3's GPU API, which has no Switch driver
- The platform layer on libnx and SDL2: audio at 48 kHz, controllers, the clocks, the system language
- Runs at the stock clocks; a CPU or GPU boost can be set in `environment.txt` (put back on quit)
- The pause menu's Screen %, anti-aliasing and texture filtering work on the Switch too

### Installing

1. Copy `sheepdogwolf.nro` and `environment.txt` to `/switch/sheepdogwolf/` on the SD card.
2. Copy `Levels/`, `Musics/`, `Voices/`, `References/` and `Bonus/` from **your** PC game's folder next to them.
3. Start the Homebrew Menu by **holding R while starting a game** (the album has too little memory), then the game.

Controls, settings and troubleshooting: **[switch/README.md](switch/README.md)**.

### Building

devkitPro with `switch-dev switch-sdl2 switch-mesa switch-libdrm_nouveau switch-libpng switch-zlib`, then:

```sh
switch/build.sh                                   # build-switch/SheepD3D.nro
switch/package.sh --data "<your PC game folder>"  # build-switch/sdcard/ for the SD card's root
```

The desktop builds (Windows, macOS, Linux) of upstream Rewooled still work from this tree: see the
[upstream README](https://github.com/SDW-Community/Sheep-Dog-n-Wolf-Rewooled#building).

### What the port changed

The game code is upstream's. The Switch's own parts are `switch/` (the build), `src/render/gl/` (the renderer) and
`src/platform/switch/` (the platform layer). Two fixes to shared code, both found on hardware:

- `src/app/d3dapp.cpp`: the frame limiter tested for a zero frame time *after* dividing by it. That division is
  undefined in C++, so GCC dropped the test, and the first frame spun forever.
- `src/objects/mcard.cpp`: anonymous structs in anonymous unions at namespace scope (a Clang extension) made portable
  for GCC, with the same layout.

## Credits

- **[SDW-Community](https://github.com/SDW-Community/Sheep-Dog-n-Wolf-Rewooled)** and the Rewooled decompilation
  contributors, for the decompiled game this port runs. The decompilation was done with Claude and Codex coordinating
  via an MCP mailbox server; in-game testing and knowledge of the game's inner workings came from the Sheep Raider
  community.
- **lu9**, for the project's icon
- **uhwot**, for [sdw_re_stuff](https://github.com/uhwot/sdw_re_stuff) (MIT): the research into the game's asset formats
- **The whole Sheep Raider Discord community**, for helping crack this game open
- **adumah**, for the Switch port

## Licence

MIT, as upstream: see [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md). The game itself, its program, data and assets,
belongs to its rights holders.
