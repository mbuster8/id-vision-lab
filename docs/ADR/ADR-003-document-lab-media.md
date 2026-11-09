# ADR-003: The Document Lab's media stack

**Date:** 2026-11-09
**Status:** Accepted

## Context

The Document Lab is the desktop workbench for document reading (ProAuthenticator's roadmap, step A2): a camera
or a recorded video goes through processing code, and the results show over the frames. It has to:

- open cameras on macOS (Continuity Camera included), Windows and Linux, with the platform's camera permission;
- replay video files (an iPhone's 10-bit HDR HEVC among them), single images, folders of images and its own
  recordings, every frame in order;
- record frames losslessly, and keep a captured frame at full resolution;
- draw frames at camera rates without blocking the window, with the viewer's zoom, pan and overlays;
- send its frames to the Visual Debug Viewer when asked;
- keep `libs/` free of Qt, so later steps' processing and tests run without it.

Options considered, by concern:

| Concern | Options | Evaluation |
|---|---|---|
| Cameras | Qt Multimedia (`QCamera`, `QMediaCaptureSession`, `QVideoSink`) | One API on all three platforms (AVFoundation, Media Foundation or FFmpeg, GStreamer), the platform's permission prompt from Qt 6.5, and Continuity Camera through the bundle's Info.plist. Frames arrive as `QVideoFrame`s in the camera's own pixel format. |
| | OpenCV's `VideoCapture` | No permission prompt, no hot-plug list, no camera names on macOS, and no say over the pixel format. |
| Video files | frame_io's readers: OpenCV's videoio (AVFoundation first on macOS), FFmpeg's libraries for 10-bit and HDR | Already built and measured for `live_sender`; the same code runs in the lab's tests without Qt. |
| | Qt Multimedia's `QMediaPlayer` | Plays at the clock's pace and drops frames to keep up; no frame-exact stepping, no 16-bit frames. |
| Recordings | A folder of PNGs and a `recording.json` | Lossless at 8 and 16 bits, written in parallel by several threads, readable frame by frame, and a crash leaves every frame written so far. A 1920×1440 frame takes 52.4 ms and 2.92 MB with OpenCV's default PNG settings, measured on this project's Mac with real C920 frames; four writer threads write 70.0 frames/s, six 103.9. |
| | A lossless video (FFV1) | One file and smaller, but a crash loses the file's end, writing is one stream, and stepping back needs seeking. |
| The canvas | The viewer's `ImageView` and `LiveConverter`, moved to `qt_common` | The same zoom, pan, overlays and off-thread conversion, tested once. |
| FFmpeg on Windows | vcpkg's build for frame_io and Qt Multimedia's FFmpeg plug-in alike | One FFmpeg build per process; two builds in one process clash. |
| | Qt's own FFmpeg for the plug-in, vcpkg's for frame_io | Two builds of the same DLL names in one process. |

## Decision

1. **Cameras through Qt Multimedia**, in the app only (`apps/document_lab`). Each frame is converted by its
   own pixel format, never the chosen format's, with the matrix and range its format names; the mirroring flags
   are left alone, and only darwin's unflagged front-camera mirroring is undone.
2. **Files through `frame_io`**, a Qt-free library: OpenCV's videoio for 8-bit video (AVFoundation first on
   macOS, kept only when it reads a first frame), FFmpeg's libraries for more than 8 bits or HLG and PQ, image
   files and folders through imgcodecs, recordings through their `recording.json`.
3. **Processing in `lab_core`**, a Qt-free library: the `FrameProcessor` hook, the processors, the engine that
   moves frames through them on one thread, and the sender.
4. **Recordings are PNG folders** with a `recording.json`, written by six threads from a queue bounded in
   bytes, readable by their owner only.
5. **The canvas is the viewer's**, moved to `qt_common`.
6. **On Windows, vcpkg's FFmpeg only**: its five DLLs are copied next to the lab's programs and installed into
   the packages, and windeployqt is told to deploy none of Qt's (`--no-ffmpeg`) and not the deprecated Windows
   Media Foundation backend.

## Consequences

- Positive: the lab's file reading, processing, recording and sending are tested without Qt, cameras or
  threads, through immediate modes and injected clocks.
- Positive: one canvas, one set of overlays and one off-thread conversion for both apps.
- Negative: a recording of a 4K camera fills a disk fast; recording stops below 5 GB free.
- Negative: on Windows the packages need Qt 6.7 or newer for windeployqt's options.
- Negative: Ubuntu's Qt 6.4 GStreamer camera waits for the window's thread for each frame; Linux is the
  secondary platform.

## Links

- `docs/architecture.md`: the components, the threads and the engine.
- `docs/user-guide.md`: running the Document Lab.
- `docs/build-guide.md`: Qt Multimedia per platform, and the FFmpeg DLLs on Windows.
- `docs/ADR/ADR-002-windows-dependencies.md`: vcpkg and the Windows presets.
