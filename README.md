# 9mm Vita

This is a wrapper/port of *9mm HD Android* for the *PS Vita*.

The port works by loading the official Android ARMv7 executable in memory, resolving
its imports with native functions and patching it in order to properly run.

**This software does not contain the original code, executables, assets, or other
non redistributable parts of the game. The authors do not promote or condone piracy
in any way. To launch and play the game on their PS Vita device, users must provide
their own legally obtained copy of the game in the form of an .apk file.**

Tested with the last Android release, the one whose `libCopStory.so` is 11524815
bytes. Other builds will refuse to start rather than misbehave, see
[Build Instructions](#build-instructions-for-developers) for why.

## What you need before starting

- A Vita or PSTV on custom firmware, with VitaShell.
- Around 1.8 GB free on `ux0:`.
- Your own copy of the game, still installed on an Android device (or a backup of
  it). You need both the `.apk` **and** the data the game downloads on first
  launch, which is most of that 1.8 GB.

## Setup Instructions (For End Users)

### 1. Plugins

Install [kubridge](https://github.com/bythos14/kubridge/releases) and
[FdFix](https://github.com/TheOfficialFloW/FdFix/releases): copy `kubridge.skprx`
and `fd_fix.skprx` into `ur0:tai`, then add them to `ur0:tai/config.txt` under the
`*KERNEL` section:

```
*KERNEL
ur0:tai/kubridge.skprx
ur0:tai/fd_fix.skprx
```

**Note** Don't install fd_fix.skprx if you're using rePatch plugin!

Reboot after editing `config.txt`, otherwise the plugins aren't loaded.

### 2. Shader compiler

Install `libshacccg.suprx` if you don't have it already, by following
[this guide](https://samilops2.gitbook.io/vita-troubleshooting-guide/shader-compiler/extract-libshacccg.suprx).
It has to come off your own console, nobody can hand it to you. The game compiles
its shaders while it runs, so it won't get past a black screen without it.

### 3. Get the game files off Android

Two things, from a device where the game is installed and has finished
downloading:

- **The apk.** Any apk extractor from the Play Store will pull it out, or grab it
  over adb. It has to be the ARMv7 build (`armeabi-v7a`).
- **The data folder.** On the device it's at
  `Android/data/com.gameloft.android.ANMP.Gloft9MHM/files/` and it's called
  `Gloft9MHM`. About 1.7 GB. If it's much smaller than that, the game hasn't
  finished downloading and you should launch it on Android and let it finish.

### 4. Put them on the Vita

Create `ux0:data/9mm/` and put in it:

- everything that's inside the `Gloft9MHM` folder (the `.gla` files,
  `sounds_hi.glza`, and so on). The files themselves, not the folder.
- two files out of the apk: an apk is a zip, so rename a copy to `.zip`, open it,
  and pull `libCopStory.so` and `libStormGLOFT.so` out of `lib/armeabi-v7a/`.

You should end up with paths like `ux0:data/9mm/libCopStory.so` and
`ux0:data/9mm/actors.gla`.

**How to move 1.7 GB there.** Pick whichever applies:

- *SD2Vita / microSD*: take the card out, put it in your PC, copy straight onto it.
  By far the fastest, and what I'd do.
- *USB*: VitaShell, press `Select`, the Vita shows up as a drive on your PC.
- *FTP*: VitaShell, press `Select` to switch to FTP, connect with FileZilla or
  similar. Works fine, but Vita wifi is slow, so expect this to take a while.

### 5. Install the vpk

Download the `.vpk` from the [Releases](../../releases) page and copy it anywhere
on the Vita (`ux0:` root is fine). In VitaShell, move the cursor onto it, press
`X`, and confirm. The bubble shows up on the home screen.

### 6. Optional

[PSVshell](https://github.com/Electry/PSVshell/releases) lets you overclock to
500 MHz. Loading is a lot less painful with it.

<details>
<summary>Optional: script instead of copying files by hand</summary>

If you have Python around and would rather not dig through the apk, this does step
3 and 4 for you and checks nothing is missing:

```sh
python3 tools/prepare_data.py --apk 9mm-HD.apk --cache Gloft9MHM.zip --out 9mm-data
```

Then copy everything from `9mm-data/` into `ux0:data/9mm/`. It also leaves out the
Android savegames, which is harmless either way but means you start clean.
</details>

## Controls

| Vita | Action |
| --- | --- |
| Left stick | Move |
| Right stick | Aim |
| R | Fire |
| L | Slow motion |
| Square | Reload |
| Cross | Sprint |
| D-pad Left / Right | Switch weapon |
| Start | Pause |

The touch widgets for those are hidden since the buttons already cover them.
Everything else in the touch UI still works.

## Known Issues

- Parts of the title screen draw wrong. It's a Scaleform (Flash) overlay,
  gameplay isn't affected.
- Multiplayer is untested. I'd assume it's broken.
- If you copied the Android savegames along with the data, you'll start with the
  progression from that device rather than a fresh game.

## Build Instructions (For Developers)

You need a [VitaSDK](https://vitasdk.org/) built with the **softfp** ABI, with the
dependent libraries recompiled the same way (`-mfloat-abi=softfp`).

```sh
git clone --recursive https://github.com/iwannagooutside/9mm-vita
cd 9mm-vita
cmake -Bbuild . && cmake --build build
```

`-DDEV_SHORTCUTS=ON` adds two debug shortcuts in game (Select dumps a frame,
Triangle ends the level). Off by default, keep it that way for anything you hand
out.

A note on `source/patch.c` if you plan to touch it: the patches are byte offsets
into one specific build of the game, so each one declares the instruction it is
about to overwrite and checks it first. Get a different build and you get a message
saying so, instead of a quietly corrupted binary and a crash three functions away.
Adding a patch means adding that check too.

Most of what's in there works around the same thing: levels stream in one zone at a
time while rendering and gameplay are already running, so for a few frames the
player has no zone and portals point at zones that don't exist yet. The engine
notices in several of those spots, logs the null pointer, and then reads through it
anyway. Android gets away with it.

## Credits

- [TheFloW](https://github.com/TheOfficialFloW) for the Android SO loader
  everything here sits on top of.
- [Rinnegatamante](https://github.com/Rinnegatamante) for vitaGL and so_util.
- [v-atamanenko](https://github.com/v-atamanenko) for the soloader boilerplate this
  started from, and FalsoJNI.
- [elliencode](https://github.com/elliencode) for FalsoNDK.
- Everybody on the Vita scene whose ports I read through to figure out how any of
  this works.

## License

MIT, see [LICENSE](LICENSE).

## Disclaimer

9mm is a trademark of Gameloft SE. © Gameloft. All rights reserved. Gameloft and
the Gameloft logo are trademarks of Gameloft in the U.S. and/or other countries.

The work presented in this repository is not "official" and is not produced or
sanctioned by the owner(s) of the aforementioned trademark(s).
