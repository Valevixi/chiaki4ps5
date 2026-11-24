# chiaki4ps5

**PS5 Remote Play that runs natively on a jailbroken PS5.** It streams your other PS5 to it, with frame generation (30 → 60 fps) and picture filters.

chiaki4ps5 is a port of [chiaki-ng](https://github.com/chiaki-ng/chiaki-ng), the open-source PlayStation Remote Play client, to the PS5 itself. Everything runs on the console, with no PC in the middle.

## Features

- **Native PS5 app** that shows up on the home screen like any other title. It draws through Vulkan, with RADV on the PS5's GPU.
- **Remote Play to another PS5** at 1080p or 720p, using HEVC or H.264 at 10–50 Mbps.
- **Frame generation**: a 30 fps stream is shown at 60 fps. It's based on [ps5-framegen](https://github.com/Swordpdf/ps5-framegen) (FSR3 optical-flow interpolation), plus a fix in this repo that keeps characters the camera follows from breaking up during fast camera turns.
- **Picture filters**: detail restore, deband, sharpening and color boost.
- **DualSense support**: buttons, sticks, triggers, touchpad, motion and rumble.
- **Profiles**: add as many players as you want, each with a name and a PSN account ID. You can type the ID, or fill it in from the PSN account signed in on the PS5.
- **On-screen settings** for everything, entered with an on-screen keyboard. No config files needed.

## Requirements

- A jailbroken PS5 that can run homebrew, with a homebrew loader that registers folder titles from `/data/homebrew` (for example ShadowMountPlus).
- A second PS5 (the one you stream from) with Remote Play turned on.
- Both consoles on the same network. Wired is best.

## Install

1. Download the latest release and unzip it.
2. Copy the `PPSA48766` folder to `/data/homebrew/` on the jailbroken PS5 (FTP works).
3. Refresh your homebrew loader so **chiaki4ps5** shows up on the home screen.

## Setup (first launch)

1. **Console IP**: enter the IP of the PS5 you stream from. You can find it on that PS5 under *Settings > Network > Connection Status*.
2. **+ Add profile**: give it a name and the PSN account ID that's signed in on the PS5 you stream from.
   - If it's the same account as the one on this PS5, choose **"Fill in from this PS5's signed-in account"**.
   - Otherwise, type the account ID, either in base64 (like `AbCdEfGhIjk=`) or as the number.
3. Pick your profile. The first time, on the other PS5 go to *Settings > System > Remote Play > Link Device* and enter the PIN it shows into chiaki4ps5.

## Controls

| While streaming | Does |
|---|---|
| Hold **Options** + press **D-pad Up** | Open / close the settings menu |
| Hold **Options** + press **D-pad Down** | PS button on the streamed PS5 |
| Tap **Options** | Options in the game |
| Settings menu | Up/Down choose, Left/Right change, X select, O close |

The same list is shown in the app under **Controls & help**.

## Recommended settings

- **Frame gen** set to 30 → 60, **Resolution** 1080p, **Codec** HEVC, **Bitrate** 30–50 Mbps on a wired network.
- Frame gen works best when the stream itself runs at a steady 30 fps.

## Building

The app is built with the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk), the pacbrew homebrew libraries (ffmpeg, OpenSSL, Opus, Dear ImGui), and [mihawk-99's PS5_Vulkan / PS5_Mesa](https://github.com/mihawk-99) RADV port for the GPU.

```
ps5/libchiaki/   Makefile and PS5 shims that build chiaki-ng's lib/ into libchiaki.a
ps5/app/         the app (main.cpp), Vulkan function table, network wrappers, shaders, build.sh
ps5/framegen/    ps5-framegen's interpolation shader with the "hold" fix (hold-fix.patch) and the compiled shaders
lib/src/         chiaki-ng sources changed for the PS5 (sock.c: non-blocking sockets, takion.c: a bigger receive buffer)
```

1. Build `ps5/libchiaki` with the pacbrew `prospero-clang` toolchain to get `libchiaki.a`.
2. Build RADV from PS5_Vulkan, which needs LLVM 18.
3. Run `ps5/app/build.sh`. It compiles the shaders and the app, links against RADV and the pacbrew libraries, and packages a title folder with `ps5-native-tool`.

Paths in `build.sh` point to where the dependencies were unpacked, so adjust them to your setup.

## Credits

- [chiaki-ng](https://github.com/chiaki-ng/chiaki-ng), and the original [Chiaki](https://git.sr.ht/~thestr4ng3r/chiaki) by thestr4ng3r: the Remote Play protocol and client library.
- [ps5-framegen](https://github.com/Swordpdf/ps5-framegen) by Swordpdf: frame generation, based on AMD FidelityFX FSR3.
- [mihawk-99](https://github.com/mihawk-99): PS5_Vulkan and PS5_Mesa (RADV on the PS5).
- The PS5 Payload SDK and pacbrew projects.

## License

Like chiaki-ng, this project is licensed under the **GNU AGPL v3**. See `COPYING` and `LICENSES/`. ps5-framegen is GPL-3.0-or-later, and the FSR3 shaders are MIT.

This is an unofficial project. It isn't affiliated with or endorsed by Sony Interactive Entertainment. "PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment.
