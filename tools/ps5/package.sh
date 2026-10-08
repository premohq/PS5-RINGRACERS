#!/usr/bin/env bash
# Turn build/ps5/ringracers.elf into a PS5 title folder.
#
#   tools/ps5/package.sh [--assets /path/to/RingRacers]
#
# Output: build/ps5/dist/PPSA99620/ and build/ps5/dist/PPSA99620.zip. Upload
# the folder (not the zip) to /data/homebrew/ on the console and launch it
# with your homebrew loader; see docs/PS5.md.
#
# --assets copies bios.pk3 and data/ from a Ring Racers install into the
# title, so it runs with nothing else on the console. Without it, the game
# looks for them in /data/ringracers (see src/ps5/ps5_paths.cpp).
#
# The steps are those of the native-app boilerplate's tools/build.sh, after
# its link: convert the LLVM ELF into a PS5 module, sign it as a development
# FSELF, and lay out the folder with the boilerplate's libc.prx runtime.

set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build=${PS5_BUILD_DIR:-"$repo/build/ps5"}
assets=""

while (( $# > 0 )); do
	case "$1" in
		--assets) assets=${2:?--assets needs a directory}; shift 2 ;;
		*) echo "usage: $0 [--assets /path/to/RingRacers]" >&2; exit 2 ;;
	esac
done

# A plain assignment, so that a failure in deps.sh stops this script.
deps_env=$("$repo/tools/ps5/deps.sh")
eval "$deps_env"

elf="$build/ringracers.elf"
[[ -f $elf ]] || { echo "$elf not found: build first (tools/ps5/build.sh)" >&2; exit 2; }

tool="$PS5_BOILERPLATE/build/host/ps5-native-tool"
param="$repo/tools/ps5/sce_sys/param.json"
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")

# Loader and container constants from the boilerplate's tools/build.sh,
# which its authors validated on firmware 6.02 and 12.70.
module_sdk=0x02000009
companion_sdk=0x08050001
fself_magic=0x1D3D154F

# Every system module the title may import: the same set the link used
# (cmake/ps5/PS5Platform.cmake), the payload SDK's stubs less the browser's
# POSIX module, plus ps5-opengl's two for the graphics driver.
stubs="$build/stubs"
rm -rf "$stubs"
mkdir -p "$stubs"
for so in "$PS5_PAYLOAD_SDK"/target/lib/*.so; do
	[[ $(basename "$so") == libScePosixForWebKit.so ]] && continue
	ln -s "$so" "$stubs/"
done
ln -sf "$PS5_OPENGL_PREFIX/lib/libSceAgc.so" "$stubs/libSceAgc.so"
ln -sf "$PS5_OPENGL_PREFIX/lib/libSceAgcDriver.so" "$stubs/libSceAgcDriver.so"

printf '==> [ps5] converting %s\n' "$elf"
"$tool" link --in "$elf" --out "$build/eboot.elf" \
	--stub-dir "$stubs" --module-sdk "$module_sdk" \
	--companion-sdk "$companion_sdk" --file-name eboot.elf

dist="$build/dist"
app="$dist/$title_id"
rm -rf "$app" "$dist/$title_id.zip"
mkdir -p "$app/sce_sys" "$app/sce_module"

printf '==> [ps5] signing eboot.bin\n'
"$tool" self --sign --in "$build/eboot.elf" --out "$app/eboot.bin" --magic "$fself_magic"

cp "$param" "$app/sce_sys/param.json"
cp "$repo/tools/ps5/sce_sys/icon0.png" "$app/sce_sys/icon0.png"
# Backgrounds: the boilerplate's, until someone draws Ring Racers ones
# (3840x2160 BC7 DDS; see the boilerplate's docs/PRESENTATION_ASSETS.md).
for picture in pic0.dds pic1.dds; do
	if [[ -f $repo/tools/ps5/sce_sys/$picture ]]; then
		cp "$repo/tools/ps5/sce_sys/$picture" "$app/sce_sys/$picture"
	else
		cp "$PS5_BOILERPLATE/sce_sys/$picture" "$app/sce_sys/$picture"
	fi
done
cp "$PS5_BOILERPLATE/runtime/libc.prx" "$app/sce_module/libc.prx"
# Mozilla's CA certificates, as PacBrew's curl was built with, for HTTPS to
# the master server and add-on downloads (PS5_CA_BUNDLE in src/ps5).
cp "$PS5_PAYLOAD_SDK/target/user/homebrew/etc/ca-bundle.crt" "$app/ca-bundle.crt"

"$tool" self --inspect --file "$app/eboot.bin"

if [[ -n $assets ]]; then
	[[ -f $assets/bios.pk3 && -d $assets/data ]] || {
		echo "$assets does not hold bios.pk3 and data/" >&2
		exit 2
	}
	printf '==> [ps5] copying the game data from %s\n' "$assets"
	cp "$assets/bios.pk3" "$app/"
	cp -r "$assets/data" "$app/"
fi

# The console only starts a title whose files are open to everyone; ps5-opengl
# found this the hard way and zips its showcase with 0777 for that reason.
chmod -R 0777 "$app"

printf '==> [ps5] zipping\n'
python3 - "$dist" "$title_id" <<'PY'
import os, sys, zipfile
dist, title = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(os.path.join(dist, title + ".zip"), "w", zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk(os.path.join(dist, title)):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(root, name)
            info = zipfile.ZipInfo.from_file(path, os.path.relpath(path, dist))
            info.external_attr = (0o100777 << 16)
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(path, "rb") as f:
                z.writestr(info, f.read())
PY

printf '\nTitle folder: %s\nZip:          %s.zip\n' "$app" "$app"
