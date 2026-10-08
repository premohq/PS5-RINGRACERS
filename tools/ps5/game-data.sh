#!/usr/bin/env bash
# Fetch the Ring Racers 2.4 game data, pinned and checksummed, and print the
# directory that holds its bios.pk3 and data/ (build/ps5-deps/ringracers-2.4
# unless PS5_DEPS says otherwise).
#
#   tools/ps5/game-data.sh
#
# The archive is the one Kart Krew attach to their v2.4 release for packagers
# (Flathub bundles the same file). Every archive the game checks matches the
# hashes in src/d_main.cpp. The download is about 750 MB and is kept in
# build/ps5-deps/downloads with the SDKs.

set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
deps=${PS5_DEPS:-"$repo/build/ps5-deps"}
mkdir -p "$deps/downloads"

data_url="https://github.com/KartKrewDev/RingRacers/releases/download/v2.4/Dr.Robotnik.s-Ring-Racers-v2.4-Assets.zip"
data_sha="eebad71b872c20f3323425bc4f4321b9b6256472bc485010d0a73c43fe0af120"

say() { printf '==> [ps5-data] %s\n' "$*" >&2; }

for tool in curl unzip sha256sum; do
	command -v "$tool" >/dev/null || { echo "missing required command: $tool" >&2; exit 2; }
done

fetch() { # url sha256 destination
	if [[ -f $3 ]] && printf '%s  %s\n' "$2" "$3" | sha256sum --check --strict >/dev/null 2>&1; then
		return
	fi
	say "downloading $(basename "$3")"
	curl -fsSL --retry 4 -o "$3.part" "$1"
	printf '%s  %s\n' "$2" "$3.part" | sha256sum --check --strict >&2
	mv "$3.part" "$3"
}

data="$deps/ringracers-2.4"
if [[ ! -f $data/.ringracers-ready ]]; then
	fetch "$data_url" "$data_sha" "$deps/downloads/ringracers-v2.4-assets.zip"
	rm -rf "$data"
	say "unpacking bios.pk3 and data/"
	unzip -q "$deps/downloads/ringracers-v2.4-assets.zip" 'bios.pk3' 'data/*' -d "$data"
	touch "$data/.ringracers-ready"
fi

printf '%s\n' "$data"
