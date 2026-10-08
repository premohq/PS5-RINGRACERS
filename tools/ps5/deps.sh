#!/usr/bin/env bash
# Fetch and prepare everything the PlayStation 5 build needs, pinned and
# checksummed, into one directory (build/ps5-deps unless PS5_DEPS says
# otherwise). Nothing is installed system-wide and nothing needs root.
#
#   tools/ps5/deps.sh            # prepare, then print the variables build.sh uses
#
# What it fetches:
#   ps5-payload-sdk v0.42        the PS5 toolchain glue, libc stubs and libc++
#   PacBrew v0.40.2              prebuilt PS5 ports: curl, OpenSSL, zlib, libpng, Opus
#   ps5-opengl SDK 1.0.1         OpenGL 4.6 / compatibility + EGL on the PS5's GPU
#   ps5-native-app-boilerplate   the ELF -> eboot.bin converter and libc.prx runtime
#   Pillow and etcpak (PyPI)     to draw the home screen art (tools/ps5/presentation.py)
#
# Host requirements (Debian/Ubuntu names): clang-18 lld-18 libclang-rt-18-dev
# cmake ninja-build python3 python3-venv curl unzip git.

set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
deps=${PS5_DEPS:-"$repo/build/ps5-deps"}
mkdir -p "$deps/downloads"

payload_sdk_url="https://github.com/ps5-payload-dev/sdk/releases/download/v0.42/ps5-payload-sdk.zip"
payload_sdk_sha="8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da"
pacbrew_url="https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/v0.40.2/ps5-payload-dev.tar.gz"
pacbrew_sha="a85f65de418a8e6a898c6c3e3c870d50fff7618a200e4dd59ea9692af6ecec4d"
opengl_url="https://github.com/blackbearreloaded/ps5-opengl/releases/download/v1.0.1/ps5-opengl-sdk-1.0.1.tar.gz"
opengl_sha="aaa2e8957f55e1fc0b654dcb35a36e585f7e635d63952da40a28be7f64fde741"
boilerplate_url="https://github.com/blackbearreloaded/ps5-native-app-boilerplate.git"
boilerplate_rev="4f531c4b517f80bcb6b1267135848168d2250047"
# The boilerplate builds its host tool against zlib and fetches it from
# zlib.net; GitHub carries the same release, so seed it from there.
zlib_url="https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz"
zlib_sha="bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16"

say() { printf '==> [ps5-deps] %s\n' "$*" >&2; }

for tool in clang-18 curl unzip tar git python3 sha256sum; do
	command -v "$tool" >/dev/null || { echo "missing required command: $tool" >&2; exit 2; }
done
[[ -f "$(clang-18 --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a" ]] || {
	echo "compiler-rt builtins missing: install libclang-rt-18-dev" >&2
	exit 2
}

fetch() { # url sha256 destination
	if [[ -f $3 ]] && printf '%s  %s\n' "$2" "$3" | sha256sum --check --strict >/dev/null 2>&1; then
		return
	fi
	say "downloading $(basename "$3")"
	curl -fsSL --retry 4 -o "$3.part" "$1"
	printf '%s  %s\n' "$2" "$3.part" | sha256sum --check --strict >&2
	mv "$3.part" "$3"
}

# --- Payload SDK, with the PacBrew ports inside it ----------------------------
sdk="$deps/ps5-payload-sdk"
if [[ ! -f $sdk/.ringracers-ready ]]; then
	fetch "$payload_sdk_url" "$payload_sdk_sha" "$deps/downloads/ps5-payload-sdk-v0.42.zip"
	fetch "$pacbrew_url" "$pacbrew_sha" "$deps/downloads/pacbrew-v0.40.2.tar.gz"
	rm -rf "$sdk"
	say "unpacking the payload SDK"
	unzip -q "$deps/downloads/ps5-payload-sdk-v0.42.zip" -d "$deps"
	say "unpacking the PacBrew ports into the SDK"
	tar -xzf "$deps/downloads/pacbrew-v0.40.2.tar.gz" -C "$sdk/target/user" \
		--strip-components=4 opt/ps5-payload-sdk/target/user/homebrew
	[[ -f $sdk/target/user/homebrew/lib/libcurl.a ]] || { echo "PacBrew layout unexpected" >&2; exit 2; }
	touch "$sdk/.ringracers-ready"
fi

# --- ps5-opengl ---------------------------------------------------------------
opengl="$deps/ps5-opengl-sdk-1.0.1"
if [[ ! -f $opengl/sdk/lib/libPS5OpenGLCore33.a ]]; then
	fetch "$opengl_url" "$opengl_sha" "$deps/downloads/ps5-opengl-sdk-1.0.1.tar.gz"
	say "unpacking ps5-opengl"
	tar -xzf "$deps/downloads/ps5-opengl-sdk-1.0.1.tar.gz" -C "$deps"
	(cd "$opengl/sdk" && sha256sum --check --strict --quiet manifest.sha256)
fi

# --- Native app boilerplate: converter, signer and libc.prx -------------------
boilerplate="$deps/ps5-native-app-boilerplate"
if [[ ! -x $boilerplate/build/host/ps5-native-tool || ! -f $boilerplate/runtime/libc.prx ]]; then
	if [[ ! -d $boilerplate/.git ]]; then
		say "cloning the native app boilerplate"
		git clone --quiet "$boilerplate_url" "$boilerplate"
	fi
	git -C "$boilerplate" fetch --quiet origin "$boilerplate_rev" 2>/dev/null || true
	git -C "$boilerplate" checkout --quiet --force "$boilerplate_rev"

	# The same change ps5-opengl's native titles make (its
	# tools/build-native-test-app.sh): give the system libc heap a fixed
	# 256 MiB instead of "unlimited", since malloc itself is redirected to
	# the title's own direct-memory heap (src/ps5/native/app_heap.c).
	writer="$boilerplate/tooling/native/sce_module_writer.cpp"
	heap_default='write_u64(result.data, result.heap_size, std::numeric_limits<std::uint64_t>::max());'
	if grep -Fq "$heap_default" "$writer"; then
		sed -i "s/$heap_default/write_u64(result.data, result.heap_size, 0x10000000ULL);/" "$writer"
	fi

	mkdir -p "$boilerplate/.deps/native/zlib"
	ln -sfn "$sdk" "$boilerplate/.deps/native/ps5-payload-sdk"
	fetch "$zlib_url" "$zlib_sha" "$boilerplate/.deps/native/zlib/zlib-1.3.2.tar.gz"

	say "building the converter and the libc.prx runtime"
	if command -v ccache >/dev/null; then
		export USE_CCACHE=${USE_CCACHE:-1}
	else
		export USE_CCACHE=0
	fi
	(cd "$boilerplate" && bash tools/rebuild-libc.sh >&2)
fi

# --- Python, for the home screen art -----------------------------------------
venv="$deps/venv"
if ! "$venv/bin/python" -c 'import etcpak, PIL' >/dev/null 2>&1; then
	say "creating a Python environment for the artwork"
	rm -rf "$venv"
	python3 -m venv "$venv" || { echo "python3 -m venv failed: install python3-venv" >&2; exit 2; }
	"$venv/bin/pip" install --quiet --disable-pip-version-check "etcpak==0.9.15" "pillow==12.3.0" >&2
fi

cat <<EOF
PS5_PYTHON=$venv/bin/python
PS5_PAYLOAD_SDK=$sdk
PS5_OPENGL_PREFIX=$opengl/sdk
PS5_BOILERPLATE=$boilerplate
EOF
