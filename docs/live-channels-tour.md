# A tour of live channels

Nine stops through the viewer's live channels, each with what it shows, the commands to run and what you
should see. Each stop builds on the one before. The [user guide](user-guide.md#live-channels) is the
reference for everything here.

The commands assume the debug build on macOS and are run from the repository root (see the
[build guide](build-guide.md)); stops 8 and 9 use the release build. Run each `live_sender` in its own
terminal window, and stop it with Ctrl+C. You need no camera until stop 7.

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

**What it shows:** a recording sent in real time.

Use any video: a recording from QuickTime Player (**File → New Movie Recording**), or a clip from a phone.

```sh
build/debug/examples/live_sender/live_sender path/to/video.mov
```

- The channels are named after the file, `video.mov` and `video.mov edges` (the first 32 characters of
  its name). The video plays at its own rate (the strip's `fps in` matches the file's) and the sender stops
  at its end with `video.mov: N sent, N delivered`.
- It keeps the video's timing even when sending cannot keep up, as with a 4K video at 60 frames a second in
  the debug build: a frame ready more than a frame late is skipped, and the summary counts them,
  `N sent, N delivered, M skipped`. The video then takes as long as it lasts, not longer. If decoding is
  what cannot keep up, skipping would not help: the video plays slower instead, every frame shown.
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

- `camera 0` and `camera 0 edges` stream at the camera's own rate, often 24 to 30 frames a second and fewer
  in dim light. The age in the info strip is a few milliseconds.
- With a second camera (an iPhone through Continuity Camera, say), run `live_sender --camera 1` in another
  terminal at the same time: `camera 1` and `camera 1 edges` join the Live group, four rows in all, each
  stream in its own.
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

**Several senders on one viewer.** Cameras and files are named after their source, so two cameras, or a
camera and a video, keep apart by themselves (stop 7). Two patterns would both send `camera` and `edges`, and
take turns in the same rows; give each a name:

```sh
build/debug/examples/live_sender/live_sender --pattern --name pattern
build/debug/examples/live_sender/live_sender --pattern --size 1280x720 --name wide
```

The Live group has four rows: `pattern`, `pattern edges`, `wide` and `wide edges`, and each summary names
its own channels. Drag the divider below the Live group down to see more rows at once, up to every channel;
the viewer remembers its height.

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

## 9. HDR video

**What it shows:** a phone's HDR video as 16-bit data, tone-mapped for this screen, with its highlights,
clipping and values readable.

Use an iPhone's HDR video (the Camera app records HEVC 10-bit HLG, Dolby Vision 8.4), and an Android phone's
if you have one. Keep the release viewer from stop 8 open:

```sh
build/release/examples/live_sender/live_sender --hdr path/to/iphone.MOV
```

- The sender prints what it found, such as
  `Horses close to Tegernsee.MOV: 3840x2160, 10-bit, HLG, BT.2020, VideoToolbox`, and at the end a summary
  that names the transfer: `Horses close to Tegernsee.MOV (HLG, BT.2020): N sent, N delivered`, with
  `M skipped` if sending fell behind. A portrait video arrives upright.
- The info strip names the frame's tags: `Horses close to Tegernsee.MOV  live · 3840×2160 · 3 ch · 16U · HLG ·
  BT.2020 · 4100–60219 · …`. The range button reads **Range: HDR ▾**, the default for an HLG or PQ channel.
- **Against QuickTime Player.** Open the same file in QuickTime Player beside the viewer and pause both on a
  similar frame (Space freezes the channel). The viewer's picture should be close to QuickTime's: skin and
  grass alike, the sky's highlights bright but not blown flat, colours not oversaturated. QuickTime shows HDR
  on a Mac's HDR screen brighter than any SDR picture can be; compare the look, not the peak brightness.
- **Auto and Fixed on the same frame.** Freeze a frame. **Range → Auto** shows the raw HLG signal stretched
  from 4100 to 60219: flatter, and every super-white is white. **Range → Fixed…** with 0 and 65535 shows the
  whole signal, nominal white a little below the screen's white and the highlights above it. **Range → HDR**
  brings back the tone-mapped picture. The readout reads the same values in all three.
- **Clipping on glare.** Choose **Image → Show Clipping** (⇧⌘K) and find glare: the sun on water, a car's
  chrome, a hologram on a card turned to the light. Under HDR, magenta marks only what the camera itself
  clipped (65535); under Auto, everything above nominal white turns magenta. Blue marks what is at or below
  black. Zoom in and out: the marks stay visible at every zoom.
- **A region over a highlight.** Shift-drag a rectangle over the glare. The label gives each channel's
  minimum, maximum, mean and σ; with the channel unfrozen they follow the video, twice a second. Esc clears
  it.
- **Load.** Watch the strip's `fps in`, `drawn` and `dropped`, the summary's `skipped`, and the viewer's CPU in
  Activity Monitor, for a 4K HDR video at 60 frames a second and, if you have one, at 30. A 16-bit 4K frame is
  about 50 MB; on an M4 Pro, `live_sender` kept a 60 fps 4K video's timing by skipping 80 to 145 of its first
  600 to 750 frames, depending on what else the Mac was doing. If `drawn` stays well below `fps in`, note it:
  converting live frames off the viewer's UI thread is then the next round's first item.
- An SDR video with `--hdr` arrives as `SDR, BT.709` on the same 16-bit scale, and the HDR range is not
  offered for it.
