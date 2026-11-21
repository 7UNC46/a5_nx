<div align="center">
<img src="launcher/icon.jpg" alt="Asphalt 5 HD" width="160">
  
# a5_nx

**Asphalt 5 HD on Nintendo Switch**

An unofficial Nintendo Switch wrapper for the Android version of
**Asphalt 5 HD**.

[![Switch](https://img.shields.io/badge/Nintendo_Switch-Homebrew-E60012?style=for-the-badge&logo=nintendoswitch&logoColor=white)](#)
[![ARM](https://img.shields.io/badge/ARM-32--bit-0091BD?style=for-the-badge&logo=arm&logoColor=white)](#)
[![Download a5_nx](https://img.shields.io/github/v/release/7UNC46/a5_nx?include_prereleases&label=Download%20A5_NX&style=for-the-badge&logo=nintendoswitch&logoColor=white&color=2EA44F)](https://github.com/7UNC46/a5_nx/releases/latest)
</div>

---

## About

`a5_nx` is a native wrapper that runs the 32-bit (ARMv5TE) Android build of
**Asphalt 5 HD** on Nintendo Switch. It loads the game's own engine library and
recreates the services that library expects under Horizon OS: Android, JNI,
the Java sound and music players, input and OpenGL ES 1.1. The process runs in
AArch32 mode, on libnx32 and a 32-bit build of Mesa.

This release targets **Asphalt 5 HD 3.4.1**
(`com.gameloft.android.GAND.GloftA5HD`, version code 341). The wrapper runs the
game's library `lib/armeabi/libasphalt5.so` from your APK directly.

No game code or data is included. You need your own APK and the game's data
folder.

---

## Controls

| Input | Action |
| --- | --- |
| **Left Stick** | Steer |
| **ZL** | Brake / drift |
| **A** | Nitro |
| **+** | Pause |
| **B / -** | Back |
| **Touchscreen** | Touch input in handheld mode, as on a phone |

The car accelerates by itself, as on the phone.

In menus the D-Pad or the Left Stick moves between the buttons on the screen,
A confirms and B or - goes back. + opens the menu.

**Pointer:** in menus, **L** switches between button selection and a free
pointer. Moving the **Right Stick** switches the pointer on as well. While it is
on, the Right Stick moves it and **A**, **ZR** or **R3** taps where it points.
This reaches places that button selection does not.

---

## Build

### Requirements

* Docker
* The vita2hos AArch32 toolchain image
  (`ghcr.io/vita2hos/devcontainer/vita2hos:latest`): devkitARM, switch-tools
  and miniz
* [libnx32](https://github.com/aks796/libnx32), the 32-bit libnx
* [mesa32](https://github.com/aks796/mesa32), the 32-bit Mesa 20.1 and
  libdrm_nouveau for Switch
* FFmpeg 6.1.2 (LGPL build, see `THIRD_PARTY_NOTICES.md`)
* The `devkitpro/devkita64` image for the launcher

`tools/setup_deps.sh` fetches and builds the dependencies into `deps/`. You
only need to run it once.

```bash
./tools/setup_deps.sh
```

The repository has no icon: the game's icon belongs to Gameloft. Put a 256×256
`launcher/icon.jpg` there first (it stays local, see `.gitignore`).

Compile the 32-bit game program, then the launcher that carries it:

```bash
./build.sh
launcher/build.sh
```

The output is `launcher/a5_nx.nro`. The build number is written into both
programs and appears in `debug.log`.

For a clean rebuild:

```bash
./build.sh clean
./build.sh
```

To pack a release zip from the finished build:

```bash
tools/make_release.sh
```

---

## Running

Requires Atmosphère and a homebrew menu that can install forwarders, such as
sphaira.

Put `a5_nx.nro`, your APK and the game's data folder in this folder on the SD
card:

```text
sd:/switch/a5_nx/
├── a5_nx.nro
├── a5.apk
└── asphalt5/        the game data (package_general.bar_001.cnk ... _093.cnk, ...)
```

The APK is recognized by what it holds, so its file name does not matter.

1. In the homebrew menu, choose **Asphalt 5 HD** and install a forwarder from
   its options.
2. Launch the new **Asphalt 5 HD** icon on the HOME menu.

The first launch takes the game's library out of the APK. It then installs the
game program as an Atmosphère ExeFS override for that icon
(`atmosphere/contents/<title id>/exefs.nsp`) and restarts it.

Afterwards the folder looks like this:

```text
sd:/switch/a5_nx/
├── a5_nx.nro
├── a5.apk
├── asphalt5/        game data
├── config.ini
└── debug.log
```

To update, replace `a5_nx.nro`. The game updates its override on the next
launch.

### Settings (`config.ini`)

The file is written with its defaults on the first launch.

| Section | Setting | Values |
| --- | --- | --- |
| `[display]` | `resolution` | `auto` (default), `1080`, `720` |
| `[display]` | `frame_rate` | `30` (default), `60` |
| `[display]` | `game_size` | `854x480` (default), `800x480`, `1280x720`: the size the game draws at; `1280x720` puts the menus in a corner |
| `[controls]` | `steering` | `stick` (default), `motion`, `both` |
| `[controls]` | `steer_gain` | `0.2` - `1.5` (default `0.6`) |
| `[controls]` | `motion_gain` | `0.5` - `4` (default `1.5`) |
| `[controls]` | `motion_invert` | `false` (default), `true` |
| `[controls]` | `touch_screen` | `true` (default), `false` |
| `[game]` | `language` | `auto` (the console's), `en`, `fr`, `de`, `es`, `it`, `ja` |
| `[performance]` | `boost_cpu_when_loading` | `true` (default), `false` |
| `[debug]` | `cooperative` | keep `false` |

---

## Status

**Version 0.3.0 (beta).** Boot, menus, sound and music work. Whole races run,
with steering, brake and nitro.

### Known issues

* The engine sound drops out now and then during a race.
* The controls in the main menu still need work.
* The yes / no questions (leaving the game, back to the main menu) cannot be
  answered with the D-Pad or the stick yet; tap them on the touch screen.
* A drift may not end when you steer the other way (under investigation;
  `steer_gain` may help).
* When an OBB file is present, the wrapper does not yet check whether it has
  already been extracted.

### Roadmap

* **Menus:** D-Pad control for every screen, including the yes / no questions.
* **Sound:** a steady engine sound.
* **Drift:** check how counter-steering ends a drift.
* **OBB:** detect an OBB and extract it if needed.
* **Intro video:** play the game's intro (needs video playback).

Online features are not available. The game runs as a device without a network
connection, so multiplayer and store features do not work.

The wrapper is built for version **3.4.1** of the game. Other versions have not
been tested.

---

## Credits

**Asphalt 5 HD Nintendo Switch port**: 7UNC46

**Asphalt 5 HD**: Gameloft

The runtime is [android32](https://github.com/aks796/android32) (aks796), via
[mc3_nx](https://github.com/Kaays/mc3_nx) (Kaays). The port layer grew from
[a6_nx](https://github.com/7UNC46/a6_nx). The loader derives from the
open-source Android `.so` loader work by Andy Nguyen (TheOfficialFloW) and
fgsfds, ported to AArch32 with reference to vita2hos. It is MIT-licensed. See
`LICENSE`.

Uses stb_vorbis (Sean Barrett, public domain or MIT), miniz (MIT) and FFmpeg
(LGPL-2.1-or-later). See `THIRD_PARTY_NOTICES.md`.

Built with devkitPro, devkitARM, libnx32 (vita2hos) and a 32-bit build of Mesa.

---

## Contributing

Bug reports and tested improvements are welcome. Include your firmware and
Atmosphère versions, the steps to reproduce, and `debug.log` and
`debug.prev.log` from the game folder. If the game closed by itself, include
`crash.log` and the `a5_nx.elf` from your build as well.

---

## Disclaimer

This is an unofficial fan project and is not affiliated with, sponsored by or
endorsed by Nintendo or Gameloft. Asphalt and Asphalt 5 are trademarks of
Gameloft.

***No game code or assets are included. You need your own copy of the game.***
