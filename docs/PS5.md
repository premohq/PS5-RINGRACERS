# Ring Racers on PlayStation 5

An unofficial build of Dr. Robotnik's Ring Racers for jailbroken PlayStation 5
consoles. The game is upstream Ring Racers, unchanged except where noted below;
what is new is a platform layer in place of SDL (`src/ps5/`) and a build that
produces a PS5 title folder.

**It is not affiliated with Kart Krew.** Please do not report problems with this
build to them.

**It has not yet been run on a console.** Everything here builds, links and is
converted into a signed title folder, and each piece of it follows a PS5
project that has run on hardware, but the game itself is untested on a PS5.
Treat the first runs as testing, and see "When it does not work" below.

## What it is built from

| | |
|---|---|
| Ring Racers | [KartKrewDev/RingRacers](https://github.com/KartKrewDev/RingRacers) `master` at `4bad15a` (2026-08-31), imported unmodified as the first commit of this branch |
| OpenGL | [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) 1.0.1: Mesa's OpenGL over the console's own graphics driver, with EGL |
| Toolchain | [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) v0.42 with Clang 18 |
| Libraries | [PacBrew](https://github.com/ps5-payload-dev/pacbrew-repo) v0.40.2 ports: curl, OpenSSL, zlib, libpng, Opus |
| Title packaging | [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) `4f531c4`: ELF to `eboot.bin` conversion, signing, `libc.prx` |

## What works compared with the PC version

| | PS5 | Notes |
|---|---|---|
| Software renderer | yes | the PC default |
| Legacy OpenGL renderer | yes | needs ps5-opengl's compatibility profile, which it supports but has not conformance-tested |
| Resolution setting | yes | renders at the chosen size, scaled to the screen |
| 1080p / 1440p / 4K output, 60 / 120 Hz | yes | chosen at startup, see "Display mode" |
| Controllers | DualSense, up to 4 | one per signed-in user; rumble; the light bar shows your colour |
| Splitscreen | up to 4 | sign in a user for each controller |
| Sound effects and music | yes | the PC mixer, resampled to the console's 48 kHz |
| Voice chat | listen only | other players are heard; there is no microphone capture |
| Online play | yes | host and join, server browser, add-on downloads, STUN |
| Add-ons | yes | from `.ringracers/addons` in the home directory, or downloaded from a server |
| Replays, screenshots, profiles, unlocks | yes | saved in the home directory |
| USB keyboard | yes, untested | chat, the console and keyboard controls, as on PC; see below |
| On-screen keyboard | the game's own | every text field in the menus (names, server address, replay titles), with a controller |
| Mouse | no | the game only uses it in menus on PC |
| Discord Rich Presence | no | no Discord client on a console |
| WebM movie recording | no | its libyuv has no PS5 port; GIF and PNG screenshots still work |

### Online play and versions

This build is upstream's `master`, which calls itself **2.5** (protocol 14). The
current PC release is **2.4** (protocol 13). Ring Racers refuses to connect two
different versions, and the server browser lists only servers of your own
version, so **this build cannot join public 2.4 servers**. It can play online
with other consoles running this build, and with PC players running a build of
the same upstream commit.

The game data is the same for both: `master` checks for exactly the archives
2.4 ships (the hashes in `src/d_main.cpp` are identical), so a 2.4 install is
what you copy to the console.

To host, forward UDP port 5029 to the console, as on PC.

## What you need

* A PS5 that can run homebrew and launch native title folders, for example
  with ShadowMountPlus. The packaging tools this build uses were verified by
  their authors on firmware 6.02 and 12.70.
* Ring Racers 2.4 from [kartkrew.org](https://www.kartkrew.org/): you need its
  `bios.pk3` and `data/` folder. They are not in this repository.
* To build: Linux or WSL with `clang-18`, `lld-18`, `libclang-rt-18-dev`,
  `cmake`, `ninja-build`, `python3`, `python3-venv`, `git`, `curl` and `unzip`.

On Debian or Ubuntu:

```bash
sudo apt install clang-18 lld-18 libclang-rt-18-dev cmake ninja-build python3 python3-venv git curl unzip
```

## Building

```bash
tools/ps5/build.sh --assets ~/RingRacers
```

That one command fetches everything (about 600 MB, checked against pinned
SHA-256 sums, kept in `build/ps5-deps/`), builds the game into `build/ps5/`, and
writes the title folder:

```
build/ps5/dist/PPSA99620/
    eboot.bin
    sce_module/libc.prx
    sce_sys/param.json  icon0.png  pic0.dds  pic1.dds
    ca-bundle.crt
    bios.pk3            <- with --assets
    data/               <- with --assets
```

and `build/ps5/dist/PPSA99620.zip`, the same folder zipped.

The home screen art is drawn at packaging time by `tools/ps5/presentation.py`
from the Ring Racers logo and icon already in this repository: the pixel
Robotnik icon, a purple checkered-track background with the logo for the
selected tile, and the logo centred for the launch screen. To use your own,
put `icon0.png` (512x512), `pic0.dds` and `pic1.dds` (3840x2160, BC7) in
`tools/ps5/sce_sys/`.

`--assets` copies `bios.pk3` and `data/` from a Ring Racers install into the
title so it runs with nothing else on the console. Leave it out to keep the
title small and put the game data on the console separately (see below).

The pieces can also be run on their own: `tools/ps5/deps.sh` fetches and
prepares the toolchain and prints where it put it; `cmake/ps5/Toolchain-PS5.cmake`
is an ordinary CMake toolchain file; `tools/ps5/package.sh` turns
`build/ps5/ringracers.elf` into the title folder.

## Installing

Upload the `PPSA99620` folder (not the zip) to `/data/homebrew/` on the console
with an FTP client, then launch it the way your loader launches native titles.

The game looks for `bios.pk3` and `data/` in these places, in order:

1. `/data/ringracers/`
2. the title folder itself (`/app0`), where `--assets` puts them
3. `/mnt/usb0/ringracers/`, `/mnt/usb1/ringracers/`, `/mnt/ext0/ringracers/`

Keeping them in `/data/ringracers/` means a new build of the title is a 60 MB
upload instead of 750 MB.

### Where your files go

The game's home directory, where `.ringracers/` with your config, game data,
profiles, replays, screenshots, add-ons and `latest-log.txt` lives, is
`/data/ringracers` if the title can write there, otherwise `/download0`, the
title's own storage. Only the first can be reached over FTP. Whether a title
can write to `/data` depends on the loader; check the log (below) to see which
one was used.

## Command line arguments

A title starts with no command line. To pass the game arguments, put them in a
text file named `ringracers-args.txt` in `/data/ringracers/` (or the home
directory), one or more per line, `#` for comments:

```
# join a friend's game as soon as the game starts
-connect 192.168.1.20
```

Two arguments exist only on PS5:

* `-ps5res 1080|1440|2160` picks the output resolution (default 1080).
* `-ps5hz 60|120` picks the refresh rate (default 60). A TV that cannot do
  120 Hz stays at 60.

The output mode is fixed for the run: the OpenGL implementation can only change
it by restarting, which loses every texture. The game's own resolution setting
in the video menu still works and is independent of it.

## When it does not work

The game writes what it knows:

* `ringracers-stdout.txt`: everything the game and the OpenGL stack print,
  unbuffered, so it survives a crash. In `/data/ringracers/` if the title can
  write there, otherwise in `/download0`.
* `.ringracers/latest-log.txt` in the home directory: the game's own log, as
  on PC.
* The kernel log: everything above also goes there; read it with
  [klogsrv](https://github.com/ps5-payload-dev/klogsrv) on port 3232.

A fatal error also shows as a notification on the home screen.

Things most likely to need attention on a first run, in rough order:

* **The legacy OpenGL renderer.** It, and the 2D renderer underneath both
  renderers, are written for OpenGL 2.1 and GLSL 1.20 and so need a
  compatibility-profile context. ps5-opengl creates one but only tests its core
  profile. If the screen stays black, try the other renderer with
  `-software` or `-opengl` in the arguments file.
* **Exiting.** Quitting from the menu asks the system to close the title the
  way PS4 homebrew does; if the PS5 shell disagrees, close it from the PS
  button menu.
* **Audio.** The output is opened for the user who started the game, then for
  the system user.
* **The USB keyboard.** Its library is loaded when the game starts rather than
  linked in, so that a console that will not give it to a title loses the
  keyboard and nothing else. The log says `USB keyboard ready` or why not.
  Whether a keyboard plugged in mid-game is picked up has not been tried; if
  one does nothing, restart the game with it plugged in.

## How the port is put together

| | |
|---|---|
| `src/ps5/` | the platform layer: video (EGL), input (DualSense), sound (AudioOut), system, paths, threads |
| `src/ps5/native/` | the title's C runtime: startup, the malloc heap, linker script, shims |
| `cmake/ps5/` | the CMake toolchain and the PS5 link |
| `tools/ps5/` | dependency fetching, building, packaging, and the title metadata and icon |

Shared engine code is changed in seven files, 41 lines in all, each change
marked `PS5`, `SRB2_PS5` or `PS5_CA_BUNDLE` with a comment saying why: the top-level and `src/` CMake files
choose the PS5 platform layer; `r_opengl.h`/`.cpp` take Mesa's GL headers and
log to the console; `i_video_common.cpp` includes the PS5 GL header instead of
SDL's; `http-mserv.c` and `d_netfil.c` point curl at the certificates the title
carries.

Some details that cost time and are worth knowing before changing anything:

* The payload SDK's `libc.a` must be linked after the system module stubs. It
  carries `mmap` and `mprotect` versions for payloads that reach for kernel
  primitives a title does not have; ahead of the stubs they replace the
  system's own.
* The browser's POSIX module (`libScePosixForWebKit`) is left out of the link
  entirely; `getaddrinfo`, `strcasestr` and `arc4random_buf` come from the
  payload SDK's `libc.a`, whose resolver uses `libSceNet`.
* An import the system will not load stops a title from starting at all, so
  anything optional (the USB keyboard) is loaded at run time with
  `sceKernelLoadStartModule` instead of being linked.
* The converter that makes `eboot.bin` refuses unresolved weak symbols. Two
  libraries have them (OpenSSL's `dladdr`, zstd's tracing hooks); they are
  defined in `src/ps5/native/runtime_shims.c`.
* C++ exceptions work because the linker script defines `__eh_frame_start` and
  friends: the SDK's libunwind finds unwind tables through them rather than a
  dynamic loader.

## Licence

Ring Racers is distributed by Kart Krew under the GNU General Public License,
version 2 or later. The PS5 build links components under the GPL version 3 or
later (ps5-opengl, the payload SDK's runtime libraries, the boilerplate's
startup code and heap), so a PS5 binary built from this branch is distributed
under the GPL version 3 or later as a whole.
