# A tour of live channels

Eight stops through the viewer's live channels, each with what it shows, the commands to run and what you
should see. Each stop builds on the one before. The [user guide](user-guide.md#live-channels) is the
reference for everything here.

The commands assume the debug build on macOS and are run from the repository root (see the
[build guide](build-guide.md)); stop 8 uses the release build. Run each `live_sender` in its own terminal
window, and stop it with Ctrl+C. You need no camera until stop 7.

## 1. A test pattern

**What it shows:** a stream in place of a history, the Live group, a channel's state, and how a channel
goes idle and is closed.

```sh
open build/debug/apps/viewer/visual_debug_viewer.app
build/debug/examples/live_sender/live_sender --pattern
```

Wait for the viewer's status bar to say `Listening on [tcp://127.0.0.1:7139]` before starting the sender.

- A **Live** group appears at the top of the sidebar with two rows, `camera` and `edges`, each with its
  newest frame, `30 fps`, a dot in the accent colour and the word `live`.
- If the view was empty, `camera` takes it at once: a moving gradient with a disc crossing it and the frame
  number. If an image was in view, it stays there; click the `camera` row.
- The info strip reads `camera  live · 640×480 · 3 ch · 8U · 30 fps in · 30 drawn · 0 dropped · 2 ms`, give
  or take a frame and a few milliseconds, and the window's title names `camera`.
- The mouse wheel zooms and dragging pans while the stream runs; hovering reads the pixel under the cursor.
- Stop the sender with Ctrl+C. It prints `camera: N sent, N delivered` and the same for `edges`. Three
  seconds later both rows' dots turn grey and the rows read `idle`, and a ✕ appears on each.
- The log (**Log ▴**) has `Live channel [camera] started` and `Live channel [camera] idle after N frames
  (0 dropped)` for each channel.
- Click `camera`'s ✕. The row goes, the log adds `Closed live channel [camera]`, and the view shows the image
  you looked at before the channel, or nothing. Close `edges` too.

## 2. Freeze and Snapshot

**What it shows:** holding a frame while the stream goes on, and keeping one in the history.

```sh
build/debug/examples/live_sender/live_sender --pattern
```

- If `camera` is not in view, click its row. Press Space (or ⇧⌘T, or **Freeze** in the info strip). The
  frame stops; the row and the strip read `frozen`, and the strip's `fps in` keeps counting. The row's dot
  stays in the accent colour while frames arrive. Press Space again: the newest frame shows.
- Press ⌘T (or **Snapshot**). A thumbnail named like `camera 14:03:22.517` (the frame's arrival time) joins the
  history; the channel stays in view.
- **Edit → Copy Image** (⌘C) and **File → Save As…** (⇧⌘S) take the frame on screen; Save As suggests the name
  a snapshot would get.
- Freeze `camera` and stop the sender with Ctrl+C. Three seconds later the row reads `frozen · idle`, its ✕
  appears, and the frozen frame stays in view. Close both channels.

## 3. Range and colours

**What it shows:** how values map to the screen, for a live channel and for stills, and that thumbnails
follow.

```sh
build/debug/examples/live_sender/live_sender --pattern
```

- Click the `edges` row. The info strip has a **Colours ▾** button, as `edges` has one channel. Choose
  **Turbo**: the view and the `edges` row both turn to Turbo.
- Click `camera`: the **Colours** button goes, as `camera` has three channels.
- Back on `edges`, choose **Range → Fixed…**. The popup opens below the Range button, its right edge at the
  button's right edge, and stays inside the window even when the window is narrow. Type 0 and 510 and
  **Apply**: the edges, whose value is 255, turn from Turbo's dark red to the green in its middle.
- Press ⌘T. The snapshot's thumbnail is in Turbo with the fixed range, as on screen.
- With the sender still running, send some stills:

  ```sh
  build/debug/examples/pipeline_demo/pipeline_demo
  ```

  They are listed below the Live group without taking the view. Click `Sobel X` and choose **Colours →
  Viridis**: the view and its thumbnail turn Viridis. Run `pipeline_demo` again: the new `Sobel X` arrives in
  Viridis too, and so does its thumbnail.

Keep the sender running for stop 4.

## 4. Stills and live together, with the keyboard

**What it shows:** one list for the arrow keys, and muting a channel you do not want to see.

- Click any thumbnail, so the keyboard is on the history. Press ↑ until you reach the first thumbnail, then
  once more: `edges` shows, its row chosen. Once more: `camera`. Another ↑ does nothing. ↓ walks back down
  through `edges` and into the thumbnails; ↓ at the last thumbnail does nothing.
- Hover the `edges` row: an eye with a stroke through it appears at its right (also on the row in view).
  Click it. The row reads `muted`, its picture greys and keeps the frame it had, its rate goes, and the button
  stays visible and pressed. The log says `Muted live channel [edges]`.
- Now ↑ and ↓ skip `edges`, and clicking its row does nothing. If `edges` was in view when you muted it, the
  view went back to the image you looked at before it.
- Right-click a row: **Mute** or **Unmute**, and **Close**, which is enabled only once the channel is idle.
- Stop the sender with Ctrl+C. Three seconds later `edges` reads `muted · idle` and shows its ✕; the log gets
  an `idle after` line for `camera` only.
- Start the sender again. `edges` comes back muted, quietly: no `started` line. Unmute it with its button:
  `Unmuted live channel [edges]`, and its row shows the newest frame.

## 5. Pause and Stop

**What it shows:** what a live sender sees when the viewer stops taking frames.

```sh
build/debug/examples/live_sender/live_sender --pattern
```

- Press ⌘P (**Server → Pause**). The status bar turns amber. No frames arrive, so both rows go `idle` after
  three seconds; the channel in view stays.
- Press ⌘P again (**Resume**). The rows go `live` again, and the log has one line for the pause, such as
  `Rejected 180 requests while paused: [viewer paused]`.
- Press ⌘. (**Server → Stop**), wait a few seconds, then ⌘. again (**Start**). The channels go idle and
  come back the same way; the sender carries on throughout.
- Stop the sender. Its summary counts what was not delivered, such as
  `camera: 900 sent, 780 delivered, 30 Viewer unavailable, 90 Rejected`, and it exits with 1.

## 6. Video files

**What it shows:** a recording sent at its own frame rate.

Use any video: a recording from QuickTime Player (**File → New Movie Recording**), or a clip from a phone.

```sh
build/debug/examples/live_sender/live_sender path/to/video.mov
```

- The video plays at its own rate (the strip's `fps in` matches the file's) and the sender stops at its end
  with `N sent, N delivered`.
- On macOS, files are read through AVFoundation, the Mac's own media framework. An iPhone's HDR video
  (HEVC 10-bit HLG) plays at full speed, with colours close to QuickTime Player's; a portrait clip arrives
  upright.
- A file AVFoundation cannot read, such as a `.mkv`, still plays, through OpenCV's default reader. OpenCV
  prints one line of its own first, which you can ignore.

## 7. A camera

**What it shows:** finding a camera's number and streaming it.

```sh
build/debug/examples/live_sender/live_sender --list-cameras
```

- macOS may ask whether the terminal may use the camera; allow it.
- It prints one line per camera that opens, such as `--camera 0: 1920x1080`, and stops at the first number
  that does not. OpenCV prints two lines of its own for that number.

```sh
build/debug/examples/live_sender/live_sender --camera 0
```

- `camera` streams at the camera's own rate, often 24 to 30 frames a second and fewer in dim light. The age
  in the info strip is a few milliseconds.
- `--camera 9` (a number that does not open) prints
  `cannot open camera 9 (live_sender --list-cameras shows the numbers that open)` and exits with 2; OpenCV
  may print a line or two of its own first.

## 8. Keeping images, two viewers, several senders, and load

**What it shows:** the parts that involve more than one program.

**Keeping images.** Tick **History → Keep Images Between Sessions**, run `pipeline_demo`, and take a snapshot
of a live channel. Quit the viewer and open it again: the stills come back, the snapshots do not (live
frames and their snapshots are never written to disk). Untick the setting: the viewer asks before deleting
the stored images.

**Two viewers.** Each listens on its own endpoint, and a sender picks one:

```sh
open -n build/debug/apps/viewer/visual_debug_viewer.app --args --endpoint tcp://127.0.0.1:7140
build/debug/examples/live_sender/live_sender --pattern --endpoint tcp://127.0.0.1:7140
```

The stream appears in the second viewer only.

**Several senders on one viewer.** Give each a name, so their channels stay apart:

```sh
build/debug/examples/live_sender/live_sender --pattern --name pattern
build/debug/examples/live_sender/live_sender --camera 0 --name cam0
```

The Live group has four rows: `pattern`, `pattern edges`, `cam0` and `cam0 edges`, and each summary names
its own channels. Without `--name`, both senders would send `camera` and `edges`, and the two streams would
take turns in the same rows.

**Load.** Build the release preset (`cmake --preset release`, `cmake --build --preset release`), open its
viewer, and send large frames fast:

```sh
open build/release/apps/viewer/visual_debug_viewer.app
build/release/examples/live_sender/live_sender --pattern --size 1920x1080 --fps 60
build/release/examples/live_sender/live_sender --pattern --size 3840x2160 --fps 30
```

Run one at a time. `drawn` in the info strip should keep up with `fps in`, with `dropped` growing slowly if
at all; Activity Monitor shows the viewer's CPU. While a frame is drawn smaller than its own pixels, the
viewer shrinks it to the drawn size before colouring and drawing it; zoomed in to 1:1 or closer, it converts
the whole frame.
