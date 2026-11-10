# CLAUDE.md — ID Vision Lab

A 2013 thesis project revived in 2026, public under Apache-2.0: the `visual_debug` library sends a
`cv::Mat` over ZeroMQ to the Visual Debug Viewer, a separate Qt 6 app with zoom, history and pixel readout;
the Visual Debug Example Sender is a small Qt app that shows how to use the library; the Document Lab is
the workbench for document-reading code (a camera or a replayed video, image, folder or recording through a
processor, drawn on the viewer's canvas, recorded, captured and optionally sent to the viewer). Read the
linked documents rather than re-deriving them:

- [`docs/architecture.md`](docs/architecture.md): components, dependency rule, threads, transport, extension points
- [`docs/wire-format.md`](docs/wire-format.md): the message format (authoritative)
- [`docs/build-guide.md`](docs/build-guide.md): tools, presets, targets, tests, Qt Creator, troubleshooting
- [`docs/user-guide.md`](docs/user-guide.md): running the programs, `SendResult` meanings, worked examples
- [`docs/ADR/`](docs/ADR/): decision records

## Working rules

- **Never commit, push or merge** unless the owner asks in that request. Leave changes in the working tree.
- **One simple command per Bash call**: no `&&`, `;` or pipes; absolute paths instead of `cd`.
- Test-first for library code: write the failing GoogleTest, watch it fail, then implement.
- `docs/superpowers/` (specs and plans) is git-ignored; nothing committed may link into it.

## Accuracy

- Check before stating: that an API exists, with that signature, in the versions this project supports
  (OpenCV 4.6 and 5.x, Qt 6.4 and newer); a tool's or action's current version. Say what you could not check.
- Numbers in docs, commits and reports come from a run in this session. Never present an estimate as a
  measurement.
- No test counts or timings in Markdown files, CLAUDE.md or skills: they change with every new test and with how
  busy the machine is, so they drift. What matters is that everything builds, runs and passes, which CI and
  `scripts/check.sh` show. A commit message or pull request may name the tests a change adds.
- Say what was not done: tests not run, platforms not built, steps skipped.
- Look facts up yourself; ask the owner only for decisions, with a recommendation.

## Commands

```sh
cmake --preset debug                  # also: release, asan, coverage
cmake --build --preset debug
ctest --preset debug                  # all tests; -R <regex> to filter
ctest --preset asan                   # sanitizers; undefined behaviour aborts the test
cmake --preset debug -B build/no-qt -DID_VISION_LAB_BUILD_QT_APPS=OFF   # proves the libraries need no Qt
scripts/check.sh                      # what CI runs, here: clang-format, debug, asan, Ubuntu in Docker
scripts/coverage.sh                   # coverage preset: line and function coverage per library and app
python/.venv/bin/python -m pytest python/tests   # the Python sender (venv setup: build guide)
python/.venv/bin/black --check python            # black 24.2.0, Ubuntu 24.04's, as CI runs it
```

On Windows the presets are `windows-debug`, `windows-release` and `windows-asan` (MSVC, vcpkg from
`vcpkg.json`, Qt from `$env:QT_ROOT_DIR`), run inside the VS x64 developer environment (it also provides `cmake`
and `ninja`, and it overwrites `VCPKG_ROOT` with Visual Studio's bundled vcpkg, which is why the presets read
`VCPKG_INSTALLATION_ROOT`); `scripts/check.sh
--linux` from Git Bash covers the Ubuntu job, and clang-format is Qt Creator's copy. `cpack --config
build\windows-release\CPackConfig.cmake -B build\windows-release\packages` makes the `.zip` and the `.msi`
(WiX 3) from `cmake/windows_packaging.cmake`; licences shipped with them are in `packaging/licenses/`. Setup and commands:
[`docs/build-guide.md#windows`](docs/build-guide.md#windows).

Before a push or a pull request, run `scripts/check.sh` (the `local-ci` skill); to prepare a pull request,
use the `pr-ready` skill.

The apps build as `build/<preset>/apps/viewer/visual_debug_viewer`,
`build/<preset>/apps/example_sender/visual_debug_example_sender` and `build/<preset>/apps/document_lab/document_lab`
(`.app` bundles on macOS; on Windows `build\<preset>\apps\visual_debug_viewer.exe`,
`visual_debug_example_sender.exe` and `document_lab.exe`, without the subfolder). Their shown names live in
`apps/qt_common/application_identity.h` and `MACOSX_BUNDLE_BUNDLE_NAME`, and also name their settings and data
folders: renaming an app moves them. The Document Lab's bundle has its own `apps/document_lab/Info.plist.in`
(`NSCameraUsageDescription`, Continuity Camera): macOS ends a camera app without it. Start it with `open` on
macOS, so the camera prompt names the Document Lab rather than the terminal.

## Layout

`libs/visual_debug` (transport, no Qt) · `libs/viewer_core` (viewer logic, no Qt) · `libs/frame_io` (videos,
image folders, recordings and captures; YUV conversion; no Qt) · `libs/lab_core` (the Document Lab's engine,
processors and sender; no Qt) · `apps/viewer` · `apps/example_sender` · `apps/document_lab` · `apps/qt_common`
(what the apps share, the canvas among it: `ImageView`, `LiveConverter`, `live_drawing`, `overlay_painter`) ·
`examples/{pipeline_demo,minimal_sender,overlay_sender,live_sender}` ·
`python/` (the Python sender: `visual_debug.py`, its pytest tests, `pyproject.toml`, a demo) ·
`tests/{visual_debug,viewer_core,frame_io,lab_core,qt_common,viewer,example_sender,document_lab,examples}`
(`tests/qt_common` also holds the shared Qt test `main()` and helpers) · `tests/wire/header_cases.json` (golden
headers both senders must write) · `cmake/` (warnings, sanitizers, coverage, app icons: macOS `.icns`, Windows `.ico` via `make_ico.cpp`; Windows UTF-8
manifest and packaging) · `packaging/licenses/` (third-party notices for the Windows packages) · `scripts/`
(`check.sh`, `coverage.sh`, the Ubuntu package list, the pre-push hook) · `docker/` (the CI's Ubuntu job as a container) ·
`.claude/skills/` (`local-ci`, `pr-ready`, `authorised-delivery`). Older commits use former names (`Image_processing`, Slovak
identifiers): [`docs/former-names.md`](docs/former-names.md).

## Rules that keep it working

- **The libraries stay Qt-free.** No Qt include or link in `libs/`. Automatic moc/uic/rcc is enabled in
  `apps/CMakeLists.txt` only. The no-Qt configure above must keep building.
- **Threading:** `ImageSink::onImageReceived` runs on the server's worker thread. `QtImageSink` hands images
  to the UI thread by queued signal; the UI must call `markHandled()` for every image it takes, or the sink
  fills its eight slots and the viewer reports busy. Live frames (`sendLive`) never wait and skip those
  slots: the sink keeps each channel's newest frame and the UI takes them all with `takeLiveFrames()`;
  live frames and snapshots never reach `StorageController`. The live channel in view is converted, and its
  histogram counted, on `LiveConverter`'s one thread (`apps/qt_common/live_converter.h`): a job copies or
  shares read-only all it needs, a newer job replaces the waiting one, and a result is applied only if its
  channel and the window's view counter still match. `viewer_core`'s classes (`LiveChannels`,
  `ImageHistory`, `DisplaySettingsStore`) are UI-thread only and have no locks; its free functions
  (`toDisplayImage`, `valueExtent`, `rangeMarkers`, `histogramOf`) keep no state and also run on the
  converter's thread. Snapshots' history pictures are made on `SnapshotConverter`'s thread, in a queue that
  never drops one. Window tests use `LiveConversion::Immediate`, which makes both immediate, unless they test
  the threads.
- **The Document Lab's threads:** the UI thread does no per-frame work beyond checking the camera session,
  stamping the time and handing the `QVideoFrame` to the `Engine` in a closure. The engine's one processing
  thread converts it (`toFrame`), paces and reads replays, runs the `FrameProcessor` and pushes to the
  recorder and the `ResultSender`; the window talks to it only through its mailbox of commands, and every wait
  there wakes for a command. Each run has a generation: the window drops results of an older one, and of any
  run once stopped. A camera frame waits in a slot of one (a newer replaces it); a replay never skips a frame
  (`ReplayPacer`); the recorder never drops a replay's frame (it waits for room) but counts a camera frame it
  has no room for as not recorded. Hooks fire with no lock held, and nothing thrown leaves the processing
  thread (a catch-all stops the run). A seek is a request under the lock that newer ones replace or add to, one
  command posted while it waits; a video seeks on its read-ahead thread, and a newer seek, a restart or the
  source's end abandons it between frames. The `ViewerLink` (pings, Send Frame) has its own sender on its own
  thread, beside the stream's `ResultSender`: a ZeroMQ REQ socket serves one thread. ■ ends the lab's channels
  on the retiring sender's thread, and ▶ waits for `endingChannels` to clear. The canvas takes the viewer's glue
  (view counter, `LiveConverter` jobs numbered by result). Engine, link and window tests use the immediate modes,
  `pump()` and an injected clock unless they test the threads; window tests give the link and the stream fake
  senders, so no test reaches a real viewer.
- **Wire-format changes:** a new optional header field keeps `version` 2, and so does a new message `type` (an
  older viewer refuses it with `unsupported message type`, which still answers); changing or removing a field
  bumps it. Update `docs/wire-format.md` and `docs/user-guide.md` in the same change, and the Python sender
  (`python/visual_debug.py`) with them: both encoders must write `tests/wire/header_cases.json`'s golden
  headers, and the Python side is checked only by the venv's pytest and CI's Ubuntu job, not by ctest.
- **Security:** no exception ever leaves `visual_debug` (every send returns a `SendResult`); reply reasons
  are fixed ASCII and never echo request bytes; the header is parsed without exceptions and never copied
  or dumped (a deeply nested header would overflow the worker's stack); the server binds loopback only
  unless `allowRemote` is set.
- **Portability** (macOS, Linux and Windows): no POSIX- or GNU-only APIs; paths through `std::filesystem` or
  Qt; production code reads environment variables only through `visual_debug::defaultEndpoint()` (tests
  through `test_support::environmentValue`: MSVC warns on `getenv`; in Python only `default_endpoint()`
  reads `VISUAL_DEBUG_ENDPOINT`, and `python/tests/conftest.py` points it at an unused port for every test); non-ASCII source is UTF-8 (MSVC gets
  `/utf-8`); OpenCV API must also exist in 4.x (OpenCV 5 moved some headers: guard them with
  `__has_include`). On Windows `cv::imread`/`imwrite` and `cv::VideoCapture` take narrow paths, so in the
  viewer and the example sender a non-ASCII path goes through Qt and `cv::imdecode`/`imencode`, while
  `frame_io`'s users (the Document Lab, `media_smoke`, `live_sender`), the other examples and their tests get a
  UTF-8 code page from `id_vision_lab_use_utf8_code_page()` and pass paths as UTF-8 (`u8string()`; the lab
  makes them from `QString` through UTF-16); under it `std::filesystem::path` throws on bytes that are not
  UTF-8, so split names from raw strings yourself (`live_sender::fileNameOf`); Qt's permission bits do not reach
  access lists, so the apps set their folders' lists in `apps/qt_common/private_files.cpp` and privacy tests
  use `tests/qt_common/owner_only.h`; those two `.cpp` files are the only ones that include `<windows.h>`.
  `frame_io` sets owner-only bits on the files it writes; on Windows they inherit the folder's list, so the
  lab makes its recording and capture folders private through `private_files` before writing into them.
- **Warnings:** zero from project code under `-Wall -Wextra -Wpedantic`, and under MSVC's `/W4`.
- **UI tests run offscreen in the Fusion style**, so they cannot see macOS drawing (a `QToolBar`'s buttons
  lose their frames) or how a live appearance switch reaches child widgets (only as `PaletteChange`). Check
  such changes in the running app too. On Windows the offscreen platform has no fonts of its own:
  `tests/qt_common/test_main.cpp` points it at `%WINDIR%\Fonts` and Segoe UI 9 pt, or text-sized layouts
  come out wrong.

## Conventions

Files `snake_case.h/.cpp`; classes `PascalCase`; functions `camelCase`; members `m_`; constants
`kPascalCase`; namespaces lower case; tests `<unit>_test.cpp`. British English in identifiers, comments, UI
text and docs (`normalise`, `colour`); third-party names keep theirs (`cv::COLOR_BGR2RGB`). New-style Qt
`connect`. No `using namespace` in headers. Comments explain why. `.clang-format`: LLVM base, 4 spaces,
110 columns, pointers left; CI checks it with clang-format **22.1.8** exactly (Qt Creator's bundled copy,
which `scripts/check.sh` finds); other versions format a few constructs differently.
