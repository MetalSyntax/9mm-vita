#!/usr/bin/env python3
"""
Assembles the game data for the 9mm Vita port.

Nothing game related is bundled here or downloaded: this only reads files you
already own (an apk, plus the data the game downloaded on your Android device) and
lays them out the way the port expects.

    python3 prepare_data.py --apk 9mm-HD.apk --cache Gloft9MHM.zip --out 9mm-data
    python3 prepare_data.py --apk 9mm-HD.apk --cache /path/to/Gloft9MHM/ --out 9mm-data

Then copy everything inside 9mm-data/ to ux0:data/9mm/ on the Vita.

The cache is what the game pulls down on first launch. On Android it sits in
Android/data/com.gameloft.android.ANMP.Gloft9MHM/files/, as a Gloft9MHM folder of
roughly 1.7 GB.
"""

import argparse
import os
import sys
import zipfile

# The two native libraries, taken out of the apk.
APK_LIBS = {
    "lib/armeabi-v7a/libCopStory.so":   "libCopStory.so",
    "lib/armeabi-v7a/libStormGLOFT.so": "libStormGLOFT.so",
}

# Android savegames. Copying them works, but you inherit that device's progress,
# so they're skipped to start clean.
SKIP = {
    "achievements.dat", "androidTrophy.dat", "mpprofile.sav", "progress.dat",
    "serverConfig.sav", "settings.dat", "tracklog.dat", "save.dat",
}

# Without these the game won't get past startup.
REQUIRED = {
    "actors.gla", "automat.gla", "effects.gla", "entities.gla", "levels.gla",
    "menu_hud.gla", "particlesystems.gla", "sounds_hi.glza", "strings.gla",
    "weapons.gla", "location_main_menu.gla", "oconf.bar",
}


def log(msg):
    print(msg, flush=True)


def extract_apk(apk_path, out_dir):
    if not zipfile.is_zipfile(apk_path):
        sys.exit(f"{apk_path} isn't a readable apk.")
    got = []
    with zipfile.ZipFile(apk_path) as z:
        names = set(z.namelist())
        for src, dst in APK_LIBS.items():
            if src not in names:
                sys.exit(
                    f"{src} isn't in the apk.\n"
                    "This needs the ARMv7 (armeabi-v7a) build of the game; an\n"
                    "arm64-only apk can't be ported."
                )
            with z.open(src) as fsrc, open(os.path.join(out_dir, dst), "wb") as fdst:
                fdst.write(fsrc.read())
            got.append(dst)
    return got


def iter_cache(cache_path):
    """Yields (filename, read_fn) for every file in the cache."""
    if os.path.isdir(cache_path):
        root = cache_path
        # accept either the Gloft9MHM folder or its parent
        if os.path.isdir(os.path.join(root, "Gloft9MHM")):
            root = os.path.join(root, "Gloft9MHM")
        for name in sorted(os.listdir(root)):
            full = os.path.join(root, name)
            if os.path.isfile(full):
                yield name, (lambda p=full: open(p, "rb").read())
    elif zipfile.is_zipfile(cache_path):
        with zipfile.ZipFile(cache_path) as z:
            for info in z.infolist():
                if info.is_dir():
                    continue
                name = os.path.basename(info.filename)
                if name:
                    yield name, (lambda i=info, zz=cache_path: _read_zip(zz, i))
    else:
        sys.exit(f"{cache_path} is neither a folder nor a readable archive.")


def _read_zip(zip_path, info):
    with zipfile.ZipFile(zip_path) as z:
        return z.read(info)


def main():
    ap = argparse.ArgumentParser(
        description="Assemble the 9mm HD game data for the PS Vita port, "
                    "from files you already own.")
    ap.add_argument("--apk", required=True, help="9mm HD apk (armeabi-v7a)")
    ap.add_argument("--cache", required=True,
                    help="game cache: a Gloft9MHM folder, or a .zip of it")
    ap.add_argument("--out", default="9mm-data", help="output folder")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)

    log("Pulling the native libraries out of the apk")
    for name in extract_apk(args.apk, args.out):
        log(f"   {name}")

    log("Copying the game data")
    copied, skipped, total = set(), [], 0
    for name, read in iter_cache(args.cache):
        if name in SKIP:
            skipped.append(name)
            continue
        if name.startswith("."):
            continue
        data = read()
        with open(os.path.join(args.out, name), "wb") as f:
            f.write(data)
        copied.add(name)
        total += len(data)
        if len(data) > 50 * 1024 * 1024:
            log(f"   {name}  ({len(data) / 1e6:.0f} MB)")

    log(f"   {len(copied)} files, {total / 1e9:.2f} GB")
    if skipped:
        log("Left out (Android savegames, so you start clean):")
        log(f"   {', '.join(sorted(skipped))}")

    missing = REQUIRED - copied
    if missing:
        log("\nThe cache is incomplete. Missing:")
        for m in sorted(missing):
            log(f"   {m}")
        log("\nThe game probably hasn't finished downloading its data. Launch it")
        log("on Android, let it finish, then try again.")
        sys.exit(1)

    log("\nDone: " + os.path.abspath(args.out))
    log("Copy everything in there to ux0:data/9mm/ on the Vita.")


if __name__ == "__main__":
    main()
