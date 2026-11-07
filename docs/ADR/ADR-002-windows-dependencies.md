# ADR-002: Dependencies and build presets on Windows

**Date:** 2026-11-07
**Status:** Accepted

## Context

ADR-001 chose CMake, Ninja and presets as the only build system, with dependencies from each platform's
package manager: Homebrew on macOS, apt on Ubuntu, and vcpkg on Windows "later". The Windows port has to:

- build, test and run with MSVC on Windows 11 x64, from a terminal and from Qt Creator, and in CI next to
  macOS and Ubuntu;
- pin every C++ dependency's version in the repository, since Windows has no system package manager to
  supply them;
- keep the same commands and build folders as on the other platforms (`cmake --preset <name>`,
  `build/<preset>`);
- keep a first build, which compiles the dependencies from source, under an hour, and later ones fast.

Options considered, by concern:

| Concern | Options | Evaluation |
|---|---|---|
| C++ dependencies | vcpkg in manifest mode | One `vcpkg.json` declares every dependency, its features and a pinned baseline; the CMake toolchain installs them during configure, so the laptop and CI read the same file. Built from source with MSVC, cached as binary packages after the first build. Microsoft maintains it; GitHub's Windows runners ship it. |
| | Conan | Also pins versions, with prebuilt binaries, but a Python tool and a second recipe language beside CMake, and its packages for OpenCV with selected features are often built from source anyway. |
| | Prebuilt downloads (OpenCV's Windows installer and so on) | No version pinning, a different layout per library, and each one found by hand; no FFmpeg development files. |
| Qt | The Qt Online Installer (locally); `aqtinstall` (CI) | Qt's own MSVC binaries, installed in minutes; the installer also installs Qt Creator and a maintenance tool. |
| | vcpkg's `qtbase` | Builds Qt from source: about an hour per cache miss, for binaries no different in use. |
| Generator | Ninja with MSVC (`cl`) | The same single-configuration layout and commands as macOS and Linux; Qt Creator, Visual Studio and VS Code each set up the MSVC environment for it. |
| | The Visual Studio generator | Multi-configuration: different build folders and `--config` arguments on Windows only. |
| Compiler | MSVC | The compiler Qt's and vcpkg's Windows binaries are built with; the one Windows C++ developers use. |
| | MinGW (GCC) | Would need Qt's MinGW kit and vcpkg's community MinGW triplets, and leaves MSVC, which most Windows users of the library would build with, untested. |
| FFmpeg (for `live_sender --hdr`) | vcpkg's `ffmpeg`, without its `gpl` feature | The same libraries as on macOS and Ubuntu, LGPL, which suits an Apache-2.0 repository; D3D11VA hardware decoding is enabled in its Windows build. |

## Decision

1. **vcpkg in manifest mode** supplies OpenCV, FFmpeg, ZeroMQ, cppzmq, nlohmann-json and GoogleTest, from
   `vcpkg.json` with a pinned `builtin-baseline`. OpenCV's default features are turned off and only what
   the project uses is requested (PNG, JPEG, TIFF and WebP decoding, Media Foundation and FFmpeg for video);
   DNN, G-API, calibration and the like would add build time for nothing. `pkgconf` comes as a host tool,
   because the build finds FFmpeg through pkg-config on every platform.
2. **OpenCV's `ffmpeg` feature is on**, so video files, phone videos in HEVC among them, open the same way
   as on Ubuntu, without Microsoft's paid HEVC Video Extensions. It adds little build time, since FFmpeg is
   built for `--hdr` anyway.
3. **Qt is not built by vcpkg.** Locally it comes from the Qt Online Installer (the MSVC 2022 64-bit
   component), in CI from `aqtinstall`. The presets find it through the environment variable `QT_ROOT_DIR`.
4. **Presets `windows-debug` and `windows-release`**: Ninja, `cl`, x64 only, building into
   `build/<preset>`. They read vcpkg's location from the environment variable **`VCPKG_INSTALLATION_ROOT`**,
   not `VCPKG_ROOT`: every Visual Studio developer environment (Developer PowerShell, and the MSVC
   environment IDEs set up from `vcvarsall.bat`) sets `VCPKG_ROOT` to the copy of vcpkg bundled with Visual
   Studio, overriding the user's own. A build that silently used it would get a different vcpkg tool and
   helper scripts from CI's. GitHub's Windows runners already set `VCPKG_INSTALLATION_ROOT` to their copy.
5. **Each platform lists only its own presets**, through preset conditions on the host system.
6. **No sanitizer preset on Windows yet.** MSVC has AddressSanitizer but no UndefinedBehaviorSanitizer;
   macOS CI runs both.

## Consequences

- Positive: one file pins every C++ dependency on Windows; updating means moving the baseline and
  rebuilding.
- Positive: the commands, preset structure and build folders are the same on every platform.
- Negative: the first configure compiles the dependencies: 21 minutes on an Intel Core i9 laptop
  (FFmpeg 12, OpenCV 3), mostly once, since vcpkg caches the built packages.
- Negative: two environment variables to set by hand, one of them with a less familiar name than the
  `VCPKG_ROOT` most vcpkg instructions use.
- Negative: Windows builds against OpenCV 4.x (4.14.0 at this baseline, which offers no OpenCV 5), macOS
  against Homebrew's 5.x; the code already keeps to API both have.
- Operational: some vcpkg downloads come from servers that can stall (nasm.us did, on 2026-11-07); the
  build guide says how to supply such a file by hand.

## Links

- `docs/build-guide.md`: the Windows prerequisites, presets and troubleshooting.
- `docs/ADR/ADR-001-technology-stack.md`: the stack this builds on.
- `vcpkg.json`, `CMakePresets.json`.
