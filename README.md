# ORB_Recorder

A dime-sized always-on-top orb that records your screen to **GIF** or **MP4
with sound**, records a **camera**, takes **screenshots**, and views **every
image format your machine has a decoder for**.

![ORB_Recorder recording its own right-click menu](media/orb_records_its_own_menu.gif)

*The orb recording itself.* It is mid-MP4 — magenta, `REC` — while its own
right-click menu is open on top of it. A popup menu is a modal loop that parks
the program that opened it; the capture runs straight through it anyway.
[Full quality](media/orb_records_its_own_menu.mp4).

One keypress. No dialogs. No account. No installer. **One 900 KB binary** —
copy it anywhere, run it, and tick *Run at startup* if you want it back at
login. Delete the file and it's uninstalled.

**Public domain.** Take it, sell it, fork it, ship it. See `LICENSE`.

By **Alex Maximilius** — *Alex Maz* — [github.com/AlexMaximilius](https://github.com/AlexMaximilius)
Dedicated to the public domain by its copyright holder, 2026.

---

## Run it

Double-click `orb_recorder.exe`. An orange orb appears. Drag it somewhere
you'll remember — it stays there across restarts, and comes back to the same
corner of the same monitor even if that monitor was asleep at boot.

| key | does |
|---|---|
| **F4** / **PrintScreen** | drag a rectangle → **PNG**, on the clipboard, opens in the editor |
| **F5** | record the focused window → GIF |
| **F6** | drag a region → GIF |
| **F7** | record the focused window → MP4 with sound |
| **F8** | drag a region → MP4 with sound |
| **F9** | record a camera → MP4 |
| **Esc** | stop |
| **F1** | help, every binding |

Press again to stop. Files land in `Pictures\ORB_Recorder\`, named after the
window you recorded, **already on your clipboard** — alt-tab to a chat window
and paste.

Also: **double-click** the orb to arm, then click the window you want.
**Scroll** over it to resize. **Right-click** for everything else — monitor
capture, a **delayed screenshot** that can photograph an open menu, sound
source, *Run at startup*, and a checkbox per hotkey so you can hand F5 back to
whatever else wants it.

**Drop a file on the orb.** GIFs open a frame editor with trim. Images open
the annotation editor. Videos open in your default player.

Colour is state: orange idle, yellow armed, **red recording GIF**, **magenta
recording video**, **cyan recording camera**, green editing.

---

## New in 3.1 — the editor is ours now

![The built-in screenshot editor](media/editor.png)

3.0 handed screenshots to Paint. This one has its own editor, because Paint is
being retired and because a screenshot editor is about four hundred lines when
you already own a window, a texture and a PNG writer.

Take a shot and it opens here: **arrow, box, oval, line, freehand, marker,
pixelate, text, numbered steps, crop**. Eight colours, a thickness, undo and
redo, `Ctrl+C` for the clipboard and `Ctrl+S` to save over the file. Every
tool is one key — the row along the bottom of the window lists them so there
is nothing to memorise. It works on any image you drop on the orb, not only on
screenshots.

Three things about it are deliberate:

**A stroke is rasterised the moment you let go.** Greenshot keeps each
annotation as a live object you can move afterwards, which is genuinely nicer
and costs a selection model, hit-testing, drag handles, z-order and a document
format. Undo covers the same ground for a fraction of the code, and one pixel
buffer is the whole document.

**The preview is the result.** The shape you see while dragging is drawn by
the same `paint.h` call that commits it, into the same buffer. An editor that
previews on the GPU and saves on the CPU has two rasterisers and eventually
two answers; this one cannot disagree with itself. It repaints only the dirty
rectangle, so a drag stays smooth on a 4K screenshot instead of pushing 30 MB
per mouse-move.

**The marker multiplies and the obfuscator averages.** A highlighter that
covers the text it marks is a redaction, so `MRK` keeps the darker of the two
channels and the words stay readable through the colour. Obfuscation is a
block average rather than a blur, because blurs have been reversed on
published screenshots more than once and an average has nothing left to
reverse.

Text uses the **system font** via a coverage mask from GDI, so labels are real
mixed-case type rather than the built-in 5×7 glyphs. Where no font engine is
available — X11, today — it falls back to those glyphs rather than dropping
the text.

Paint is still one click away: *After a screenshot, open >* in the right-click
menu picks between the built-in editor, the system image editor, and nothing
at all.

---

## New in 3.0

**Every screenshot opens in Paint, and in its folder.** A screenshot is
almost never finished at the moment it is taken — something in it wants
circling, cropping, or blanking out — so the shot now arrives in an image
editor with the folder behind it, on top of already being on the clipboard.
Region, window and delayed captures all behave the same way. (3.1 replaced
Paint with the built-in editor and turned the checkbox into the three-way
*After a screenshot, open >* above.)

Not hardcoded to Paint: it asks the shell for the **`edit` verb**, which is a
different thing from `open`. Double-clicking a `.png` gets you a viewer;
`edit` gets you whatever the machine uses for editing images — Paint out of
the box, Paint.NET or GIMP or Photoshop if you've said so, and no change here
when you change your mind. Where no edit verb is registered at all, Paint is
asked for by name rather than nothing happening. On Linux there is no such
verb, so it looks for a real editor first and only falls back to `xdg-open`.

---

## New in 2.1

**Recording no longer stops while the orb's own right-click menu is open.**
A popup menu is a modal message loop: it does not return until the menu
closes, and it is entered from inside an input callback, so the entire main
loop — capture included — was parked for as long as the menu was up. Frames
were not being dropped, they were never being taken. The capture tick is now
driven by a timer message, which the modal loop dispatches like any other. On
a nine-second clip with the menu held open for five of them: **76 frames
before, 231 after.** The GIF at the top of this page is the result.

**Hotkeys no longer disappear while that menu is open.** They were registered
against the thread, which makes `WM_HOTKEY` a thread message with no window to
be delivered to — so any message pump that was not ours pulled it out and
dropped it. Registering against the window instead means every pump routes it
home.

**A delayed screenshot** in the right-click menu, because a still of an open
menu has no other route: every screenshot hotkey is an interactive region
drag, and interacting dismisses the menu you were trying to photograph.

Also: the version resource said 1.0.0.0 and now says what it is.

---

## What's interesting about it

**Screenshots are the fastest thing here.** Drag a rectangle and, before you
have finished letting go of the mouse: the PNG is written, the **image** is on
the clipboard (paste into a chat box), the **file** is on the clipboard (paste
as an attachment), and the folder is raised with the file selected. The
capture was never the slow part — finding the file afterwards was.

**GIF is capped at 7 MB and stops itself.** Not a preference, a design
decision: a GIF nobody can send is a failed GIF. Discord's free limit is 8 MB,
GitHub's is 10.

**It captures GPU-composited windows.** Browsers, Electron apps and games
render through DirectComposition, where the usual `BitBlt` returns a frozen
cached surface — which is why so many recorders produce a still image of a
moving window. This uses `PrintWindow(PW_RENDERFULLCONTENT)`, with a
screen-rectangle fallback.

**It captures elevated windows.** Windows UIPI blocks a normal process from
reading an admin window, so it detects the integrity mismatch and reads the
screen rectangle instead, which isn't blocked.

**Image formats come from the OS, not a hardcoded list.** It asks Windows
Imaging Component what decoders are installed — 14 codecs, 64 extensions on a
stock Windows 11 box, including HEIC/AVIF and the whole camera-RAW family. On
Linux it asks gdk-pixbuf. Install a codec and the viewer supports it with no
change here.

**It survives losing the display driver.** If no OpenGL context can be
created, the orb keeps running painted in 2D and **recording still works** —
capture is GDI and Media Foundation, which never needed OpenGL. The editor,
viewer and region select say so plainly instead of failing.

**No dependencies you have to go and get.** GIF encode and decode are written
here. Video is Media Foundation, audio is WASAPI loopback, thumbnails are the
shell, formats are WIC — all operating system components. No ffmpeg, no codec
pack, no runtime.

That claim is checked, not asserted. Every DLL the binary imports ships with
Windows:

```
advapi32  comdlg32  gdi32  glu32  kernel32  mf  mfplat
mfreadwrite  msvcrt  ole32  opengl32  shell32  user32
```

The GCC runtime is linked in statically (`-static`, 42 KB). Without that the
executable needs `libwinpthread-1.dll`, which exists only on machines that
have MinGW installed — so it runs on the machine that built it and fails on
every other one.

---

## Build it

**Windows** — needs MinGW-w64 (`gcc`, `windres`) and GLFW 3.3.8 binaries:

```
build.bat
```

Edit `GLFW_ROOT` inside if GLFW isn't at `C:\glfw-3.3.8.bin.WIN64`.

**Linux / X11**:

```
sudo apt install build-essential libglfw3-dev libx11-dev libxext-dev \
                 libxrandr-dev libgl1-mesa-dev libglu1-mesa-dev zenity xclip
./build.sh
```

> **Platform status, stated plainly.** Windows is the developed and tested
> target. The Linux build compiles clean but has only ever been built on a
> headless machine — every X11 path (XShm capture, XGrabKey, `_NET_WM_STATE`,
> XShape regions, the zenity menu) is **written and unexercised**. Video and
> camera are Windows-only; Linux reports them unavailable rather than
> pretending. Expect to fix things.

```
orb_recorder.c          core: orb, capture loop, editor, viewer
platform.h              the OS seam, ~30 functions
platform_win32.c        Windows: GDI, hotkeys, shell, COM, WIC, registry
platform_win32_video.c  Windows: H.264/AAC via Media Foundation + WASAPI
platform_x11.c          Linux: XShm, XGrabKey, XDG, gdk-pixbuf
paint.h                 annotation rasteriser: lines, shapes, blur, masks
gif.h                   GIF89a writer
gif_reader.h            GIF89a reader
png_write.h             PNG writer
stb_image.h             still-image decode (vendored)
simplewebp.h            WebP decode (vendored)
```

Pure C11. The core is GLFW + OpenGL + libc; everything OS-specific is behind
`platform.h`.

---

## Where things go

| | Windows | Linux |
|---|---|---|
| recordings | `%USERPROFILE%\Pictures\ORB_Recorder\` | `$XDG_PICTURES_DIR/ORB_Recorder/` |
| settings | `%LOCALAPPDATA%\ORB_Recorder\settings.ini` | `$XDG_CONFIG_HOME/ORB_Recorder/` |
| log | `%LOCALAPPDATA%\ORB_Recorder\log.txt` | `$XDG_STATE_HOME/ORB_Recorder/` |

Upgrading from the old GIF_Recorder build? Settings are copied across on first
run — parked position, orb size and hotkey choices all follow. Old recordings
stay where they are; nothing moves your files.

---

## If it saved you time

It's free and it stays free — public domain, no account, no telemetry, no
nag screen. If it saved you an afternoon and you'd like to say thanks:

- **Ko-fi** — https://ko-fi.com/alexmaz
- **GitHub Sponsors** — the Sponsor button at the top of this page

Neither changes anything about the software. There is no paid tier, nothing
held back, and nothing that stops working if you don't. The most useful thing
you can do costs nothing: tell someone who is still dragging a rectangle in a
dialog box.

---

## Prior art / defensive publication

**Written by Alex Maximilius ("Alex Maz"). Published 2026-08-15 and
dedicated to the public domain by its copyright holder.** Items 13–16 were
added 2026-08-21 and items 17–20 on 2026-08-22; each is published as of the
date given.

This section exists so the design described here **cannot be patented by
anyone**, including me. Publication with a date establishes prior art: what is
disclosed below is, from this date, not novel, and a patent claim requires
novelty.

The obvious variations are enumerated deliberately. A vague disclosure only
blocks a patent on exactly what it describes; naming the variants makes those
unpatentable too. If it is written here, it is public.

**The core design.** A small, always-on-top, borderless, transparent,
circular desktop object ("orb") that serves as the entire user interface for
screen capture. State is carried by the colour of the object rather than by
text or iconography. Recording is initiated by a global hotkey with no
intervening dialog, and the resulting file is placed on the system clipboard
automatically.

**Specific mechanisms disclosed:**

1. A circular window region (`SetWindowRgn` / XShape) so that a round object
   has a round hit-box, and clicks in the transparent corners pass through.
2. A window held permanently larger than the visible object, with the region
   clipping it, so transient effects can be drawn outside the object and the
   object can be resized **without the window ever changing size** — which
   also removes the frame-lag between a window resize and its framebuffer.
3. Window position stored as a **display identity plus a fractional
   coordinate** (per-mille of that display's width and height) rather than
   absolute pixels, so placement survives resolution changes, display
   renumbering, and monitors absent at launch.
4. Deferred re-homing: honouring the position on another display while
   retaining the original preference, and returning when that display
   reappears.
5. An expanding-ring "locate me" animation triggered by taskbar or tray
   activation, drawn without moving the object.
6. Scroll-wheel resizing of the object about its own centre.
7. Selecting a capture target by arming the object and then clicking any
   window, as an alternative to focusing it first.
8. Automatically halting a recording when the **encoded file size** crosses a
   threshold chosen for a messaging platform's attachment limit.
9. Determining supported image formats by querying the operating system's
   codec registry at runtime, and using that registry as the decoder of last
   resort.
10. Falling back to screen-rectangle capture when the target window's process
    runs at a higher integrity level than the capturing process.
11. Placing a screenshot on the clipboard **as both an image and a file**, and
    raising the containing folder with the file selected, as one action.
12. Continuing to operate with a 2D-rendered object when no 3D context can be
    created, retaining capture because capture does not depend on 3D.
13. Continuing a capture across the capturing application's **own modal loop**
    — a popup menu or dialog — by driving the capture tick from a timer
    message that the modal loop itself dispatches, so that an application can
    record its own transient user interface.
14. Registering global hotkeys against the application's **window** rather
    than its thread, so that the key survives any foreign message pump,
    including the application's own modal menus.
15. Performing a timed capture from a background thread rather than the main
    loop, so that it fires on schedule while the user interface thread is
    inside a modal loop — capturing transient interface elements (menus,
    tooltips, drag states) that any keystroke would dismiss.
16. Delivering a freshly captured image to the operating system's registered
    **editor** association rather than its viewer association, automatically
    and as part of the capture, so that the capture arrives in something that
    can annotate it; including doing so alongside placing the same capture on
    the clipboard and revealing it in its containing folder.
17. Presenting a capture in an annotation editor in which the live preview of
    a shape being dragged is produced by the same rasteriser, into the same
    pixel buffer, that will commit it -- so that the preview and the saved
    file cannot differ -- with the preview confined to the changed rectangle
    so that the cost is independent of the image's size.
18. Annotating by immediate rasterisation into a single pixel buffer, with a
    bounded stack of whole-image snapshots as the undo model, in place of a
    retained-object document; including treating a crop, which changes the
    image's dimensions, as an ordinary entry on that same stack.
19. Marking up a capture with a multiplicative highlight that preserves the
    legibility of what is beneath it, and obfuscating with a block average
    chosen over a blur specifically because averaging is not invertible.
20. Rendering annotation text by obtaining a coverage mask from the host
    operating system's font engine and compositing it into the image, so that
    an application with no font library of its own still produces text in the
    system's typeface, falling back to a built-in bitmap font where no such
    engine is present.

**Also disclosed, as obvious extensions:** any other shape in place of a
circle; any other colour scheme or use of motion, size, opacity or sound to
indicate state; any other hotkey assignment; other output formats including
WebP, APNG, AV1 or a new format; picture-in-picture composition of a camera
over a screen capture; embedding the object in a taskbar, dock, menu bar,
notification area or window title bar; multiple simultaneous objects; network,
cloud or clipboard destinations for the output; automatic upload; AI-driven
selection of what to capture, when to start, or when to stop; voice control of
any of the above; and the application of every mechanism listed above to
audio-only, camera-only, or telemetry capture.

No patent rights are claimed, sought, or reserved.
