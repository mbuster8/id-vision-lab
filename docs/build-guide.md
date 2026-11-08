# Build guide

How to install the tools, build every part of the project, run the tests and work on it in Qt Creator.
For running the programs, see the [user guide](user-guide.md).

Verified on macOS 27.0.1 on Apple silicon (Mac mini), Apple clang 21, October 2026. Continuous integration
also builds and tests every push on Ubuntu 24.04. Windows 11 with Visual Studio 2022 has its own presets
and setup: see [Windows](#windows).

## Prerequisites (macOS)

1. The Xcode Command Line Tools (compiler, `git`, LLDB):

   ```sh
   xcode-select --install
   ```

2. [Homebrew](https://brew.sh), then the libraries and build tools:

   ```sh
   brew install cmake ninja pkgconf qt opencv zeromq cppzmq nlohmann-json googletest
   ```

   Homebrew's `opencv` brings `ffmpeg` with it, whose libraries `live_sender --hdr` reads videos with. They
   are optional: CMake finds them through `pkgconf`, and without them `live_sender` builds without `--hdr`.

3. Qt Creator, the reference IDE (optional; any CMake-aware editor works):

   ```sh
   brew install --cask qt-creator
   ```

Versions this was verified with:

| Package | Version |
|---|---|
| `cmake` | 4.4.4 (3.25 or newer is required) |
| `ninja` | 1.13.2 |
| `qt` | 6.12.0 (6.4 or newer is required) |
| `opencv` | 5.0.0 (the code also uses only API that exists in OpenCV 4) |
| `zeromq` / `cppzmq` | 4.3.5 / 4.11.0 |
| `nlohmann-json` | 3.12.0 (3.11 or newer is required) |
| `ffmpeg` | 9.0.2 (optional; 6.1 or newer, which Ubuntu 24.04's CI job tests) |
| `googletest` | 1.18.0 |
| Qt Creator | 20.0.2 |

## Building

The project uses CMake presets. Each preset builds into its own folder under `build/`:

| Preset | Build type | Folder |
|---|---|---|
| `debug` | Debug | `build/debug` |
| `release` | Release | `build/release` |
| `asan` | Debug with AddressSanitizer and UndefinedBehaviorSanitizer | `build/asan` |

From the repository root:

```sh
cmake --preset debug           # configure
cmake --build --preset debug   # build everything
ctest --preset debug           # run all tests
```

Replace `debug` with `release` or `asan` for the other builds. To rebuild from scratch, add
`--clean-first` to the build command, or delete the preset's folder under `build/`.

Then run what you built (the [user guide](user-guide.md) explains each program), with `build/release` for
the release build:

```sh
open build/debug/apps/viewer/visual_debug_viewer.app                   # the Visual Debug Viewer
build/debug/examples/pipeline_demo/pipeline_demo                       # sends it ten images
build/debug/examples/live_sender/live_sender --pattern                  # streams two live channels
open build/debug/apps/example_sender/visual_debug_example_sender.app   # the Visual Debug Example Sender
```

The build is warning-free under `-Wall -Wextra -Wpedantic`. Warnings are not errors, so a newer compiler
or library cannot break the build, but a new warning from project code should be fixed.

### What gets built

| Target | What it is | Where it lands (in `build/<preset>/`) |
|---|---|---|
| `visual_debug` | The sender library: wire format, `Client`, `Server`, `sendImage`, `sendLive` | `libs/visual_debug/libvisual_debug.a` |
| `viewer_core` | The viewer's UI-free logic: display conversion and settings, history, live channels, pixel readout, zoom | `libs/viewer_core/libviewer_core.a` |
| `qt_common` | Qt helpers shared by both apps | `apps/qt_common/libqt_common.a` |
| `viewer_ui` | The viewer's windows, as a library its tests link | `apps/viewer/libviewer_ui.a` |
| `visual_debug_viewer` | **The Visual Debug Viewer** | `apps/viewer/visual_debug_viewer.app` |
| `example_sender_ui` | The example sender's window, as a library its tests link | `apps/example_sender/libexample_sender_ui.a` |
| `visual_debug_example_sender` | The Visual Debug Example Sender, a Qt app that sends image files | `apps/example_sender/visual_debug_example_sender.app` |
| `pipeline` | The demo's OpenCV stages | `examples/pipeline_demo/libpipeline.a` |
| `pipeline_demo` | Command-line demo: an OpenCV pipeline that sends every stage | `examples/pipeline_demo/pipeline_demo` |
| `minimal_sender` | The smallest use of `sendImage` | `examples/minimal_sender/minimal_sender` |
| `overlay_sender` | An image sent with overlays | `examples/overlay_sender/overlay_sender` |
| `live_sender_lib` | `live_sender`'s argument parsing, test pattern, edge channel, read-ahead, real-time playback and sending, without the reading of cameras and files | `examples/live_sender/liblive_sender_lib.a` |
| `live_sender_video` | Opens a video file for `live_sender`: AVFoundation first on macOS, then OpenCV's default backend | `examples/live_sender/liblive_sender_video.a` |
| `live_sender_ffmpeg` | Reads a video with FFmpeg's libraries for `live_sender --hdr`, as 16-bit frames with their HDR tags. Built only when CMake finds FFmpeg's development files (`libavformat`, `libavcodec`, `libswscale`, `libavutil`); configuring says so when it does not | `examples/live_sender/liblive_sender_ffmpeg.a` |
| `live_sender` | Command-line example: streams a camera, a video file or a test pattern as live channels. With `live_sender_video` and the tests, the only targets that need OpenCV's `videoio` module | `examples/live_sender/live_sender` |
| `visual_debug_tests`, `viewer_core_tests`, `qt_common_tests`, `viewer_ui_tests`, `example_sender_ui_tests`, `pipeline_tests`, `live_sender_tests` | GoogleTest executables | `tests/` |

To build one target: `cmake --build --preset debug --target pipeline_demo`.

The apps' file names are the same on every platform. On macOS, Finder lists the file name, while the menu
bar, ⌘-Tab and the Dock show the bundle's name, such as **Visual Debug Viewer**. Each bundle's icon is
made at build time from the PNG in `resources/icons/`, with the `sips` and `iconutil` tools that come with
macOS. On Windows the same PNG becomes each `.exe`'s icon, for Explorer, the taskbar and Alt-Tab: a small
build tool, `id_vision_lab_make_ico` (from `cmake/make_ico.cpp`, with OpenCV), writes an `.ico` with 256, 48,
32 and 16 pixel images, and a generated `.rc` file compiles it in.

### Options

| Option | Default | Effect |
|---|---|---|
| `ID_VISION_LAB_BUILD_QT_APPS` | `ON` | The viewer, the example sender, `qt_common` and the Qt tests |
| `ID_VISION_LAB_BUILD_EXAMPLES` | `ON` | `pipeline_demo`, `minimal_sender`, `overlay_sender`, `live_sender` and their tests |
| `ID_VISION_LAB_BUILD_TESTS` | `ON` | All GoogleTest executables |
| `ID_VISION_LAB_SANITIZERS` | `OFF` | AddressSanitizer and UndefinedBehaviorSanitizer (the `asan` preset turns it on) |

### Building without Qt

The two libraries never use Qt. To prove it, or to build on a machine without Qt, turn the Qt apps off
and build into a separate folder:

```sh
cmake --preset debug -B build/no-qt -DID_VISION_LAB_BUILD_QT_APPS=OFF
cmake --build build/no-qt
ctest --test-dir build/no-qt               # the tests that need no Qt (398 on macOS, 395 on Linux and Windows)
```

CMake then does not look for Qt at all; both libraries, the three examples and their tests still build.

## Running the tests

```sh
ctest --preset debug                       # everything (800 tests on macOS, about 110 seconds; 797 on Linux and Windows)
ctest --preset debug -R WireFormat         # only tests whose name matches a regular expression
ctest --preset debug -R "Client|Server"
ctest --preset asan                        # the same tests under the sanitizers (about 151 seconds on macOS)
ctest --preset debug -L whole              # only the whole-program runs
ctest --preset debug -LE whole             # everything except them
```

Every GoogleTest case is its own CTest test, so `-R` takes test names such as
`ClientServer.RecoversAfterServerStarts`. A failing test prints its output. Each test has a 60-second
timeout, so a hung socket fails instead of stalling the run.

Each test program also runs once as a whole, as `<program> (whole program)` with the label `whole`. That is
how Qt Creator and other IDEs run it, and state one test leaves behind (a changed global setting, say) breaks
the next test only when the two share a process.

The Qt tests create an offscreen `QApplication`, so they open no windows and also run without a display.

In the `asan` build, undefined behaviour aborts the program like a memory error, so it fails the test
rather than printing a report into a passing run.

## Checking changes before a push

`scripts/check.sh` runs what continuous integration runs, on this machine:

```sh
scripts/check.sh            # clang-format, debug and asan here, and the Ubuntu job in Docker
scripts/check.sh --quick    # clang-format and debug only (about a minute)
scripts/check.sh --linux    # only the Ubuntu job in Docker
```

It prints one line per step and stops at the first failure, printing the end of that step's output; the full
output of every step is in `build/check-logs/`. clang-format must be version 22.1.8: the script finds Qt
Creator's bundled copy, a `clang-format` on `PATH`, or the one `CLANG_FORMAT` names.

The Ubuntu job needs Docker Desktop running. It builds an image from `docker/ubuntu-24.04.Dockerfile` with the
CI's packages (the first time takes a few minutes), then builds and tests a copy of the working tree in a
throwaway container, with the C locale and Qt offscreen as on GitHub's runners. Your own `build/` folders are
never touched. To look around inside the container, for example to run one failing test:

```sh
docker run --rm -it --volume "$PWD":/src:ro --entrypoint bash id-vision-lab-ubuntu
```

To run the full check before every `git push`, turn on the repository's hook once per clone:

```sh
git config core.hooksPath scripts/git-hooks   # off again: git config --unset core.hooksPath
git push --no-verify                          # skips the hook for one push
```

In Claude Code, two skills in `.claude/skills/` use the script: **`local-ci`** picks the mode for the change,
runs it, reads the logs of a failing step and reproduces the failing test; **`pr-ready`** runs the full check,
reviews the branch's changes against its base and drafts the pull request's title and description with the
measured results. Neither pushes, commits or opens a pull request unless asked.

## Working in Qt Creator

These steps are not covered by the automated tests: menu names follow Qt Creator 20.0.2 and are checked
by hand. If a menu has moved in your version, the presets above still work from the command line.

1. Start Qt Creator and choose **File → Open File or Project…**, then select the top-level
   `CMakeLists.txt`.
2. On the **Configure Project** page, tick the presets you want (for example **Debug**) and click
   **Configure Project**. Qt Creator reads `CMakePresets.json`, so the build folders are the same
   `build/<preset>` ones as on the command line.
3. Build with **Build → Build All Projects** (⌘B).
4. Run: pick `visual_debug_viewer` in the run target selector (the computer icon at the bottom left) and
   press **Run** (⌘R). Pick `pipeline_demo` and run it to send it some images.
5. Tests: open the **Test Results** pane (**View → Output → Test Results**), then **Tools → Tests → Run
   All Tests**. Qt Creator runs each GoogleTest program itself and lists the results by test suite. Unlike
   `ctest`, it runs all the tests of one program in a single process, so a test that leaves global state
   behind (or crashes) affects the tests after it, and the pane reports the program as crashed. If
   `viewer_ui_tests` crashes in `ThumbnailPanel`, see the `ImageIO` entry under Troubleshooting.
6. Debugging: click in the margin of `apps/viewer/image_view.cpp` next to the first line of
   `ImageView::wheelEvent` to set a breakpoint, start with **Debug → Start Debugging** (⌘Y), and scroll
   the mouse wheel over an image in the viewer. Qt Creator stops at the breakpoint with LLDB.

## Troubleshooting

**Qt Creator cannot find Ninja, Qt or OpenCV when started from the Dock.** Apps started from the Dock or
Finder get launchd's minimal `PATH`, which does not include `/opt/homebrew/bin`. The presets add it to
`PATH` themselves, so this only happens when configuring without a preset. Use a preset, or start Qt
Creator from a terminal.

**A test or the viewer crashes in `ImageIO` (`SIGBUS`) only when started from Qt Creator.** Qt Creator's
run setting **Add build library search path to DYLD_LIBRARY_PATH and DYLD_FRAMEWORK_PATH** puts
`/opt/homebrew/lib`, where Homebrew's OpenCV, ZeroMQ and Qt live, on `DYLD_LIBRARY_PATH`. macOS then loads
Homebrew's `libpng`, `libjpeg`, `libtiff` and `libgif` in place of ImageIO's own, and drawing a colour emoji
(an image name such as `ž😀`, as in `ThumbnailPanel.LongNameKeepsCellSize`) crashes. The executables find
their libraries through their rpath and need no search path, so turn it off in two places:

1. **Preferences → Build & Run → General**: untick **Add linker library search paths to run environment**.
   Run configurations Qt Creator makes from then on start without the search path.
2. Run configurations it has already made keep their own setting. In **Projects → Run Settings**, pick each
   in turn (the viewer, the example sender and each test program, for every preset you configured) and
   untick **Add build library search path to DYLD_LIBRARY_PATH and DYLD_FRAMEWORK_PATH**, just below **Run
   in terminal**.

`ctest` and Finder never set it.

**`Could NOT find WrapVulkanHeaders` during configure.** Qt looks for optional Vulkan support. Harmless;
nothing in this project uses Vulkan.

**`[ WARN:0@…] global loadsave.cpp … findDecoder imread_(…): can't open/read file`.** OpenCV's own log
line when a file cannot be read. The program reports the failure itself as well. Set
`OPENCV_LOG_LEVEL=ERROR` to hide these lines.

**AddressSanitizer reports `container-overflow` inside GoogleTest, OpenCV or Qt.** These Homebrew
libraries are not built with AddressSanitizer, which makes this check report false positives.
`cmake/sanitizer_defaults.cpp` turns that one check off for every executable in the `asan` build; leave
it in place.

**`Operation not permitted` while building under `~/Documents`.** Antivirus folder protection, such as
Bitdefender's Safe Files, which guards `~/Documents`, blocks a tool it does not trust from writing there. The
error names the file, not the antivirus. Add the tool to the antivirus's trusted applications only if its
path is in Homebrew (`/opt/homebrew`), the Xcode Command Line Tools (`/Library/Developer/CommandLineTools`)
or this project's `build` folder; otherwise, find out what the tool is first.

**`live_sender was built without FFmpeg, so --hdr is not available`.** CMake did not find FFmpeg's
development files when it configured. Install them (`brew install ffmpeg`, or Ubuntu's four `-dev` packages
above), check that `pkg-config --modversion libavformat` answers, and configure again.

**`ld: symbol(s) not found` after adding a source file.** Add the file to the target's `add_library` or
`add_executable` list in its `CMakeLists.txt`; CMake does not pick up new files by itself.

## Using the library in your own project

See [Link the library from your own CMake project](user-guide.md#6-link-the-library-from-your-own-cmake-project)
in the user guide.

## Linux

Continuous integration builds and tests every push on Ubuntu 24.04 with the distribution's packages, which
give OpenCV 4.6, Qt 6.4 and FFmpeg 6.1, the oldest versions supported. FFmpeg's four development packages
(`libavcodec-dev`, `libavformat-dev`, `libswscale-dev`, `libavutil-dev`) are among them, so its tests run there
too, decoding in software: CI has no GPU. The list lives in
[`scripts/install-ubuntu-deps.sh`](../scripts/install-ubuntu-deps.sh), which CI and the Docker image both run;
on an Ubuntu 24.04 machine:

```sh
sudo scripts/install-ubuntu-deps.sh
```

Then build with the presets as above. With Qt older than 6.8 the **View → Appearance** menu is absent. The
presets add `/opt/homebrew/bin` to `PATH`, which is harmless on Linux because a missing folder on `PATH` is
ignored. The apps are then plain executables rather than bundles, under the same names:
`build/debug/apps/viewer/visual_debug_viewer` and
`build/debug/apps/example_sender/visual_debug_example_sender`. Their windows show their icons, but there is
no desktop launcher for them yet.

## Windows

Verified on Windows 11 Pro (x64, Lenovo Legion 5), Visual Studio Community 2022 17.14 (MSVC 19.44), November
2026. The C++ libraries come from [vcpkg](https://vcpkg.io) in manifest mode (`vcpkg.json` in the repository
root pins every version); Qt comes from Qt's own installer. [ADR-002](ADR/ADR-002-windows-dependencies.md)
explains these choices.

### Prerequisites (Windows)

1. **Visual Studio 2022** (Community is enough) with the **Desktop development with C++** workload. It brings
   MSVC, the Windows SDK, CMake and Ninja. Check that the workload's optional **C++ CMake tools for Windows**
   is ticked; it is by default.
2. **Git for Windows**, which also gives Git Bash, used for `scripts/check.sh`.
3. **Qt 6** from the [Qt Online Installer](https://www.qt.io/download-qt-installer) (a free Qt account is
   needed). Under **Qt → Qt 6.12.0** (or newer) tick **MSVC 2022 64-bit**; under **Developer and Designer
   Tools**, Qt Creator is ticked by default. Nothing else is needed.
4. **vcpkg**, cloned to a short path with no spaces, then bootstrapped:

   ```powershell
   git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
   C:\dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics
   ```

   Visual Studio ships its own copy of vcpkg, which the presets do not use: a clone of your own is updated
   when you choose, not with Visual Studio, and is the same kind of vcpkg that CI uses.
5. **Long paths.** vcpkg's build trees nest deeply. Turn on Windows' long path support once, from an
   administrator PowerShell, then restart:

   ```powershell
   New-ItemProperty -Path HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem -Name LongPathsEnabled -Value 1 -PropertyType DWORD -Force
   ```

   Keep the repository on a short path too, such as `C:\dev\id-vision-lab`.
6. **Two user environment variables**, which the Windows presets read. Set them once, then open a new
   terminal so it sees them:

   ```powershell
   [Environment]::SetEnvironmentVariable('VCPKG_INSTALLATION_ROOT', 'C:\dev\vcpkg', 'User')
   [Environment]::SetEnvironmentVariable('QT_ROOT_DIR', 'C:\Qt\6.12.0\msvc2022_64', 'User')
   ```

   Why not `VCPKG_ROOT`, which vcpkg's own documentation uses: the Visual Studio developer environment sets
   `VCPKG_ROOT` to Visual Studio's bundled copy, overwriting yours, every time it starts.
   `VCPKG_INSTALLATION_ROOT` is left alone, and GitHub's Windows runners already set it.
7. **Line endings.** Git for Windows turns on `core.autocrlf` system-wide, which would check files out with
   CRLF line endings; clang-format and the diffs would then disagree with CI. The repository's
   `.gitattributes` asks for LF everywhere, so a fresh clone is right. A clone made before `.gitattributes`
   existed keeps its CRLF files: commit or stash your work first, then check every file out again from the
   repository root:

   ```powershell
   git config core.autocrlf false
   git rm --cached -r -q .
   git reset --hard HEAD
   ```

   `git reset --hard` throws away uncommitted changes, which is why your work must be committed first.

Versions this was verified with (vcpkg's, from the baseline in `vcpkg.json`):

| Package | Version |
|---|---|
| Visual Studio 2022 | 17.14 (MSVC 19.44; its CMake 3.31.6 and Ninja 1.12.1) |
| Qt | 6.12.0, MSVC 2022 64-bit |
| Qt Creator | 20.0.2 |
| Git for Windows | 2.45.1 |
| `opencv4` | 4.14.0 (vcpkg has no OpenCV 5 yet), with its FFmpeg and Media Foundation video backends |
| `ffmpeg` | 9.0.2 (LGPL build: `avcodec`, `avformat`, `swscale`) |
| `zeromq` / `cppzmq` | 4.3.5 / 4.11.0 |
| `nlohmann-json` | 3.12.0 |
| `gtest` | 1.18.0 |
| `pkgconf` | 3.0.3 (how CMake finds FFmpeg) |

### The developer environment

MSVC works only inside the Visual Studio developer environment, which puts `cl`, the Windows SDK, CMake and
Ninja on `PATH`. In PowerShell, enter it for 64-bit builds with:

```powershell
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64"
```

Without `-arch=x64` it sets up the 32-bit compiler, which does not match vcpkg's 64-bit libraries. It may print `vswhere.exe is not recognized`, which
is harmless.

### Building on Windows

Windows has presets of its own; `cmake --list-presets` shows only these there, and only the others on macOS
and Linux:

| Preset | Build type | Folder |
|---|---|---|
| `windows-debug` | Debug | `build\windows-debug` |
| `windows-release` | Release | `build\windows-release` |
| `windows-asan` | Debug, with AddressSanitizer | `build\windows-asan` |

From the repository root, inside the developer environment:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

**The first configure takes a long time**: vcpkg builds OpenCV, FFmpeg and the other libraries from source
(21 minutes on the Legion 5, most of it FFmpeg and OpenCV). It keeps what it built in its binary cache, under
`%LOCALAPPDATA%\vcpkg\archives`, so the next configure, of either preset or of a fresh clone, takes about 20
seconds. vcpkg builds the debug and release libraries together.

The build has no warnings under MSVC's `/W4 /permissive-`. On this machine all 797 tests pass, in 212 seconds (debug),
172 seconds (release) and 332 seconds (`windows-asan`); one, `ClientServer.RefusesNonLoopbackBindByDefault`, is skipped on purpose. To
build without Qt, as on the other platforms:

```powershell
cmake --preset windows-debug -B build\no-qt -DID_VISION_LAB_BUILD_QT_APPS=OFF
cmake --build build\no-qt
ctest --test-dir build\no-qt               # the 395 tests that need no Qt
```

### Running on Windows

vcpkg copies the DLLs each program needs next to it, but not Qt's: put Qt's `bin` folder on `PATH` first
(in a new terminal; the developer environment is not needed to run):

```powershell
$env:PATH = "$env:QT_ROOT_DIR\bin;$env:PATH"
build\windows-debug\apps\visual_debug_viewer.exe
build\windows-debug\examples\pipeline_demo\pipeline_demo.exe
build\windows-debug\examples\live_sender\live_sender.exe --pattern
build\windows-debug\apps\visual_debug_example_sender.exe
```

The apps land in `build\<preset>\apps\`, not in `apps\viewer\` and `apps\example_sender\` as on macOS and
Linux: Qt's CMake setup sends every executable of the `apps` folder there on Windows. The examples and tests
land where they do on the other platforms, as `.exe` files. Every option in the
[user guide](user-guide.md) works the same, for example `visual_debug_viewer.exe --endpoint
tcp://127.0.0.1:7140`, and `--help` prints to the terminal.

The debug build of OpenCV logs `[ INFO:0@…] … libraryLoad load … opencv_core_parallel_… => FAILED` lines on
start: it looks for optional parallel back-ends that vcpkg does not build. Harmless; the release build is
quiet, and `OPENCV_LOG_LEVEL=WARNING` hides them.

**File names with letters outside English.** The apps open files through Qt, which uses Windows' UTF-16
calls. The examples hand paths to OpenCV, which opens files by their narrow (`char`) name, so each example
(and the tests that read files through them) carries a manifest, `cmake/utf8_code_page.manifest`, that makes
UTF-8 its code page: its `argv` then arrives as UTF-8 and a path such as `obrázok č.1.png` opens. This needs
Windows 10 version 1903 or newer; on older Windows such a path is not found.

### AddressSanitizer on Windows

The `windows-asan` preset builds with MSVC's AddressSanitizer (`/fsanitize=address`). MSVC has no
UndefinedBehaviorSanitizer, so this preset finds memory errors only; the macOS and Linux `asan` presets cover
undefined behaviour. Inside the developer environment, which puts the sanitizer's runtime DLL on `PATH`:

```powershell
cmake --preset windows-asan
cmake --build --preset windows-asan
ctest --preset windows-asan
```

The programs it builds run only from that environment, or with Visual Studio's
`VC\Tools\MSVC\<version>\bin\Hostx64\x64` folder on `PATH`. `cmake/sanitizers.cmake` explains the flags it
changes: Visual Studio's `/RTC1` checks are off (they cannot be combined with the sanitizer), incremental
linking is off, and the container annotations are off, because vcpkg's libraries are built without the
sanitizer and an annotated `std::vector` passed between the two would be reported falsely.

### Packages for Windows

`cpack` makes two packages of the two apps from a `windows-release` build: a portable `.zip` and an `.msi`
installer. The `.msi` needs the [WiX Toolset](https://wixtoolset.org/) **version 3**, which CPack's `WIX`
generator uses by default (checked with WiX 3.10.4); its installer sets the `WIX` environment variable CPack
looks for. Inside the developer environment:

```powershell
cmake --build --preset windows-release
cpack --config build\windows-release\CPackConfig.cmake -B build\windows-release\packages
```

Both packages hold the same `ID Vision Lab` folder: `bin\` with the two apps, the Qt and vcpkg DLLs they load
and the Visual C++ runtime; `plugins\` with Qt's platform, style and image-format plug-ins; and `licenses\`
with the project's licence, `THIRD-PARTY-NOTICES.txt` naming every library inside and its licence, the GNU
licences Qt is used under, and each vcpkg library's own copyright file. Neither needs Qt, vcpkg or Visual
Studio on the computer it runs on. The `.zip` runs from wherever it is unpacked. The `.msi` installs into
`C:\Program Files\ID Vision Lab` for everyone on the computer (so it asks for administrator rights), adds
**Visual Debug Viewer**, **Visual Debug Example Sender** and an uninstaller to the Start menu under
**ID Vision Lab**, and is removed from **Settings → Apps** like any other program. The viewer's settings and
saved images belong to each user, not to the install, so uninstalling keeps them. `cmake/windows_packaging.cmake` lists what is left out and why.

### Checking changes on Windows

`scripts/check.sh` runs the macOS and Linux presets, so on Windows check formatting and the Ubuntu job
separately. clang-format must be 22.1.8, which Qt Creator bundles (Visual Studio's copy is older and formats
a few constructs differently). From the repository root, in PowerShell:

```powershell
& "C:\Qt\Tools\QtCreator\bin\clang\bin\clang-format.exe" --dry-run --Werror (git ls-files '*.cpp' '*.h')
```

The Ubuntu job, with Docker Desktop running, from Git Bash:

```sh
scripts/check.sh --linux
```

Do not build in the same clone while it runs: it copies the working tree into the container, and a file
that changes during the copy fails it with `tar: .: file changed as we read it`.

Continuous integration's `windows` job (in `.github/workflows/ci.yml`) builds and tests the `windows-debug`
preset on GitHub's `windows-2025` runner. It checks vcpkg out at the manifest's baseline, installs Qt with
`aqtinstall`, and caches both Qt and vcpkg's built libraries, so only its first run builds OpenCV and FFmpeg.

### Qt Creator on Windows

Checked with Qt Creator 20.0.2 on Windows 11, November 2026. Qt Creator reads `CMakePresets.json`, sets up
MSVC's environment itself, and reads `VCPKG_INSTALLATION_ROOT` and `QT_ROOT_DIR` from your user environment
(start it after setting them).

1. **File → Open File or Project…**, then the top-level `CMakeLists.txt`.
2. On the **Configure Project** page, tick **Windows Debug (MSVC, vcpkg)** and **Windows Release (MSVC,
   vcpkg)**, and click **Configure Project**. They build into the same `build\windows-debug` and
   `build\windows-release` folders as the command line.
3. Build, run `visual_debug_viewer` and run the tests as on macOS (steps 3 to 5 of
   [Working in Qt Creator](#working-in-qt-creator), with Ctrl in place of ⌘). The **Test Results** pane
   reports every test passing except 1 skipped (`ClientServer.RefusesNonLoopbackBindByDefault`) and 2
   disabled: Qt Creator lists GoogleTest's cases, not CTest's seven whole-program runs, so its total is
   CTest's less those 7.

**`LNK1104: cannot open file 'apps\visual_debug_viewer.exe'`.** The viewer (or the example sender) from that
build is still running, and Windows does not let a running program's file be replaced. Quit it and build
again.

### Troubleshooting on Windows

**vcpkg builds with Visual Studio's bundled copy, or reports a baseline it cannot find.** The configure used
`VCPKG_ROOT`. Check that `VCPKG_INSTALLATION_ROOT` is set in this terminal (`$env:VCPKG_INSTALLATION_ROOT`);
if you set it after opening the terminal, open a new one. Delete `build\<preset>` and configure again.

**The first configure stops for many minutes at `Downloading nasm`.** nasm.us sometimes serves nothing at
all. vcpkg keeps a mirror: download the file vcpkg names (for example `nasm-3.01-win64.zip`) from
`https://vcpkg.github.io/assets/nasm/`, check its SHA-512 against the one in vcpkg's message, put it in
`<vcpkg>\downloads\`, and configure again.

**`Could not find a package configuration file provided by "Qt6"`.** `QT_ROOT_DIR` is not set in this
terminal, or does not point at the `msvc2022_64` folder.

**A program exits at once with no window and no message** (exit code 53 in PowerShell). It cannot find Qt's
DLLs: put `$env:QT_ROOT_DIR\bin` on `PATH` as shown above.

**`CMake Warning (dev) at …/share/ffmpeg/FindFFMPEG.cmake:45` (`The NAMES keyword was followed by an empty
string`), twice.** From vcpkg's FFmpeg package, which OpenCV loads. Harmless: the next lines say
`Found FFMPEG` and `Found OpenCV`, and the configure says nothing about `live_sender --hdr`, which it does
only when it cannot find FFmpeg.

**`Could NOT find WrapVulkanHeaders`.** As on macOS: harmless.

**A UI test fails because a widget is narrower or wider than expected.** Qt's offscreen platform, which the
tests use, finds fonts only in `QT_QPA_FONTDIR`, and Qt's Windows install has none of its own. The tests'
`main()` points it at `%WINDIR%\Fonts` and uses Segoe UI 9 pt, the Windows default, so the layout matches a
real window. If you set `QT_QPA_FONTDIR` yourself, it must hold Segoe UI.
