# ORB_Recorder

A dime-sized always-on-top orb that records your screen to **GIF** or **MP4
with sound**, records a **camera**, takes **screenshots**, and views **every
image format your machine has a decoder for**.

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
| **F4** / **PrintScreen** | drag a rectangle → **PNG**, on the clipboard, folder opens |
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
capture, sound source, *Run at startup*, and a checkbox per hotkey so you can
hand F5 back to whatever else wants it.

**Drop a file on the orb.** GIFs open a frame editor with trim. Images open a
viewer. Videos open in your default player.

Colour is state: orange idle, yellow armed, **red recording GIF**, **magenta
recording video**, **cyan recording camera**, green editing.

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
dedicated to the public domain by its copyright holder.**

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
