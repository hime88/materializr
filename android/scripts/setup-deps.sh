#!/usr/bin/env bash
#
# Reproduce the native prerequisites for the Materializr Android build:
#   * SDL2 source placed at android/app/jni/SDL
#   * FreeType (static) + OpenCASCADE 7.8.1 (shared) cross-compiled for
#     arm64-v8a into $PREFIX
#   * the OCCT .so set copied into the APK's jniLibs
#
# Requires: Android SDK + NDK r26.x (set ANDROID_HOME / ANDROID_NDK), curl,
# cmake, a host C/C++ toolchain. Run from anywhere; paths are derived from the
# repo location. Idempotent-ish: re-running rebuilds from clean build dirs.
set -euo pipefail

# ── Config ───────────────────────────────────────────────────────────────────
ABI="arm64-v8a"
API=24
JOBS="${JOBS:-4}"                       # keep low on RAM-constrained machines
SDL_VER="2.30.9"
FT_VER="2.13.3"
OCCT_TAG="V7_8_1"

ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
NDK="${ANDROID_NDK:-$(ls -d "$ANDROID_HOME"/ndk/* 2>/dev/null | sort -V | tail -1)}"
TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
CMAKE_BIN="$(command -v cmake || echo "$ANDROID_HOME/cmake/3.22.1/bin/cmake")"

REPO="$(cd "$(dirname "$0")/../.." && pwd)"      # materializr repo root
WORK="${MATERIALIZR_WORK:-$HOME/Android}"        # downloads + build trees
PREFIX="$WORK/prefix/$ABI"
DL="$WORK/dl"; SRC="$WORK/src"; BUILD="$WORK/build"
mkdir -p "$DL" "$SRC" "$BUILD" "$PREFIX"

echo "NDK:    $NDK"
echo "PREFIX: $PREFIX"
echo "REPO:   $REPO"
[ -f "$TOOLCHAIN" ] || { echo "NDK toolchain not found: $TOOLCHAIN"; exit 1; }

# Expected SHA-256 of each pinned source tarball — verified after download so a
# corrupted mirror or tampered upstream can't slip in (supply-chain integrity;
# also what F-Droid wants for a reproducible build from source).
SDL2_SHA256="24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4"
FT_SHA256="5c3a8e78f7b24c20b25b54ee575d6daa40007a5f4eea2845861c3409b3021747"
OCCT_SHA256="7321af48c34dc253bf8aae3f0430e8cb10976961d534d8509e72516978aa82f5"

fetch() { # url dest sha256
    [ -f "$2" ] || curl -L --fail --retry 3 -o "$2" "$1"
    if [ -n "$3" ]; then
        echo "$3  $2" | sha256sum -c - || {
            echo "ERROR: checksum mismatch for $2 — refusing to build." >&2
            rm -f "$2"; exit 1
        }
    fi
}

# ── SDL2 source -> android/app/jni/SDL ───────────────────────────────────────
fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz" "$DL/sdl2.tar.gz" "$SDL2_SHA256"
[ -d "$SRC/SDL2-$SDL_VER" ] || tar -xzf "$DL/sdl2.tar.gz" -C "$SRC"
ln -sfn "$SRC/SDL2-$SDL_VER" "$REPO/android/app/jni/SDL"
echo "SDL2 linked at android/app/jni/SDL"

# ── FreeType (static) ────────────────────────────────────────────────────────
fetch "https://download.savannah.gnu.org/releases/freetype/freetype-$FT_VER.tar.gz" "$DL/freetype.tar.gz" "$FT_SHA256"
[ -d "$SRC/freetype-$FT_VER" ] || tar -xzf "$DL/freetype.tar.gz" -C "$SRC"
rm -rf "$BUILD/freetype-$ABI"
"$CMAKE_BIN" -S "$SRC/freetype-$FT_VER" -B "$BUILD/freetype-$ABI" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON \
    -DFT_DISABLE_PNG=ON -DFT_DISABLE_ZLIB=ON
"$CMAKE_BIN" --build "$BUILD/freetype-$ABI" --target install -j"$JOBS"

# ── OpenCASCADE (shared) ─────────────────────────────────────────────────────
fetch "https://github.com/Open-Cascade-SAS/OCCT/archive/refs/tags/$OCCT_TAG.tar.gz" "$DL/occt.tar.gz" "$OCCT_SHA256"
OCCT_DIR="$SRC/OCCT-${OCCT_TAG#V}"
[ -d "$OCCT_DIR" ] || tar -xzf "$DL/occt.tar.gz" -C "$SRC"

# Patch: clang on arm64 rejects char*/unsigned char* aliasing in BRepFont.
sed -i 's#const char\* aTags      = &anOutline->tags\[aStartIndex\];#const char* aTags      = (const char* )\&anOutline->tags[aStartIndex];#' \
    "$OCCT_DIR/src/StdPrs/StdPrs_BRepFont.cxx" || true

rm -rf "$BUILD/occt-$ABI"
"$CMAKE_BIN" -S "$OCCT_DIR" -B "$BUILD/occt-$ABI" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_FIND_ROOT_PATH="$PREFIX" -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH -DBUILD_LIBRARY_TYPE=Shared \
    -DBUILD_MODULE_Draw=OFF -DBUILD_DOC_Overview=OFF -DBUILD_SAMPLES_QT=OFF \
    -DUSE_FREETYPE=ON -D3RDPARTY_FREETYPE_DIR="$PREFIX" \
    -D3RDPARTY_FREETYPE_INCLUDE_DIR_freetype2="$PREFIX/include/freetype2" \
    -D3RDPARTY_FREETYPE_INCLUDE_DIR_ft2build="$PREFIX/include/freetype2" \
    -D3RDPARTY_FREETYPE_LIBRARY="$PREFIX/lib/libfreetype.a" \
    -D3RDPARTY_FREETYPE_LIBRARY_DIR="$PREFIX/lib" \
    -DUSE_TK=OFF -DUSE_TCL=OFF -DUSE_FREEIMAGE=OFF -DUSE_TBB=OFF -DUSE_VTK=OFF \
    -DUSE_RAPIDJSON=OFF -DUSE_OPENVR=OFF -DUSE_DRACO=OFF -DUSE_FFMPEG=OFF
"$CMAKE_BIN" --build "$BUILD/occt-$ABI" --target install -j"$JOBS"

# ── Stage OCCT + resources into the app ──────────────────────────────────────
bash "$REPO/android/copy-occt-libs.sh"

echo
echo "Done. Native prerequisites are ready:"
echo "  OCCT/FreeType prefix : $PREFIX"
echo "  SDL2 source          : android/app/jni/SDL -> $SRC/SDL2-$SDL_VER"
echo "  OCCT .so staged into  : android/app/src/main/jniLibs/$ABI"
echo "Now build the APK:  cd android && ./gradlew assembleDebug"
