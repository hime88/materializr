# Building Materializr

One repo, four targets. The windowing/input backend is SDL2 on every platform;
the touch interface is a **runtime setting** (Settings ▸ General ▸ Touch mode,
default on for Android, off on desktop) — not a separate build.

## Linux (desktop)

```sh
sudo apt install build-essential cmake git libsdl2-dev libgl-dev \
    libocct-data-exchange-dev libocct-draw-dev libocct-foundation-dev \
    libocct-modeling-algorithms-dev libocct-modeling-data-dev \
    libocct-visualization-dev libcurl4-openssl-dev zlib1g-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/materializr
```

If `libsdl2-dev` is absent, CMake builds SDL 2.30.9 from source (needs the X11
dev headers). GLM and Dear ImGui are always fetched by CMake.

The release AppImage is built in Docker: `./scripts/build-appimage.sh`
(see `Dockerfile`; CI runs this on x86_64 and aarch64 via
`.github/workflows/linux.yml`).

## Windows

CI (`.github/workflows/windows.yml`) is the reference: vcpkg provides
`opencascade glew curl sdl2` (x64-windows), then a standard CMake/MSVC build
with `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`.

## macOS (Apple Silicon)

```sh
brew install cmake opencascade sdl2
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build -j$(sysctl -n hw.ncpu)
./build/materializr
```

Needs the Xcode Command Line Tools (`xcode-select --install`) for AppleClang.
GLM and Dear ImGui are fetched by CMake; OpenCASCADE and SDL2 come from Homebrew,
and curl + zlib from the macOS SDK. The GL backend uses the system OpenGL
framework (`<OpenGL/gl3.h>`) — no GLEW loader — with a forward-compatible **3.3
Core** context running the same GLSL 330 shaders as the other desktop targets.

Tested on arm64 (Apple Silicon), including HiDPI/Retina — the offscreen 3D
viewport renders at the display's pixel resolution.

A self-contained `Materializr.app` + `.dmg` is built by
`./packaging/macos/build-dmg.sh` (run after the build above; needs
`brew install dylibbundler`). It copies every Homebrew/OpenCASCADE dylib into
the bundle and rewrites install names, so the app runs on a Mac that has never
seen Homebrew. It is ad-hoc signed (not notarized): a downloaded copy is
quarantined, so the first launch needs **System Settings ▸ Privacy & Security ▸
"Open Anyway"** (macOS 15 removed the old right-click ▸ Open bypass), or
`xattr -dr com.apple.quarantine Materializr.app`.

The bundled Homebrew dylibs are built for the macOS they were compiled on, so a
locally built `.dmg` requires that macOS or newer — the script writes the true
floor into `LSMinimumSystemVersion`. CI builds on the `macos-14` runner, so the
released `.dmg` targets **macOS 14+**; it is built, the bundle is launch-tested,
and the artifact uploaded on pushes to `main` (`.github/workflows/macos.yml`).
Not yet wired up: Intel/universal binaries and Developer-ID signing/notarization.

## Android (arm64-v8a)

Prerequisites: JDK 17, Android SDK + NDK r26.x, cmake, curl on the host.

```sh
# one-time: fetch + cross-compile SDL2 / FreeType / OpenCASCADE 7.8.1
# (sources are SHA-256 verified; ~30+ min for OCCT)
ANDROID_HOME=~/Android/Sdk ./android/scripts/setup-deps.sh

cd android && ./gradlew assembleDebug
# -> app/build/outputs/apk/debug/app-debug.apk
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Native prerequisites land under `$MATERIALIZR_WORK` (default `~/Android`);
the OCCT `.so` set is staged into `android/app/src/main/jniLibs/` (not
committed — everything builds from pinned upstream source).

## Layout notes

- `src/` is shared by all targets. Platform code is guarded with
  `#if defined(__ANDROID__)`; touch *behaviour* gates on
  `materializr::touchMode()` (see `src/touch_mode.h`) so a tablet with a
  mouse — or a desktop touchscreen — can switch interaction models at runtime.
- `src/main.cpp` is the desktop entry; `src/android_main.cpp` (SDL_main) is
  Android's. Each build includes only its own.
- `android/` is self-contained (Gradle project, vendored SDL Java glue with a
  one-line soft-keyboard patch, dependency scripts).
