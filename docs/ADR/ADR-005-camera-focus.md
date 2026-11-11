# ADR-005: The camera's own focus, reached around Qt

**Date:** 2026-11-11
**Status:** Accepted

## Context

The Document Lab must make a passport's page sharp enough to read its MRZ (ADR-004). The owner's Logitech C920
left most passport pages blurred, its own autofocus losing the page whenever it moved, and the Legion's
integrated camera never reached the Sharp gate's bar. The owner asked for the lab to focus on the page itself,
for click-to-focus as the fallback, and for a plain message when a camera cannot focus.

A spike on 11 November (the `camera_focus_probe` tool, on a branch of its own) measured what each route offers,
aggregates only, with the owner holding the passport:

- **Qt Multimedia 6.12** reports no usable focus control for any of the owner's cameras: the C920 on macOS only
  `FocusModeAuto`; the iPhone (Continuity Camera) and the Legion's integrated camera nothing (no custom focus
  point, no focus distance, no manual mode).
- **The C920's own UVC controls** (Focus, Auto and Focus (Absolute), 0 to 250 in steps of 5) answer IOKit class
  requests from a plain user-space program on macOS, from worker threads, while Qt streams and after Qt has
  stopped the camera.
- **Held still at about half the frame**, the C920's page was sharp only around positions 25 to 40. A search over
  the positions, judged by the page's sharpness, found that band in 5 trials of 5 and held page sharpness p50
  between 304 and 473; the C920's own autofocus reached p50 422 when the page stayed still for 2 s, and lost it
  as soon as the page moved.
- **The Legion's integrated camera** offers no focus control through DirectShow, and no distance brought its page
  to the bar: best p50 27 (max 34) at 60 to 70% of the frame, in good light.

Options considered:

| Option | Evaluation |
|---|---|
| Qt's `QCamera` focus API (`setFocusMode`, `setCustomFocusPoint`, `setFocusDistance`) | Portable, but offers nothing for these cameras. |
| The camera's own controls: UVC requests through IOKit on macOS, DirectShow's `IAMCameraControl` on Windows, V4L2 controls on Linux | Reaches what the hardware offers; one implementation per platform, untestable without the camera. |
| Media Foundation's or WinRT's focus controls on Windows | Reach the same driver controls as DirectShow for USB cameras, with more setup; nothing gained for the cameras at hand. |
| The camera's own autofocus only, and a banner | Simple, but the C920's autofocus does not hold a page that moves. |

## Decision

1. **The lab drives a camera's own focus where it can**, through a `FocusControl` made per camera run from Qt's
   camera id (`apps/document_lab/camera_focus*.cpp`): IOKit UVC requests on macOS (`DeviceRequestTO`, bounded by
   time limits), DirectShow's `IAMCameraControl` on Windows. Linux gets the banner only until a Linux camera can be
   tested (V4L2 later).
2. **The decisions are Qt-free and tested without hardware**: `lab_core::FocusController`, run by the engine on
   its processing thread after each camera frame's processor, judged by the frames' own times; native calls only
   on that thread.
3. **The camera's own autofocus stays the default.** The lab searches only when a steady page stays blurred, holds
   the best position while the page stays, and puts back what it found whenever it lets go.
4. **One Sharp bar for every camera** (fails under 50, passes again from 55): a camera that cannot reach it is
   told so, with advice that fits the case, rather than given a lower bar.
5. **A third file may include `<windows.h>`**: `camera_focus_windows.cpp`, through `<dshow.h>`.

## Consequences

- The C920 on macOS focuses on the page by itself, and on a click; a camera the lab cannot focus says so.
- Three native implementations to keep, two of them reachable only on hardware; the descriptor bytes a USB camera
  sends are parsed as untrusted input (`findCameraTerminal`), and its requests are bounded in time on macOS.
- UVC settings outlast the program: a crash of the lab while it holds the focus leaves the camera in manual focus
  until it is unplugged.
- DirectShow focus is compiled on Windows CI, where only its factory is tested; it has been checked against no
  camera that offers focus.
