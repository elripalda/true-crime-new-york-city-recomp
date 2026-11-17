#!/bin/bash
# Build the native Linux/SteamOS test package from a native build directory.
#   scripts/package-linux.sh [ntsc-build-dir] [output-dir] [pal-build-dir] [pal-eu-build-dir]
set -euo pipefail
umask 022
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$SCRIPT_DIR/.." && pwd)
BUILD=${1:-/root/tcnyc-linux-build}
OUT=${2:-$REPO/dist}
PAL_BUILD=${3:-/root/tcnyc-linux-pal-build}
PAL_EU_BUILD=${4:-/root/tcnyc-linux-pal-eu-build}
NAME=TrueCrimeNYC-linux-x86_64

# Reproducible archive: use the commit timestamp when CI provides it, otherwise
# a fixed epoch. The same value stamps BUILD.txt, the tar entries and gzip -n.
EPOCH=${SOURCE_DATE_EPOCH:-946684800}
case "$EPOCH" in
    ''|*[!0-9]*)
        echo "SOURCE_DATE_EPOCH must be an integer Unix timestamp, not '$EPOCH'." >&2
        exit 1
        ;;
esac
export SOURCE_DATE_EPOCH=$EPOCH
BUILD_TIME=$(date -u -d "@$EPOCH" '+%Y-%m-%dT%H:%M:%SZ')
VERSION=$(tr -d '\r\n' < "$REPO/launcher/VERSION")
test -n "$VERSION"
# A release package must come from the tag it names: UPDATE-PROTOCOL carries
# VERSION and the updater refuses an archive whose release tag disagrees.
if [ "${TCNYC_ALLOW_UNTAGGED:-0}" != 1 ]; then
    RELEASE_TAG=v$VERSION
    HEAD_TAG=$(git -C "$REPO" describe --tags --exact-match HEAD 2>/dev/null || true)
    if [ "$HEAD_TAG" != "$RELEASE_TAG" ]; then
        echo "HEAD is ${HEAD_TAG:-untagged}, expected the release tag $RELEASE_TAG." >&2
        echo "Tag the release commit, or set TCNYC_ALLOW_UNTAGGED=1 for a local test package." >&2
        exit 1
    fi
fi

if [ ! -f "$BUILD/TrueCrimeNYC" ]; then
    echo "Native build not found: $BUILD/TrueCrimeNYC" >&2
    echo "Build platform/linux first, or pass the build directory as the first argument." >&2
    exit 1
fi
if [ ! -f "$PAL_BUILD/TrueCrimeNYC" ]; then
    echo "Native PAL build not found. Build with -DTCNYC_PAL=ON and pass the build directory as the third argument." >&2
    exit 1
fi
if [ ! -f "$PAL_EU_BUILD/TrueCrimeNYC" ] ||
   ! grep -qx 'TCNYC_PAL_EU:BOOL=ON' "$PAL_EU_BUILD/CMakeCache.txt"; then
    echo "Native PAL EU build not found. Build with -DTCNYC_PAL_EU=ON and pass the build directory as the fourth argument." >&2
    exit 1
fi
if ! grep -qx 'TCNYC_PAL:BOOL=ON' "$PAL_BUILD/CMakeCache.txt" ||
   ! grep -qx 'TCNYC_PAL:BOOL=OFF' "$BUILD/CMakeCache.txt"; then
    echo "Expected separate NTSC and PAL CMake build directories." >&2
    exit 1
fi
for file in README-LINUX.md README.md sdk/THIRD_PARTY.txt sdk/LICENSE; do
    if [ ! -f "$REPO/$file" ]; then
        echo "Missing package input: $REPO/$file" >&2
        exit 1
    fi
done

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

PKG="$STAGE/$NAME"
mkdir -p "$PKG/lib" "$PKG/assets" "$PKG/LICENSES" "$PKG/music"  # music: the default custom music folder

# the release binary: no debug tables, the rpath already points at $ORIGIN/lib
install -m 0755 "$BUILD/TrueCrimeNYC" "$PKG/TrueCrimeNYC"
install -m 0755 "$PAL_BUILD/TrueCrimeNYC" "$PKG/TrueCrimeNYCPAL"
install -m 0755 "$PAL_EU_BUILD/TrueCrimeNYC" "$PKG/TrueCrimeNYCPALEU"
strip --strip-unneeded "$PKG/TrueCrimeNYC" "$PKG/TrueCrimeNYCPAL" "$PKG/TrueCrimeNYCPALEU"

SHADERC=$(ldconfig -p 2>/dev/null | awk '/libshaderc\.so\.1 /{print $NF; exit}' || true)
if [ -z "$SHADERC" ]; then
    echo "libshaderc.so.1 was not found; install libshaderc1" >&2
    exit 1
fi
cp -L "$SHADERC" "$PKG/lib/libshaderc.so.1"
# the shaderc package's own dependencies must exist on the target
if ! LD_LIBRARY_PATH="$PKG/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        ldd "$PKG/lib/libshaderc.so.1" > "$STAGE/ldd-shaderc.txt" 2>&1; then
    echo "ldd failed for lib/libshaderc.so.1:" >&2
    cat "$STAGE/ldd-shaderc.txt" >&2
    exit 1
fi
if grep -q 'not found' "$STAGE/ldd-shaderc.txt"; then
    echo "libshaderc.so.1 has unresolved dependencies:" >&2
    grep 'not found' "$STAGE/ldd-shaderc.txt" >&2
    exit 1
fi

# Every dependency of the release binary must resolve with the bundled lib/
# directory in the search path. glibc, libstdc++ and libgcc are intentionally
# not bundled; they come from the target system.
for executable in "$PKG/TrueCrimeNYC" "$PKG/TrueCrimeNYCPAL" "$PKG/TrueCrimeNYCPALEU"; do
    if ! LD_LIBRARY_PATH="$PKG/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
            ldd "$executable" > "$STAGE/ldd.txt" 2>&1; then
        echo "ldd failed for $executable:" >&2
        cat "$STAGE/ldd.txt" >&2
        exit 1
    fi
    if grep -q 'not found' "$STAGE/ldd.txt"; then
        echo "$executable has unresolved shared-library dependencies:" >&2
        grep 'not found' "$STAGE/ldd.txt" >&2
        echo "Install the missing development/runtime packages before packaging, or bundle only the" >&2
        echo "non-system libraries the binary needs (not glibc, libstdc++ or libgcc)." >&2
        exit 1
    fi
done

cp "$REPO/README-LINUX.md" "$PKG/README-LINUX.md"
cp "$REPO/README.md" "$PKG/README.md"
cp "$REPO/CHANGELOG.md" "$PKG/CHANGELOG.md"
cp "$REPO/sdk/THIRD_PARTY.txt" "$PKG/THIRD_PARTY.txt"
cp "$REPO/sdk/LICENSE" "$PKG/LICENSES/XboxXBE-Recomp-LICENSE.txt"
cp "$REPO/LICENSE" "$PKG/LICENSES/Project-LICENSE.txt"
cp "$REPO/sdk/third_party/fsr3_vk/LICENSE.txt" "$PKG/LICENSES/AMD-FidelityFX-SDK-1.1.4.txt"

if [ -f "$BUILD/CMakeCache.txt" ]; then
    DLSS_SDK=$(sed -n 's/^TCNYC_DLSS_SDK:PATH=//p' "$BUILD/CMakeCache.txt")
    if [ -n "$DLSS_SDK" ]; then
        case "$DLSS_SDK" in /*) ;; *) DLSS_SDK="$REPO/$DLSS_SDK" ;; esac
        if [ -f "$DLSS_SDK/include/nvsdk_ngx_helpers_vk.h" ] &&
           [ -f "$DLSS_SDK/lib/Linux_x86_64/libnvsdk_ngx.a" ]; then
            dlss_runtime=("$DLSS_SDK"/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.*)
            if [ "${#dlss_runtime[@]}" -ne 1 ] || [ ! -f "${dlss_runtime[0]}" ]; then
                echo "Expected one NVIDIA DLSS runtime library in $DLSS_SDK/lib/Linux_x86_64/rel" >&2
                exit 1
            fi
            install -m 0644 "${dlss_runtime[0]}" "$PKG/lib/"
            install -m 0644 "${dlss_runtime[0]}" "$PKG/lib/libnvidia-ngx-dlss.so"
            install -m 0644 "$DLSS_SDK/LICENSE.txt" "$PKG/LICENSES/NVIDIA-RTX-SDK.txt"
        fi
    fi
fi
for file in /usr/share/doc/libshaderc1/copyright /usr/share/doc/glslang-dev/copyright; do
    if [ -f "$file" ]; then
        cp "$file" "$PKG/LICENSES/$(basename "$(dirname "$file")")-copyright.txt"
    fi
done
if [ -f "$BUILD/_deps/imgui-src/LICENSE.txt" ]; then
    cp "$BUILD/_deps/imgui-src/LICENSE.txt" "$PKG/LICENSES/imgui-LICENSE.txt"
fi

cat > "$PKG/assets/PUT-THE-GAME-HERE.txt" <<'EOF'
Put the game in this folder, one of:

  - the extracted files: default.xbe with the game's folders beside it, or
  - a disc image: any .iso file.

The extracted files are used when both are present. No game assets are included
in this package.
EOF

GLIBC=$(objdump -T "$PKG/TrueCrimeNYC" "$PKG/TrueCrimeNYCPAL" "$PKG/TrueCrimeNYCPALEU" | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1 || true)
[ -n "$GLIBC" ] || GLIBC=unknown
{
    echo "True Crime: New York City - native Linux build"
    echo "Version: $VERSION"
    echo "Built: $BUILD_TIME"
    echo "Git: $(git -C "$REPO" describe --always --dirty 2>/dev/null || echo unknown)"
    echo "GLIBC: $GLIBC"
} > "$PKG/BUILD.txt"

chmod 0755 "$PKG/TrueCrimeNYC"
chmod 0644 "$PKG/README.md" "$PKG/README-LINUX.md" "$PKG/THIRD_PARTY.txt" "$PKG/BUILD.txt" \
    "$PKG/assets/PUT-THE-GAME-HERE.txt" "$PKG/lib/libshaderc.so.1"
for licence in "$PKG"/LICENSES/*; do
    if [ -e "$licence" ]; then
        chmod 0644 "$licence"
    fi
done
chmod 0755 "$PKG" "$PKG/lib" "$PKG/assets" "$PKG/LICENSES"

mkdir -p "$OUT"
if [[ -n $(find "$PKG" -type f \( -iname '*.xbe' -o -iname '*.iso' \) -print -quit) ]]; then
    echo "The release staging directory contains game assets." >&2
    exit 1
fi
printf 'tcnyc-update-1\n%s\n' "$VERSION" > "$PKG/UPDATE-PROTOCOL"
(cd "$PKG" && find . -type f ! -name SHA256SUMS -printf '%P\0' | sort -z | xargs -0 sha256sum > SHA256SUMS)

tar --sort=name --format=gnu --mtime="@$EPOCH" --owner=0 --group=0 --numeric-owner \
    -C "$STAGE" -cf - "$NAME" | gzip -n > "$OUT/$NAME.tar.gz"
(cd "$OUT" && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256")
ls -l "$OUT/$NAME.tar.gz" "$OUT/$NAME.tar.gz.sha256"
