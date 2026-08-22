/*
 * gif_orb.c -- ORB_Recorder, portable core.
 *
 * Alex Maz -- first program (2026).
 * "AI knows how to code" -- pure C, one core file, two platform layers.
 *
 * A dime-sized always-on-top orb that records a window or monitor to GIF.
 * This file is GLFW + OpenGL + gif.h + platform.h only. All OS-specific
 * work lives in platform_win32.c (Windows) or platform_x11.c (Linux/X11).
 */

#include <GLFW/glfw3.h>
/* glu.h on MinGW uses the CALLBACK macro from windows.h. Include first. */
#ifdef _WIN32
#  include <windows.h>
#endif
#include <GL/gl.h>
#include <GL/glu.h>
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F     /* OpenGL 1.2 -- missing in some GL headers */
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>

#define GIF_H_IMPLEMENTATION
#include "gif.h"
#define GIF_READER_IMPLEMENTATION
#include "gif_reader.h"
/* Portable still-image decode: JPG/PNG/BMP/TGA/PSD. Anything stb cannot
 * read falls back to the OS shell thumbnail (see plat_shell_thumbnail). */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
/* GLFW hands drop paths as UTF-8. Without this stb calls plain fopen(),
 * which is ANSI on Windows and fails on any path with non-ASCII in it --
 * e.g. the em-dash Firefox puts in screenshot filenames. */
#ifdef _WIN32
#  define STBI_WINDOWS_UTF8
#endif
#include "stb_image.h"
/* WebP: stb has no decoder for it. simplewebp is a single header, BSD-3,
 * no dependencies, and handles lossy VP8 as well as lossless VP8L and
 * alpha -- verified against generated test files before adopting it.
 * This is why we do not need Microsoft's Store codec package. */
#define SIMPLEWEBP_IMPLEMENTATION
#include "simplewebp.h"
#include "platform.h"
#define PNG_WRITE_IMPLEMENTATION
#include "png_write.h"
#include "paint.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── config ─────────────────────────────────────────────────────────────── */

#define ORB_SIZE_DEFAULT 68       /* dime, out of the box */
#define ORB_SIZE_MIN     36
#define ORB_SIZE_MAX     240
#define ORB_SIZE        (g.orb_size)   /* live value -- scroll to resize */
#define EDITOR_W        900       /* editor window default size */
#define EDITOR_H        600
/* The window is ALWAYS this big -- three times the orb -- and a circular
 * region clips it down to just the orb. Nothing is ever resized or moved to
 * make room for the ping; only the region changes. That removes the
 * appear/disappear flicker on restore, because a resize plus a move plus a
 * repaint is exactly what was causing it. */
/* The orb's window is a FIXED size and is never resized.
 *
 * It used to be three times the current orb, resized on every scroll notch.
 * That is what made resizing wobble: the window changes size immediately but
 * the GL framebuffer follows a frame later, and while they disagree the orb
 * is drawn centred in the smaller framebuffer, which sits at the window's
 * top-left -- so it appears offset by half the difference in BOTH axes. That
 * is the dx == dy flick measured off Joe's captures, 9 px for a 6 px step,
 * and it survived two narrower fixes because the real cause is the resize
 * itself.
 *
 * Sizing it once for the largest orb removes the resize from the problem
 * entirely: no size change, no framebuffer lag, nothing to desynchronise.
 * The window region already clips it to a circle, so a window bigger than
 * the orb costs nothing visually and nothing in hit-testing -- which is
 * exactly the trick the ping already relies on. */
#define ORB_WIN_SIZE    (ORB_SIZE_MAX * 3)

/* The SPHERE is much smaller than the viewport it is drawn into: radius 0.5
 * at z = -3 under a 45-degree perspective spans 2*3*tan(22.5) = 2.485 world
 * units across ORB_SIZE pixels, so the visible ball is only 0.402 * ORB_SIZE
 * wide. Anything positioned against the viewport edge therefore sits ~30% of
 * the orb's width away from the ball itself, which is exactly why the toast
 * kept looking detached. Integer per-mille, no float in layout maths. */
#define ORB_VIS_RADIUS  ((g.orb_size * 201) / 1000)
#define PING_MS         1100      /* taskbar-click ping: brief acknowledgement */
#define PING_MS_LONG    4000      /* startup / re-home ping: long enough to find */
#define SETTINGS_DEBOUNCE_MS 600  /* delay after last move before saving */
#define THUMB_H         64        /* height of each thumbnail in the strip */
#define TARGET_W        640
#define TARGET_H        480
#define MAX_TARGET_W    1280
#define MAX_TARGET_H    720
#define GIF_DELAY_10MS  10        /* 100ms per frame -> 10 fps */
#define VIDEO_FPS       30        /* video is smooth where GIF is cheap */
#define VIDEO_MAX_SEC   600       /* 10 minutes -- a real ceiling, not 30s */
#define MAX_FRAMES      300       /* 30 second cap */
#define GIF_MAX_MONITORS 16

/* Size-aware auto-stop: cap encoded file at this many bytes.
 * Discord free = 8 MB, GitHub = 10 MB. 7 MB leaves headroom for anything. */
#define MAX_GIF_BYTES   (7 * 1024 * 1024)

/* ── state ──────────────────────────────────────────────────────────────── */

/* What an armed pick will do when the user clicks a window.
 *
 * The three "... window" menu items arm rather than acting on the focused
 * window, because opening a popup menu requires SetForegroundWindow on the
 * orb -- so by the time the item runs, the orb IS the foreground window and
 * "record the focused window" correctly refuses to record itself. Pointing at
 * the target is both the fix and the better interaction. */
#define ARM_GIF   0
#define ARM_MP4   1
#define ARM_SHOT  2

typedef enum {
    ORB_NOCAPTURE = -1,
    ORB_IDLE      = 0,
    ORB_ARMED     = 1,
    ORB_RECORDING = 2,
    ORB_EDITOR    = 3    /* viewing / editing a dropped GIF */
} OrbState;

typedef struct {
    GLFWwindow* window;
    void*       native;             /* opaque native handle from platform */
    OrbState    state;

    /* One flag per hotkey (F5..F9). Any of them can be handed back to the
     * rest of the system -- F5 is Refresh almost everywhere, and F7/F8 are
     * claimed by plenty of applications too. */
    bool hotkey_on[PLAT_HK_COUNT];
    bool auto_open_folder;
    bool auto_clipboard;
    /* A screenshot is almost never finished at the moment it is taken --
     * something in it wants circling, cropping or blanking out. Handing it
     * straight to an editor removes the step everybody does by hand.
     * 0 = nothing, 1 = the built-in editor, 2 = whatever the OS uses. */
    int  shot_editor;

    /* Set by save_screenshot, acted on by the main loop. The delayed capture
     * saves from a background thread, and creating a window off the main
     * thread is undefined behaviour in GLFW on every platform -- so the path
     * is parked here and opened where windows are allowed to be made. */
    char pending_edit[700];   /* sized to match save_screenshot's path[] */
    bool tray_only;               /* hide taskbar button, use notification area */
    int  audio_src;               /* PLAT_AUDIO_* for video recording */
    bool dbl_video;               /* double-click arms MP4 rather than GIF */
    bool armed_video;             /* the pick in flight is MP4 */
    int  armed_mode;              /* ARM_* -- what the pick will do */

    /* recording target: either a window handle or a monitor rect */
    void*      targetWindow;
    PlatMonitor targetMonitor;
    bool       recordingMonitor;
    bool       recordingVideo;    /* F7: MP4 + sound rather than GIF */
    bool       recordingCamera;   /* F9: frames come from a camera, not the screen */
    uint64_t   videoFrameDueMs;

    /* Live GIF writer -- frames are LZW-encoded into the file as they're
     * captured. No per-frame buffer, no background save thread. */
    GifWriter  gw;
    bool       gw_open;
    char       record_path[512];
    char       record_label[64];   /* sanitized window title for the filename */
    int        nframes;
    int        captureW, captureH;
    uint64_t   recordStartMs;
    uint64_t   lastFrameMs;

    bool     isDragging;
    int      dragOffsetX, dragOffsetY;
    uint64_t lastClickMs;

    char     popupText[256];       /* full text for log */
    char     popupShort[24];       /* short text drawn on the orb */
    char     popupLabel[12];       /* optional line ABOVE it: GIF / MP4 */
    uint64_t popupUntilMs;

    float    rotationAngle;

    PlatMonitor monitors[GIF_MAX_MONITORS];
    int         nmonitors;

    /* Remembered geometry (persisted to settings.ini).
     *
     * want_orb_* is where the USER parked the orb. orb_* is where it
     * actually sits right now. They differ when the monitor the user chose
     * is not awake yet -- which happens every morning, because a slow
     * monitor makes Windows evacuate windows off it before it powers up.
     * Only a deliberate drag updates want_*, so a temporary fallback can
     * never overwrite the real preference. */
    int         want_orb_x, want_orb_y;
    /* Where the orb LIVES, expressed the way the user actually thinks about
     * it: "bottom-right of that screen", not "pixel 3315,1227". Absolute
     * coordinates rot -- a display can change resolution, get renumbered, or
     * go away for good, and Windows consolidates everything onto whatever is
     * left. A monitor identity plus a corner survives all of that. */
    char        anchor_mon[64];       /* stable display id, "" = none stored */
    /* Position as a FRACTION of the display, in per-mille: 950 = 95% of the
     * way across. Resolution-independent by construction, so the orb lands
     * in the same visual place on a 1080p laptop or a 4K panel.
     * Per-mille integers rather than floats -- this is stored state, and
     * integer state cannot drift or round differently between machines. */
    int         anchor_fx, anchor_fy; /* 0..1000, position of the orb centre */
    bool        have_anchor;

    int         orb_x, orb_y;         /* canonical dime-window top-left */
    bool        orb_displaced;        /* true = parked somewhere temporary */
    uint64_t    last_monitor_check_ms;
    int         last_monitor_count;
    int         ed_x, ed_y;           /* editor window top-left */
    int         ed_w, ed_h;           /* editor window size */
    bool        have_orb_pos;
    bool        have_ed_geom;
    bool        settings_dirty;
    uint64_t    settings_dirty_ms;

    int         orb_size;         /* live diameter; scroll wheel resizes */

    /* Locate-me ping (expanding rings around the orb). */
    bool        ping_active;
    uint64_t    ping_start_ms;
    int         ping_ms;          /* duration of the ping in flight */
    /* Where the orb sits INSIDE the enlarged ping window. Normally the
     * centre -- but when the orb is near a screen edge the big window has
     * to be clamped back on-screen, and without tracking this the orb
     * appears to jump sideways for the duration of the ping. */
    int         ping_cx, ping_cy;

    /* Editor -- its own top-level window, created on demand. */
    GLFWwindow* ed_window;
    bool        ed_open;
    GifReader   ed_r;
    int         ed_frame;         /* current frame index */
    int         ed_trim_in;       /* inclusive */
    int         ed_trim_out;      /* inclusive */
    bool        ed_playing;
    uint64_t    ed_last_advance_ms;
    unsigned    ed_main_tex;      /* GL texture id for main frame */
    int         ed_main_tex_frame;/* which frame is currently uploaded */
    unsigned*   ed_thumb_tex;     /* one texture per frame */
    int         ed_thumb_w;       /* per-thumb width (aspect-preserved) */
    char        ed_path[512];     /* source file path */

    /* Directory playlist: every image sitting next to the dropped file, so
     * Left/Right can walk the folder the way a picture viewer should. */
    char**      ed_files;
    int         ed_nfiles;
    int         ed_file_idx;
    bool        ed_is_static;     /* true = still image, false = animated GIF */

    /* How this image got here. A capture you just took is a document, not a
     * folder: the browse chevrons down each side are noise, and they sit
     * exactly where an annotation wants to be drawn. A file you dropped, or
     * picked, IS an entry in a folder, and browsing it is the point. */
    bool        ed_browsing;
    int         ed_thumb_prev_count; /* strip textures currently allocated */
} App;

static App g;

/* ── log (portable, using plat_get_log_path) ────────────────────────────── */

/* No GL context available: the orb paints itself with 2D primitives and
 * the GL-only windows (editor, viewer, help) decline politely. Recording
 * is unaffected -- capture is GDI/Media Foundation, not OpenGL. */
static bool g_no3d = false;

/* Orb hidden entirely. Not minimised -- there is no taskbar button to
 * restore from, which is exactly why hiding forces tray-only on: the tray
 * icon must exist before the orb can disappear, or there is no way back. */
static bool g_orb_hidden = false;

/* Delayed whole-screen capture.
 *
 * The only way to photograph a menu. Windows menus are modal and close on any
 * keystroke that is not a menu key, so a screenshot hotkey dismisses the very
 * thing being captured -- ours included, which is how this came to be needed:
 * documenting the program was impossible with the program.
 *
 * Whole screen rather than a region, because the region selector is itself a
 * full-screen window and would cover whatever you were trying to photograph.
 * Crop afterwards. */
static volatile bool g_shot_pending = false;
static volatile bool g_shot_cancel  = false;

static char g_img_exts[2048];   /* ".bmp,.gif,.jpg,..." from the OS */

static FILE* g_log = NULL;
static char  g_log_path[512];

static void log_open(void) {
    plat_get_log_path(g_log_path, sizeof g_log_path);
    /* Rotate at 1 MB */
    FILE* peek = fopen(g_log_path, "rb");
    if (peek) {
        fseek(peek, 0, SEEK_END);
        long sz = ftell(peek);
        fclose(peek);
        if (sz > 1024 * 1024) {
            char bak[600];
            snprintf(bak, sizeof bak, "%s.old", g_log_path);
            remove(bak);
            rename(g_log_path, bak);
        }
    }
    g_log = fopen(g_log_path, "a");
    if (g_log) {
        time_t tt = time(NULL);
        struct tm lt = *localtime(&tt);
        fprintf(g_log,
            "\n===== ORB_Recorder started %04d-%02d-%02d %02d:%02d:%02d "
            "(Alex Maz - first program) =====\n",
            lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
            lt.tm_hour, lt.tm_min, lt.tm_sec);
        fflush(g_log);
    }
}

static void log_close(void) {
    if (g_log) {
        fprintf(g_log, "===== stopped =====\n");
        fclose(g_log);
        g_log = NULL;
    }
}

static void log_write(const char* tag, const char* fmt, ...) {
    if (!g_log) return;
    time_t tt = time(NULL);
    struct tm lt = *localtime(&tt);
    fprintf(g_log, "%02d:%02d:%02d %-8s ",
            lt.tm_hour, lt.tm_min, lt.tm_sec, tag);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* ── settings persistence ───────────────────────────────────────────────
 * Window geometry SHOULD be an OS service. It isn't -- when a monitor
 * powers up late, Windows evacuates windows off the vanished display and
 * never puts them back. So we remember our own geometry, and on restore we
 * validate it against the monitors that actually exist right now. */

static char g_cfg_path[512];

/* Hotkey names in HK_IDS order. Declared here because settings_save
 * and settings_load use them and the table itself lives further down. */
extern const char* HK_NAMES[PLAT_HK_COUNT];

static void settings_save(void) {
    if (!g_cfg_path[0]) plat_get_config_path(g_cfg_path, sizeof g_cfg_path);
    FILE* f = fopen(g_cfg_path, "w");
    if (!f) { log_write("cfg", "could not write %s", g_cfg_path); return; }
    fprintf(f, "# ORB_Recorder settings -- Alex Maz\n");
    /* Deliberately want_*, not orb_*: if we are sitting on a fallback
     * because a monitor was asleep, saving that would erase the user's
     * actual choice a little more each day. */
    if (g.have_orb_pos)
        fprintf(f, "orb_x=%d\norb_y=%d\n", g.want_orb_x, g.want_orb_y);
    if (g.have_anchor) {
        fprintf(f, "orb_mon=%s\n", g.anchor_mon);
        fprintf(f, "orb_fx=%d\n",  g.anchor_fx);   /* per-mille across */
        fprintf(f, "orb_fy=%d\n",  g.anchor_fy);   /* per-mille down   */
    }
    if (g.have_ed_geom)
        fprintf(f, "ed_x=%d\ned_y=%d\ned_w=%d\ned_h=%d\n",
                g.ed_x, g.ed_y, g.ed_w, g.ed_h);
    fprintf(f, "orb_size=%d\n",  g.orb_size);
    /* Name each key rather than numbering it. "hk%d" with i+5 was fine
     * while the list ran F5..F9, but F4 and PrintScreen then wrote hk10
     * and hk11 -- which the reader could not parse, so those two settings
     * silently never loaded. Names cannot drift out of order that way. */
    for (int i = 0; i < PLAT_HK_COUNT; i++)
        fprintf(f, "hk_%s=%d\n", HK_NAMES[i], g.hotkey_on[i] ? 1 : 0);
    fprintf(f, "auto_open=%d\n", g.auto_open_folder ? 1 : 0);
    fprintf(f, "auto_clip=%d\n", g.auto_clipboard ? 1 : 0);
    fprintf(f, "shot_editor=%d\n", g.shot_editor);
    fprintf(f, "tray_only=%d\n", g.tray_only ? 1 : 0);
    fprintf(f, "orb_hidden=%d\n", g_orb_hidden ? 1 : 0);
    fprintf(f, "audio_src=%d\n", g.audio_src);
    fprintf(f, "dbl_video=%d\n", g.dbl_video ? 1 : 0);
    fclose(f);
    g.settings_dirty = false;
    log_write("cfg", "saved orb=(%d,%d) editor=(%d,%d %dx%d)",
              g.want_orb_x, g.want_orb_y, g.ed_x, g.ed_y, g.ed_w, g.ed_h);
}

static void settings_mark_dirty(void) {
    g.settings_dirty = true;
    g.settings_dirty_ms = plat_now_ms();
}

/* Flush a pending save once the user has stopped moving things. */
static void settings_tick(void) {
    if (!g.settings_dirty) return;
    if (plat_now_ms() - g.settings_dirty_ms < SETTINGS_DEBOUNCE_MS) return;
    settings_save();
}

static void settings_load(void) {
    plat_get_config_path(g_cfg_path, sizeof g_cfg_path);
    FILE* f = fopen(g_cfg_path, "r");
    if (!f) { log_write("cfg", "no settings at %s (first run)", g_cfg_path); return; }
    char line[256];
    int got_ex = 0, got_ey = 0, got_ew = 0, got_eh = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        /* String-valued keys first. */
        if (!strncmp(line, "orb_mon=", 8)) {
            if (snprintf(g.anchor_mon, sizeof g.anchor_mon, "%s", line + 8)
                    >= (int)sizeof g.anchor_mon)
                g.anchor_mon[0] = 0;   /* a half id might match the wrong screen */
            for (char* c = g.anchor_mon; *c; c++)
                if (*c == '\n' || *c == '\r') { *c = 0; break; }
            continue;
        }
        char key[64]; int val;
        if (sscanf(line, "%63[^=]=%d", key, &val) != 2) continue;
        if      (!strcmp(key, "orb_x"))     { g.want_orb_x = val; g.have_orb_pos = true; }
        else if (!strcmp(key, "orb_y"))     { g.want_orb_y = val; }
        else if (!strcmp(key, "ed_x"))      { g.ed_x = val; got_ex = 1; }
        else if (!strcmp(key, "ed_y"))      { g.ed_y = val; got_ey = 1; }
        else if (!strcmp(key, "ed_w"))      { g.ed_w = val; got_ew = 1; }
        else if (!strcmp(key, "ed_h"))      { g.ed_h = val; got_eh = 1; }
        else if (!strncmp(key, "hk_", 3)) {
            for (int i = 0; i < PLAT_HK_COUNT; i++)
                if (!strcmp(key + 3, HK_NAMES[i])) {
                    g.hotkey_on[i] = val != 0;
                    break;
                }
        }
        /* Settings written before the keys were named. */
        else if (!strncmp(key, "hk", 2) && key[2] >= '5' && key[2] <= '9') {
            g.hotkey_on[key[2] - '5'] = val != 0;
        }
        else if (!strcmp(key, "f5"))        { g.hotkey_on[0] = val != 0; }
        else if (!strcmp(key, "auto_open")) { g.auto_open_folder = val != 0; }
        else if (!strcmp(key, "orb_size"))  {
            /* Even, always. An odd size makes orb_size/2 truncate, which
             * puts the drawn centre half a pixel off and shivers as the
             * size steps odd-even-odd. Enforced here too, so a value
             * stored before that was understood cannot reintroduce it. */
            g.orb_size = val & ~1;
        }
        else if (!strcmp(key, "orb_fx"))    { g.anchor_fx = val; g.have_anchor = true; }
        else if (!strcmp(key, "orb_fy"))    { g.anchor_fy = val; }
        else if (!strcmp(key, "auto_clip")) { g.auto_clipboard = val != 0; }
        else if (!strcmp(key, "shot_editor")) {
            g.shot_editor = (val < 0 || val > 2) ? 1 : val;
        }
        /* 2.x wrote a plain on/off flag; on meant the system editor, which
         * was the only one there was. Keep those settings meaning what they
         * meant rather than silently changing what the tool does. */
        else if (!strcmp(key, "edit_shots")) { g.shot_editor = val ? 2 : 0; }
        else if (!strcmp(key, "tray_only")) { g.tray_only = val != 0; }
        else if (!strcmp(key, "orb_hidden")) { g_orb_hidden = val != 0; }
        else if (!strcmp(key, "dbl_video")) { g.dbl_video = val != 0; }
        else if (!strcmp(key, "audio_src")) {
            g.audio_src = (val >= 0 && val <= 3) ? val : PLAT_AUDIO_SYSTEM;
        }
    }
    fclose(f);
    g.have_ed_geom = got_ex && got_ey && got_ew && got_eh;
    log_write("cfg", "loaded from %s", g_cfg_path);
}

/* Is this rect meaningfully visible on some monitor that exists right now?
 * Guards against the classic Windows monitor-wake shuffle stranding a
 * window on a display that no longer exists. */
static bool rect_on_live_monitor(int x, int y, int w, int h) {
    PlatMonitor mons[GIF_MAX_MONITORS];
    int n = plat_enum_monitors(mons, GIF_MAX_MONITORS);
    for (int i = 0; i < n; i++) {
        int ix = x > mons[i].x ? x : mons[i].x;
        int iy = y > mons[i].y ? y : mons[i].y;
        int ax = (x + w) < (mons[i].x + mons[i].w) ? (x + w) : (mons[i].x + mons[i].w);
        int ay = (y + h) < (mons[i].y + mons[i].h) ? (y + h) : (mons[i].y + mons[i].h);
        int ow = ax - ix, oh = ay - iy;
        if (ow > 16 && oh > 16) return true;   /* a real chunk is on-screen */
    }
    return false;
}

/* The orb sits in the middle of a window three times its size, so the
 * window's top-left is offset by one orb radius... times three halves.
 * Concretely: window = 3*orb, orb centred, so offset = orb_size. */
static void orb_window_pos(int orb_x, int orb_y, int* wx, int* wy) {
    /* The orb sits centred in a fixed-size window, so the inset depends on
     * the current orb size rather than being a constant multiple. */
    int inset = (ORB_WIN_SIZE - g.orb_size) / 2;
    *wx = orb_x - inset;
    *wy = orb_y - inset;
}

/* Keep the orb somewhere it can still be seen.
 *
 * A window dragged past the edge of the desktop is not merely untidy, it is
 * UNRECOVERABLE: an invisible orb cannot be grabbed to drag it back, and the
 * only remedy is hand-editing settings.ini. Monitors also rarely tile the
 * virtual desktop exactly, so there is real dead space between them to fall
 * into -- which is where this one ended up, at (5562,1644) with the nearest
 * display ending at x=5360, y=1298.
 *
 * The test is on the orb's CENTRE rather than its bounds, so it can still
 * straddle the seam between two displays; it is only pulled back when the
 * centre leaves every monitor. */
static void clamp_orb_to_monitors(int* x, int* y) {
    PlatMonitor mons[GIF_MAX_MONITORS];
    int n = plat_enum_monitors(mons, GIF_MAX_MONITORS);
    if (n <= 0) return;

    int s  = g.orb_size;
    int cx = *x + s / 2;
    int cy = *y + s / 2;

    for (int i = 0; i < n; i++) {
        if (cx >= mons[i].x && cx < mons[i].x + mons[i].w &&
            cy >= mons[i].y && cy < mons[i].y + mons[i].h) {
            return;                      /* centre is on a display: fine */
        }
    }

    /* Off every display -- pull it back onto whichever one is nearest. */
    int best = 0;
    long long bestd = -1;
    for (int i = 0; i < n; i++) {
        long long dx = (long long)cx - (mons[i].x + mons[i].w / 2);
        long long dy = (long long)cy - (mons[i].y + mons[i].h / 2);
        long long d  = dx * dx + dy * dy;
        if (bestd < 0 || d < bestd) { bestd = d; best = i; }
    }
    const PlatMonitor* m = &mons[best];
    if (*x < m->x) *x = m->x;
    if (*y < m->y) *y = m->y;
    if (*x + s > m->x + m->w) *x = m->x + m->w - s;
    if (*y + s > m->y + m->h) *y = m->y + m->h - s;
}

/* Where the orb IS, read from the window rather than from g.orb_x.
 *
 * Those two can drift -- the window is placed during a startup sequence that
 * also shows, styles, shapes and re-parents it, and any of that can land it
 * somewhere g.orb_x does not know about. When they disagree, anything that
 * computes a mouse offset against g.orb_x makes the orb teleport on the
 * first tick: the offset is measured to a place the orb is not. So the
 * interactive paths ask the window and re-sync g.orb_x from the answer. */
static void orb_sync_from_window(void) {
    int wx, wy;
    glfwGetWindowPos(g.window, &wx, &wy);
    /* Must be the exact inverse of orb_window_pos(). It used to add orb_size,
     * which was right only while the window was three times the orb; with a
     * fixed-size window the inset depends on the current size, and using the
     * old formula walked the orb across the screen a little on every scroll. */
    int inset = (ORB_WIN_SIZE - g.orb_size) / 2;
    g.orb_x = wx + inset;
    g.orb_y = wy + inset;
}

/* Move so the ORB lands at (x,y) -- callers think in orb coordinates. */
static void orb_move_to(int x, int y) {
    int wx, wy;
    orb_window_pos(x, y, &wx, &wy);
    plat_window_move(g.window, wx, wy);
}

/* Where the toast sits, in window coordinates. Returns false when there is
 * nothing to show. Shared by the painter and the region builder so the two
 * can never disagree -- when they did, the OS clipped the text away and it
 * simply vanished. */
static bool toast_rect(int win_w, int win_h,
                       int* rx, int* ry, int* rw, int* rh, int* out_scale) {
    if (!g.popupShort[0]) return false;
    if (plat_now_ms() > g.popupUntilMs) return false;

    int scale = g.orb_size / 68;
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    int cap = (win_w - 4) / (6 * scale);
    if (cap < 4)  cap = 4;
    if (cap > 20) cap = 20;

    int nc = (int)strlen(g.popupShort);
    if (nc > cap) nc = cap;
    int nl = (int)strlen(g.popupLabel);
    if (nl > cap) nl = cap;

    int line_h = 7 * scale + 2;
    int lines  = nl > 0 ? 2 : 1;
    int bar_h  = line_h * lines + 3;
    int widest = nc > nl ? nc : nl;

    int tx = (win_w - widest * 6 * scale) / 2;
    if (tx < 1) tx = 1;
    /* Almost touching the orb -- any gap and the words stop reading as the
     * orb's own label. */
    /* Sit directly on top of the visible sphere, not the viewport box. */
    int ty = win_h / 2 - ORB_VIS_RADIUS - bar_h;
    if (ty < 0) ty = 0;

    *rx = tx - 3;
    *rw = widest * 6 * scale + 6;
    if (*rx < 0) { *rw += *rx; *rx = 0; }
    if (*rx + *rw > win_w) *rw = win_w - *rx;
    *ry = ty;
    *rh = bar_h;
    if (out_scale) *out_scale = scale;
    return (*rw > 0 && *rh > 0);
}

/* Region: at rest the orb circle, plus the toast bar when one is showing --
 * the toast is drawn above the orb and would otherwise be clipped off. A
 * ping opens the region to the whole window so the rings can paint. */
static void orb_apply_region(bool wide) {
    if (wide) { plat_window_set_shape(g.window, 0, 0, 0, 0, 0, 0, 0); return; }
    int win = ORB_WIN_SIZE;
    int rx = 0, ry = 0, rw = 0, rh = 0;
    toast_rect(win, win, &rx, &ry, &rw, &rh, NULL);
    /* plat_window_set_shape takes the TOP-LEFT of the circle's bounding box,
     * not its centre -- CreateEllipticRgn(cx, cy, cx+d, cy+d). Passing the
     * centre offsets the region by half the orb, which clips half of it away
     * and puts the clickable area where the orb is not. */
    int inset = (win - g.orb_size) / 2;
    plat_window_set_shape(g.window, inset, inset, g.orb_size,
                          rx, ry, rw, rh);
}

/* Re-shape when a toast appears, expires, OR CHANGES TEXT.
 *
 * Watching only appear/expire was a real bug: "ARMED" sized the region for
 * five characters, then "DISARMED" replaced the text while the toast was
 * still up, no transition fired, and the region stayed five wide -- so the
 * last three characters were clipped off by the OS. The region has to track
 * the string, not just its presence. */
static void toast_region_tick(void) {
    static char last[40] = {0};
    if (g.ping_active) { last[0] = 0; return; }            /* region is wide */
    bool showing = g.popupShort[0] && plat_now_ms() <= g.popupUntilMs;
    char now_txt[40];
    snprintf(now_txt, sizeof now_txt, "%s|%s",
             showing ? g.popupLabel : "", showing ? g.popupShort : "");
    if (strcmp(now_txt, last) == 0) return;
    snprintf(last, sizeof last, "%s", now_txt);
    orb_apply_region(false);
}

/* The monitor the OS considers primary: the one containing the origin,
 * else the first enumerated. Used when the preferred display is absent. */
static int primary_monitor_index(const PlatMonitor* mons, int n) {
    for (int i = 0; i < n; i++)
        if (mons[i].x == 0 && mons[i].y == 0) return i;
    return 0;
}

/* Describe a window position as "this corner of this display, inset by
 * this much" -- the form that survives layout changes. */
static void anchor_from_position(int x, int y, int w, int h) {
    PlatMonitor mons[GIF_MAX_MONITORS];
    int n = plat_enum_monitors(mons, GIF_MAX_MONITORS);
    if (n <= 0) return;

    /* Pick the display holding the window's centre. */
    int cx = x + w / 2, cy = y + h / 2;
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (cx >= mons[i].x && cx < mons[i].x + mons[i].w &&
            cy >= mons[i].y && cy < mons[i].y + mons[i].h) { best = i; break; }
    }
    if (best < 0) best = primary_monitor_index(mons, n);
    const PlatMonitor* m = &mons[best];

    /* Where the orb's centre sits, as a per-mille fraction of the display. */
    g.anchor_fx = (m->w > 0) ? (int)(((int64_t)(cx - m->x) * 1000) / m->w) : 500;
    g.anchor_fy = (m->h > 0) ? (int)(((int64_t)(cy - m->y) * 1000) / m->h) : 500;
    if (g.anchor_fx < 0) g.anchor_fx = 0;
    if (g.anchor_fx > 1000) g.anchor_fx = 1000;
    if (g.anchor_fy < 0) g.anchor_fy = 0;
    if (g.anchor_fy > 1000) g.anchor_fy = 1000;
    snprintf(g.anchor_mon, sizeof g.anchor_mon, "%s", m->id);
    g.have_anchor = true;
}

/* Turn an anchor back into a position. Returns false if nothing is live.
 * `exact` reports whether we landed on the display the user actually chose
 * (false = the preferred one is absent and we used the primary instead). */
static bool anchor_to_position(int w, int h, int* out_x, int* out_y, bool* exact) {
    PlatMonitor mons[GIF_MAX_MONITORS];
    int n = plat_enum_monitors(mons, GIF_MAX_MONITORS);
    if (n <= 0) return false;

    int idx = -1;
    if (g.anchor_mon[0]) {
        for (int i = 0; i < n; i++)
            if (strcmp(mons[i].id, g.anchor_mon) == 0) { idx = i; break; }
    }
    if (exact) *exact = (idx >= 0);
    if (idx < 0) idx = primary_monitor_index(mons, n);

    const PlatMonitor* m = &mons[idx];

    /* Fraction -> centre -> top-left, all integer. */
    int cx = m->x + (int)(((int64_t)g.anchor_fx * m->w) / 1000);
    int cy = m->y + (int)(((int64_t)g.anchor_fy * m->h) / 1000);
    int x = cx - w / 2;
    int y = cy - h / 2;

    /* Keep it fully on that display. */
    if (x < m->x) x = m->x;
    if (y < m->y) y = m->y;
    if (x + w > m->x + m->w) x = m->x + m->w - w;
    if (y + h > m->y + m->h) y = m->y + m->h - h;

    *out_x = x; *out_y = y;
    return true;
}

/* Compact 5x7 uppercase bitmap font. Bit 4 (0x10) = leftmost pixel.
 * Covers A-Z, 0-9, and a handful of punctuation. Lowercase mapped to upper. */
static const uint8_t GLYPH[128][7] = {
    [' '] = {0,0,0,0,0,0,0},
    ['!'] = {0x04,0x04,0x04,0x04,0,0,0x04},
    ['#'] = {0x0A,0x1F,0x0A,0x1F,0x0A,0,0},
    ['%'] = {0x18,0x19,0x02,0x04,0x08,0x13,0x03},
    ['('] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02},
    [')'] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08},
    ['+'] = {0,0x04,0x04,0x1F,0x04,0x04,0},
    [','] = {0,0,0,0,0,0x04,0x08},
    ['-'] = {0,0,0,0x0E,0,0,0},
    ['.'] = {0,0,0,0,0,0,0x04},
    ['/'] = {0x01,0x01,0x02,0x04,0x08,0x10,0x10},
    ['<'] = {0x00,0x02,0x04,0x08,0x04,0x02,0x00},
    ['>'] = {0x00,0x08,0x04,0x02,0x04,0x08,0x00},
    ['['] = {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},
    [']'] = {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E},
    ['='] = {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},
    ['_'] = {0x00,0x00,0x00,0x00,0x00,0x00,0x1F},
    ['*'] = {0x00,0x0A,0x04,0x1F,0x04,0x0A,0x00},
    ['0'] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    ['1'] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    ['2'] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    ['3'] = {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    ['4'] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    ['5'] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    ['6'] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
    ['7'] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    ['8'] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    ['9'] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    [':'] = {0,0x04,0,0,0,0x04,0},
    ['?'] = {0x0E,0x11,0x01,0x02,0x04,0,0x04},
    ['A'] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['B'] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    ['C'] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    ['D'] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    ['E'] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    ['F'] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    ['G'] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
    ['H'] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['I'] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    ['J'] = {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    ['K'] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    ['L'] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    ['M'] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    ['N'] = {0x11,0x11,0x19,0x15,0x13,0x11,0x11},
    ['O'] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['P'] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    ['Q'] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    ['R'] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    ['S'] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    ['T'] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    ['U'] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['V'] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    ['W'] = {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
    ['X'] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    ['Y'] = {0x11,0x11,0x11,0x0A,0x04,0x04,0x04},
    ['Z'] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
};

static void draw_glyph_pixels(float x, float y, char c, int scale) {
    unsigned char uc = (unsigned char)c;
    if (uc >= 128) return;
    if (uc >= 'a' && uc <= 'z') uc = uc - 'a' + 'A';
    for (int r = 0; r < 7; r++) {
        uint8_t bits = GLYPH[uc][r];
        for (int col = 0; col < 5; col++) {
            if (bits & (0x10 >> col)) {
                float px = x + (float)(col * scale);
                float py = y + (float)(r   * scale);
                glBegin(GL_QUADS);
                glVertex2f(px,                py);
                glVertex2f(px + (float)scale, py);
                glVertex2f(px + (float)scale, py + (float)scale);
                glVertex2f(px,                py + (float)scale);
                glEnd();
            }
        }
    }
}

static void draw_string_at(float x, float y, const char* s, int scale) {
    while (*s) {
        draw_glyph_pixels(x, y, *s, scale);
        x += (float)((5 + 1) * scale);   /* 5 wide + 1 spacing */
        s++;
    }
}

/* popup(short, long_fmt, ...):
 *   short: <=11 char mnemonic drawn on the orb (uppercase looks best).
 *   long_fmt (+args): full sentence for the log. */
/* popup_l(): like popup(), plus a small line ABOVE the message naming which
 * mode you are in -- GIF or MP4. Hitting F5 and F7 look identical otherwise,
 * and knowing which one is armed matters more than any other single word. */
static void popup_l(const char* label, const char* short_msg,
                    const char* long_fmt, va_list ap) {
    vsnprintf(g.popupText, sizeof g.popupText, long_fmt, ap);
    strncpy(g.popupShort, short_msg, sizeof g.popupShort - 1);
    g.popupShort[sizeof g.popupShort - 1] = 0;
    if (label) {
        strncpy(g.popupLabel, label, sizeof g.popupLabel - 1);
        g.popupLabel[sizeof g.popupLabel - 1] = 0;
    } else {
        g.popupLabel[0] = 0;
    }
    g.popupUntilMs = plat_now_ms() + 3500;
    log_write("popup", "%s%s%s", label ? label : "", label ? " " : "", g.popupText);
}

static void popup(const char* short_msg, const char* long_fmt, ...) {
    va_list ap; va_start(ap, long_fmt);
    popup_l(NULL, short_msg, long_fmt, ap);
    va_end(ap);
}

/* Tagged variant: popup_mode("GIF", "ARMED", "...") */
static void popup_mode(const char* label, const char* short_msg,
                       const char* long_fmt, ...) {
    va_list ap; va_start(ap, long_fmt);
    popup_l(label, short_msg, long_fmt, ap);
    va_end(ap);
}

/* ── recording lifecycle (streaming: frames go to file as captured) ─── */

static void compute_capture_size(int srcW, int srcH, int* outW, int* outH) {
    if (srcW <= 0 || srcH <= 0) { *outW = TARGET_W; *outH = TARGET_H; return; }
    int maxW = TARGET_W, maxH = TARGET_H;
    if (srcW > MAX_TARGET_W || srcH > MAX_TARGET_H) {
        popup("BIG SRC",
              "Source %dx%d capped at %dx%d. For higher-res, use a video encoder.",
              srcW, srcH, MAX_TARGET_W, MAX_TARGET_H);
        maxW = MAX_TARGET_W; maxH = MAX_TARGET_H;
    }
    double sx = (double)maxW / srcW;
    double sy = (double)maxH / srcH;
    double s = sx < sy ? sx : sy;
    if (s > 1.0) s = 1.0;
    *outW = (int)(srcW * s);
    *outH = (int)(srcH * s);
    if (*outW & 1) (*outW)--;
    if (*outH & 1) (*outH)--;
    if (*outW < 8) *outW = 8;
    if (*outH < 8) *outH = 8;
}

/* Turn a window title into something safe to put in a filename.
 *
 * Restricted to printable ASCII on purpose: gif.h opens the output with
 * plain fopen(), which is ANSI on Windows, so a title containing (say) the
 * em-dash Firefox puts in its window titles would fail to create the file.
 * Also strips the characters Windows forbids, collapses runs of separators,
 * and refuses trailing dots and spaces, which Windows silently drops. */
static void sanitize_label(const char* in, char* out, size_t out_sz) {
    size_t j = 0;
    bool last_us = false;
    for (const char* c = in; *c && j + 1 < out_sz; c++) {
        unsigned char ch = (unsigned char)*c;
        bool bad = (ch < 0x20) || (ch > 0x7E);          /* non-ASCII / control */
        if (!bad) {
            switch (ch) {
            case '\\': case '/': case ':': case '*': case '?':
            case '"': case '<': case '>': case '|':
                bad = true; break;
            default: break;
            }
        }
        if (bad || ch == ' ') {
            if (!last_us && j > 0) { out[j++] = '_'; last_us = true; }
        } else {
            out[j++] = (char)ch;
            last_us = false;
        }
    }
    /* Trim trailing separators, dots and spaces. */
    while (j > 0 && (out[j-1] == '_' || out[j-1] == '.' || out[j-1] == ' ')) j--;
    out[j] = 0;
    if (!out[0]) snprintf(out, out_sz, "capture");

    /* Reserved DOS device names. A window title is set by another program,
     * and a window called CON or LPT1 would name a file that Windows treats
     * as a device rather than a file -- writes go to the console, or fail in
     * confusing ways. Rare, but it costs one comparison to be safe. */
    {
        static const char* DEV[] = { "CON","PRN","AUX","NUL",
            "COM1","COM2","COM3","COM4","COM5","COM6","COM7","COM8","COM9",
            "LPT1","LPT2","LPT3","LPT4","LPT5","LPT6","LPT7","LPT8","LPT9", NULL };
        for (int i = 0; DEV[i]; i++) {
            size_t n = strlen(DEV[i]);
            bool same = (strlen(out) == n);
            for (size_t k = 0; same && k < n; k++) {
                char a = out[k];
                if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
                if (a != DEV[i][k]) same = false;
            }
            if (same) {
                snprintf(out, out_sz, "capture");
                break;
            }
        }
    }
}

/* Register or release a hotkey to match g.hotkey_on[]. Returns what the OS
 * actually granted -- asking for a key another app already owns fails, and
 * the menu should show that truthfully rather than a checkbox that lies. */
static const int HK_IDS[PLAT_HK_COUNT] = {
    PLAT_HK_F5, PLAT_HK_F6, PLAT_HK_F7, PLAT_HK_F8, PLAT_HK_F9, PLAT_HK_F4,
    PLAT_HK_PRTSC
};
const char* HK_NAMES[PLAT_HK_COUNT] = { "F5", "F6", "F7", "F8", "F9",
                                        "F4", "PRTSC" };

static bool apply_hotkey(int i, bool want) {
    if (i < 0 || i >= PLAT_HK_COUNT) return false;
    if (want) {
        bool got = plat_register_hotkey(HK_IDS[i]);
        g.hotkey_on[i] = got;
        if (!got) log_write("hotkey", "%s refused -- another app owns it",
                            HK_NAMES[i]);
        return got;
    }
    plat_unregister_hotkey(HK_IDS[i]);
    g.hotkey_on[i] = false;
    return false;
}

/* Compose the output path: <window title>_YYYYMMDD_HHMMSS.gif */
static void build_record_path(void) {
    char folder[512]; plat_get_output_dir(folder, sizeof folder);
    time_t tt = time(NULL);
    struct tm lt = *localtime(&tt);
    const char* label = g.record_label[0] ? g.record_label : "gif_orb";
    int need = snprintf(g.record_path, sizeof g.record_path,
             "%s/%s_%04d%02d%02d_%02d%02d%02d.gif",
             folder, label,
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             lt.tm_hour, lt.tm_min, lt.tm_sec);
    if (need >= (int)sizeof g.record_path) {
        /* Fall back to a name that always fits rather than writing to a
         * silently truncated path -- which on Windows can still open, at the
         * wrong name, in the wrong place. */
        log_write("save", "output path too long (%d chars); using a short name", need);
        /* Trim the FOLDER and nothing else, with the arithmetic written out
         * rather than left for the compiler to infer. snprintf-into-snprintf
         * says "this may truncate" at every step, which is true and is the
         * whole intent -- so say where the cut happens instead. */
        char ts[24];
        /* The %% keeps each field inside its stated width, so the total
         * length is a fact rather than an assumption -- tm fields are plain
         * ints and nothing else here promises they are small. */
        snprintf(ts, sizeof ts, "%04d%02d%02d_%02d%02d%02d",
                 (lt.tm_year + 1900) % 10000, (lt.tm_mon + 1) % 100,
                 lt.tm_mday % 100, lt.tm_hour % 100,
                 lt.tm_min % 100, lt.tm_sec % 100);
        size_t keep = strlen(folder);
        if (keep > sizeof g.record_path - 64) keep = sizeof g.record_path - 64;
        memcpy(g.record_path, folder, keep);
        snprintf(g.record_path + keep, sizeof g.record_path - keep,
                 "/capture_%s.gif", ts);
    }
}

/* Video: same targeting and same capture path as GIF, different encoder.
 * Returns false if video is unavailable on this platform. */
static bool start_video_common(void) {
    build_record_path();
    /* Swap the extension -- build_record_path() names GIFs. */
    size_t n = strlen(g.record_path);
    if (n > 4) snprintf(g.record_path + n - 4, 5, ".mp4");

    if (!plat_video_start(g.record_path, g.captureW, g.captureH,
                          VIDEO_FPS, g.audio_src)) {
        popup("NO VIDEO", "Video recording unavailable on this platform.");
        return false;
    }
    g.recordingVideo   = true;
    g.nframes          = 0;
    g.recordStartMs    = plat_now_ms();
    g.videoFrameDueMs  = g.recordStartMs;
    g.state            = ORB_RECORDING;
    plat_register_hotkey(PLAT_HK_ESCAPE);
    return true;
}

/* Common start path once size + target are decided. Opens the GIF writer,
 * transitions to RECORDING, registers Escape.                          */
static bool start_recording_common(void) {
    build_record_path();
    if (!GifBegin(&g.gw, g.record_path, g.captureW, g.captureH, GIF_DELAY_10MS)) {
        popup("SAVE ERR", "Failed to open %s for writing.", g.record_path);
        return false;
    }
    g.gw_open = true;
    g.nframes = 0;
    g.recordStartMs = plat_now_ms();
    g.lastFrameMs = 0;
    g.state = ORB_RECORDING;
    plat_register_hotkey(PLAT_HK_ESCAPE);
    return true;
}

static void start_recording_window(void* target) {
    if (!target) { popup("NO WIN", "No valid target window."); return; }
    if (plat_handles_equal(target, g.native)) {
        popup("NOT ORB", "Cannot record the orb itself. Pick another window.");
        return;
    }
    g.targetWindow = target;
    g.recordingMonitor = false;
    char title[128]; int sw = 0, sh = 0;
    uint8_t* probe = plat_capture_window(target, 8, 8, title, sizeof title, &sw, &sh);
    if (probe) free(probe);
    /* Name the file after the window -- far easier to find later than a
     * wall of identical timestamps. Truncated so paths stay sane. */
    char lbl[64];
    sanitize_label(title[0] ? title : "capture", lbl, sizeof lbl);
    if (strlen(lbl) > 48) lbl[48] = 0;
    while (lbl[0] && (lbl[strlen(lbl)-1] == '_')) lbl[strlen(lbl)-1] = 0;
    snprintf(g.record_label, sizeof g.record_label, "%s", lbl[0] ? lbl : "capture");

    compute_capture_size(sw, sh, &g.captureW, &g.captureH);

    /* Windows UIPI: an elevated target cannot be PrintWindow-ed from a
     * normal-integrity process. We fall back to reading that window's
     * screen rectangle, which works -- but captures whatever is visually
     * on top of it. Tell the user rather than silently recording overlap. */
    bool elevated_target = plat_window_is_elevated(target)
                           && !plat_process_is_elevated();
    if (!start_recording_common()) return;
    if (elevated_target) {
        popup_mode("GIF", "REC ADMIN",
              "REC (admin window, screen-region capture -- keep it unobscured; "
              "right-click orb > Restart as administrator for exact capture) "
              "%dx%d: %s",
              g.captureW, g.captureH, title[0] ? title : "(no title)");
    } else {
        popup_mode("GIF", "REC",
              "REC window %dx%d @ 10fps: %s  (Esc/double-click to stop)",
              g.captureW, g.captureH, title[0] ? title : "(no title)");
    }
}

static void stop_recording(void);   /* defined below */

/* -- help window --------------------------------------------------------
 * There was no help at all, which is the single biggest gap for anyone who
 * did not build the thing. Its own small window, drawn with the same 5x7
 * font as the toast -- no new dependency, and it scales like everything
 * else. F1 or the right-click menu. */

static GLFWwindow* g_help_win = NULL;

static const char* HELP_LINES[] = {
    "ORB_RECORDER  v3.6",
    "  BY ALEX MAXIMILIUS (ALEX MAZ)  GITHUB.COM/ALEXMAXIMILIUS",
    "  PUBLIC DOMAIN, 2026",
    "  screenshots, GIF, MP4 with sound, camera, image viewer",
    "",
    "",
    "RECORDING",
    "  F4 / PRTSC      SCREENSHOT A REGION -> PNG",
    "  HIDE ORB        RIGHT-CLICK MENU; TRAY ICON BRINGS IT BACK",
    "  DELAYED SHOT    RIGHT-CLICK MENU; THE ONLY WAY TO CAPTURE A MENU",
    "  EVERY SHOT      OPENS IN THE EDITOR + THE FOLDER",
    "",
    "EDITOR (ANY SCREENSHOT OR IMAGE)",
    "  A ARROW   R BOX    E OVAL   L LINE   P PEN",
    "  H MARKER  X PIXELATE  T TEXT  N NUMBER  C CROP",
    "  [ ]             THINNER / THICKER",
    "  1-8             COLOUR",
    "  CTRL+Z / CTRL+Y UNDO / REDO",
    "  CTRL+C          COPY THE EDITED IMAGE",
    "  CTRL+S          SAVE OVER THE FILE",
    "  F5              RECORD THE FOCUSED WINDOW / STOP",
    "  F6              DRAG A REGION, RECORD IT / STOP",
    "  F7              RECORD VIDEO + SOUND, WINDOW / STOP",
    "  F8              RECORD VIDEO + SOUND, REGION / STOP",
    "  F9              RECORD A CAMERA DIRECTLY / STOP",
    "                  SOUND SOURCE IS IN THE RIGHT-CLICK MENU",
    "  ESC             STOP RECORDING",
    "  DOUBLE-CLICK    ARM, THEN CLICK THE WINDOW YOU WANT",
    "                  ARMS GIF OR MP4 -- SET IN THE MENU",
    "  RIGHT-CLICK     MENU. HOTKEYS SUBMENU RELEASES ANY",
    "                  KEY BACK TO OTHER APPLICATIONS.",
    "",
    "THE ORB",
    "  DRAG            MOVE IT. THE SPOT IS REMEMBERED.",
    "  SCROLL          RESIZE, 36 TO 240 PX",
    "  CORE COLOUR     ORANGE IDLE   BLUE F5 OFF",
    "                  YELLOW ARMED  RED RECORDING",
    "                  GREEN EDITOR OPEN",
    "                  MAGENTA RECORDING VIDEO",
    "                  CYAN RECORDING CAMERA",
    "",
    "EDITOR AND VIEWER",
    "  DROP A FILE     GIF OPENS THE EDITOR",
    "                  VIDEO OPENS YOUR DEFAULT PLAYER",
    "                  IMAGE OPENS THE VIEWER",
    "  LEFT / RIGHT    GIF: STEP FRAMES",
    "                  IMAGE: PREV / NEXT IN FOLDER",
    "  [  ]            SET TRIM IN / OUT",
    "  SPACE           PLAY / PAUSE",
    "  S               SAVE TRIMMED COPY",
    "  PGUP / PGDN     PREVIOUS / NEXT FILE",
    "  HOME / END      FIRST / LAST FILE",
    "  ESC             CLOSE",
    "",
    "OUTPUT",
    "  10 FPS, 640X480 DEFAULT, 1280X720 CAP",
    "  AUTO-STOPS AT 7 MB OR 30 SECONDS",
    "  SAVED AS <WINDOW TITLE>_<DATE>_<TIME>.GIF",
    "  COPIED TO THE CLIPBOARD ON SAVE",
    "  GIF HAS NO AUDIO",
    "",
    "  SETTINGS AND LOG LIVE IN LOCALAPPDATA\\GIF_RECORDER",
    "  PRESS ESC OR CLOSE THIS WINDOW",
    NULL
};

static void help_close(void) {
    if (!g_help_win) return;
    glfwDestroyWindow(g_help_win);
    g_help_win = NULL;
    glfwMakeContextCurrent(g.window);
}

static void help_key(GLFWwindow* w, int key, int sc, int action, int mods) {
    (void)w; (void)sc; (void)mods;
    if (action != GLFW_PRESS) return;
    if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_F1) help_close();
}

static void help_open(void) {
    if (g_no3d) {
        popup_mode("NO 3D", "CANNOT",
                   "No 3D detected -- the help window needs it. "
                   "F4 shot, F5 GIF, F7 MP4 all still work.");
        return;
    }
    if (g_help_win) { glfwFocusWindow(g_help_win); return; }
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);
    glfwWindowHint(GLFW_FLOATING,  GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES,   0);
    g_help_win = glfwCreateWindow(620, 620, "ORB_Recorder - Help", NULL, NULL);
    if (!g_help_win) { popup("NO HELP", "Could not open the help window."); return; }
    glfwSetKeyCallback(g_help_win, help_key);

    /* Open beside the orb rather than wherever the OS feels like. Help that
     * appears across the desk from the thing it describes makes the reader
     * hunt for it; next to the orb the connection is obvious. Preferring the
     * side with more room keeps it on-screen when the orb is parked in a
     * corner, which is where people park it. */
    {
        const int HW = 620, HH = 620, GAP = 16;
        PlatMonitor mons[GIF_MAX_MONITORS];
        int n = plat_enum_monitors(mons, GIF_MAX_MONITORS);
        int hx = g.orb_x + g.orb_size + GAP;
        int hy = g.orb_y + g.orb_size / 2 - HH / 2;
        for (int i = 0; i < n; i++) {
            const PlatMonitor* m = &mons[i];
            int ocx = g.orb_x + g.orb_size / 2;
            int ocy = g.orb_y + g.orb_size / 2;
            if (ocx < m->x || ocx >= m->x + m->w) continue;
            if (ocy < m->y || ocy >= m->y + m->h) continue;
            /* Flip to the left when the right side cannot hold it. */
            if (hx + HW > m->x + m->w) hx = g.orb_x - GAP - HW;
            if (hx < m->x) hx = m->x + GAP;
            if (hy < m->y) hy = m->y + GAP;
            if (hy + HH > m->y + m->h) hy = m->y + m->h - HH - GAP;
            break;
        }
        glfwSetWindowPos(g_help_win, hx, hy);
    }

    glfwMakeContextCurrent(g_help_win);
    glfwSwapInterval(1);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glfwMakeContextCurrent(g.window);
}

static void help_draw(int w, int h) {
    glClearColor(0.07f, 0.07f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    int scale = 2, lh = 9 * scale, y = 14;
    for (int i = 0; HELP_LINES[i]; i++) {
        const char* line = HELP_LINES[i];
        if (!line[0]) { y += lh / 2; continue; }
        /* Headings (no leading space) in orange, bindings in grey-white. */
        if (line[0] != ' ') glColor4f(1.00f, 0.55f, 0.10f, 1.0f);
        else                glColor4f(0.82f, 0.85f, 0.90f, 1.0f);
        draw_string_at(14.0f, (float)y, line, scale);
        y += lh;
        if (y > h) break;
    }
}

/* -- region selection overlay ------------------------------------------
 * F6 (or right-click > Record region) drops a dimmed, crosshair-cursor
 * sheet over the whole virtual desktop. Drag a box; release to record it.
 * Escape or right-click cancels. Pure GLFW -- no platform code needed. */

static struct {
    GLFWwindow* win;
    int    vx, vy;              /* virtual-desktop origin (may be negative) */
    bool   dragging, done, cancelled;
    double ax, ay, bx, by;      /* window-space drag corners */
} g_rs;

static void rs_mouse(GLFWwindow* w, int button, int action, int mods) {
    (void)mods;
    if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) {
        g_rs.cancelled = true; g_rs.done = true;
        return;
    }
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;
    double cx, cy; glfwGetCursorPos(w, &cx, &cy);
    if (action == GLFW_PRESS) {
        g_rs.dragging = true;
        g_rs.ax = g_rs.bx = cx;
        g_rs.ay = g_rs.by = cy;
    } else if (action == GLFW_RELEASE && g_rs.dragging) {
        g_rs.bx = cx; g_rs.by = cy;
        g_rs.dragging = false;
        g_rs.done = true;
    }
}
static void rs_cursor(GLFWwindow* w, double x, double y) {
    (void)w;
    if (g_rs.dragging) { g_rs.bx = x; g_rs.by = y; }
}
static void rs_key(GLFWwindow* w, int key, int sc, int action, int mods) {
    (void)w; (void)sc; (void)mods;
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        g_rs.cancelled = true; g_rs.done = true;
    }
}

/* Union of every live monitor = the virtual desktop rectangle. */
static void virtual_desktop_bounds(int* x, int* y, int* w, int* h) {
    PlatMonitor mons[GIF_MAX_MONITORS];
    int n = plat_enum_monitors(mons, GIF_MAX_MONITORS);
    if (n <= 0) { *x = 0; *y = 0; *w = 1920; *h = 1080; return; }
    int x0 = mons[0].x, y0 = mons[0].y;
    int x1 = mons[0].x + mons[0].w, y1 = mons[0].y + mons[0].h;
    for (int i = 1; i < n; i++) {
        if (mons[i].x < x0) x0 = mons[i].x;
        if (mons[i].y < y0) y0 = mons[i].y;
        if (mons[i].x + mons[i].w > x1) x1 = mons[i].x + mons[i].w;
        if (mons[i].y + mons[i].h > y1) y1 = mons[i].y + mons[i].h;
    }
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
}

/* Returns true and fills the rect (screen coords) if the user picked one. */
static bool region_select(int* out_x, int* out_y, int* out_w, int* out_h) {
    /* The dimmed drag-a-rectangle sheet is itself a GL window, so region
     * capture is unavailable without 3D. Whole-window and whole-monitor
     * capture are not -- those go straight through GDI -- so name the
     * keys that still work rather than just refusing. */
    if (g_no3d) {
        popup_mode("NO 3D", "CANNOT",
                   "Region select needs 3D. F5 (GIF) and F7 (MP4) still "
                   "record the focused window; monitors work from the menu.");
        return false;
    }
    memset(&g_rs, 0, sizeof g_rs);
    int vw, vh;
    virtual_desktop_bounds(&g_rs.vx, &g_rs.vy, &vw, &vh);

    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING,  GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_SAMPLES,   0);

    g_rs.win = glfwCreateWindow(vw, vh, "Select region", NULL, NULL);
    if (!g_rs.win) {
        popup("NO SHEET", "Could not open the region-select overlay.");
        glfwMakeContextCurrent(g.window);
        return false;
    }
    glfwSetWindowPos(g_rs.win, g_rs.vx, g_rs.vy);
    glfwSetMouseButtonCallback(g_rs.win, rs_mouse);
    glfwSetCursorPosCallback  (g_rs.win, rs_cursor);
    glfwSetKeyCallback        (g_rs.win, rs_key);
    GLFWcursor* cross = glfwCreateStandardCursor(GLFW_CROSSHAIR_CURSOR);
    if (cross) glfwSetCursor(g_rs.win, cross);
    glfwMakeContextCurrent(g_rs.win);
    glfwSwapInterval(1);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glfwFocusWindow(g_rs.win);

    while (!g_rs.done && !glfwWindowShouldClose(g_rs.win)) {
        glfwPollEvents();
        int ww, wh; glfwGetWindowSize(g_rs.win, &ww, &wh);
        int fw, fh; glfwGetFramebufferSize(g_rs.win, &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        glOrtho(0, ww, wh, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

        /* Dim sheet so it is obvious we are in select mode. */
        glColor4f(0.0f, 0.0f, 0.0f, 0.35f);
        glBegin(GL_QUADS);
        glVertex2f(0, 0); glVertex2f((float)ww, 0);
        glVertex2f((float)ww, (float)wh); glVertex2f(0, (float)wh);
        glEnd();

        if (g_rs.dragging) {
            float x0 = (float)(g_rs.ax < g_rs.bx ? g_rs.ax : g_rs.bx);
            float x1 = (float)(g_rs.ax < g_rs.bx ? g_rs.bx : g_rs.ax);
            float y0 = (float)(g_rs.ay < g_rs.by ? g_rs.ay : g_rs.by);
            float y1 = (float)(g_rs.ay < g_rs.by ? g_rs.by : g_rs.ay);
            /* Punch the selection back to fully clear: true-colour preview. */
            glBlendFunc(GL_ONE, GL_ZERO);
            glColor4f(0, 0, 0, 0);
            glBegin(GL_QUADS);
            glVertex2f(x0, y0); glVertex2f(x1, y0);
            glVertex2f(x1, y1); glVertex2f(x0, y1);
            glEnd();
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glColor4f(1.0f, 0.55f, 0.10f, 1.0f);
            glLineWidth(2.0f);
            glBegin(GL_LINE_LOOP);
            glVertex2f(x0, y0); glVertex2f(x1, y0);
            glVertex2f(x1, y1); glVertex2f(x0, y1);
            glEnd();
            char dim[48];
            snprintf(dim, sizeof dim, "%d X %d", (int)(x1 - x0), (int)(y1 - y0));
            draw_string_at(x0 + 4.0f, (y0 > 16.0f ? y0 - 12.0f : y0 + 4.0f), dim, 2);
        }
        glfwSwapBuffers(g_rs.win);
    }

    bool ok = !g_rs.cancelled;
    int rx = 0, ry = 0, rw = 0, rh = 0;
    if (ok) {
        double x0 = g_rs.ax < g_rs.bx ? g_rs.ax : g_rs.bx;
        double x1 = g_rs.ax < g_rs.bx ? g_rs.bx : g_rs.ax;
        double y0 = g_rs.ay < g_rs.by ? g_rs.ay : g_rs.by;
        double y1 = g_rs.ay < g_rs.by ? g_rs.by : g_rs.ay;
        rx = g_rs.vx + (int)x0;
        ry = g_rs.vy + (int)y0;
        rw = (int)(x1 - x0);
        rh = (int)(y1 - y0);
        if (rw & 1) rw--;
        if (rh & 1) rh--;
        if (rw < 16 || rh < 16) ok = false;   /* stray click, not a drag */
    }

    if (cross) glfwDestroyCursor(cross);
    glfwDestroyWindow(g_rs.win);
    g_rs.win = NULL;
    glfwMakeContextCurrent(g.window);

    if (!ok) return false;
    *out_x = rx; *out_y = ry; *out_w = rw; *out_h = rh;
    return true;
}

static void start_recording_region(void) {
    if (g.state == ORB_RECORDING) { stop_recording(); return; }
    int rx, ry, rw, rh;
    if (!region_select(&rx, &ry, &rw, &rh)) {
        popup("NO BOX", "Region select cancelled.");
        return;
    }
    PlatMonitor m;
    m.x = rx; m.y = ry; m.w = rw; m.h = rh;
    snprintf(m.name, sizeof m.name, "Region %dx%d", rw, rh);
    snprintf(g.record_label, sizeof g.record_label, "Region_%dx%d", rw, rh);
    g.targetWindow = NULL;
    g.recordingMonitor = true;
    g.targetMonitor = m;
    compute_capture_size(rw, rh, &g.captureW, &g.captureH);
    if (!start_recording_common()) return;
    popup_mode("GIF", "REC BOX",
          "REC region %dx%d at (%d,%d) @ 10fps  (Esc or F6 to stop)",
          g.captureW, g.captureH, rx, ry);
}

static void start_recording_monitor(PlatMonitor m) {
    g.targetWindow = NULL;
    g.recordingMonitor = true;
    g.targetMonitor = m;
    sanitize_label(m.name, g.record_label, sizeof g.record_label);
    compute_capture_size(m.w, m.h, &g.captureW, &g.captureH);
    if (!start_recording_common()) return;
    popup_mode("GIF", "REC MON",
          "REC %s at %dx%d @ 10fps  (Esc/double-click to stop)",
          m.name, g.captureW, g.captureH);
}

/* Finish the GIF, then fire the SAVED popup, clipboard-copy, auto-open.
 * All main-thread work (no background thread, no signaling). */
static void stop_recording(void) {
    if (g.state != ORB_RECORDING) return;
    plat_unregister_hotkey(PLAT_HK_ESCAPE);
    OrbState next = g.hotkey_on[0] ? ORB_IDLE : ORB_NOCAPTURE;

    if (g.recordingVideo) {
        bool had_audio = plat_video_has_audio();
        plat_video_stop();
        if (g.recordingCamera) { plat_camera_close(); g.recordingCamera = false; }
        g.recordingVideo = false;
        int secs = (int)((plat_now_ms() - g.recordStartMs) / 1000);
        log_write("save", "video: %d frames, %ds, audio=%s -> %s",
                  g.nframes, secs, had_audio ? "yes" : "no", g.record_path);
        static const char* SRC[4] = { "no sound", "system sound",
                                      "microphone", "system + mic" };
        popup_mode("MP4", had_audio ? "SAVED" : "NO SOUND",
              "Saved %ds of video (%s) to %s",
              secs, had_audio ? SRC[g.audio_src & 3] : "no audio device",
              g.record_path);
        if (g.auto_clipboard)   plat_clipboard_copy_file(g.record_path);
        if (g.auto_open_folder) plat_open_folder_select(g.record_path);
        g.state = next;
        g.targetWindow = NULL;
        return;
    }

    if (!g.gw_open) {
        popup("EMPTY", "No frames captured.");
        g.state = next; g.targetWindow = NULL;
        return;
    }
    if (g.nframes == 0) {
        GifEnd(&g.gw);
        g.gw_open = false;
        remove(g.record_path);          /* clean up empty file */
        popup("EMPTY", "No frames captured.");
        g.state = next; g.targetWindow = NULL;
        return;
    }
    long fsize = ftell(g.gw.f);
    GifEnd(&g.gw);
    g.gw_open = false;

    log_write("save", "wrote %d frames to %s (%ld bytes)",
              g.nframes, g.record_path, fsize);
    popup_mode("GIF", "SAVED", "Saved %d frames to %s (%ld KB)",
          g.nframes, g.record_path, fsize / 1024);

    if (g.auto_clipboard)   plat_clipboard_copy_file(g.record_path);
    if (g.auto_open_folder) plat_open_folder_select(g.record_path);

    g.state = next;
    g.targetWindow = NULL;
}

/* Registered with plat_set_modal_tick(), so recording keeps taking frames
 * while a popup menu owns the thread. Only the recording tick -- redrawing
 * the orb from inside someone else's modal loop is asking for trouble, and
 * the orb is not what you are recording. */
static void modal_tick(void);
static void draw_orb_frame(void);

static void tick_recording(void) {
    if (g.state != ORB_RECORDING) return;
    uint64_t now = plat_now_ms();

    if (g.recordingVideo) {
        /* Paced against a running deadline rather than "now + interval", so
         * a slow frame does not permanently shift the clock and drift the
         * video away from the audio. */
        if (now < g.videoFrameDueMs) return;
        g.videoFrameDueMs += 1000 / VIDEO_FPS;
        if (g.videoFrameDueMs < now) g.videoFrameDueMs = now;   /* fell behind */

        uint8_t* vpx;
        if (g.recordingCamera) {
            vpx = plat_camera_read(g.captureW, g.captureH);
            if (!vpx) return;          /* no frame ready yet -- not an error */
        } else if (g.recordingMonitor) {
            vpx = plat_capture_rect(g.targetMonitor.x, g.targetMonitor.y,
                                    g.targetMonitor.w, g.targetMonitor.h,
                                    g.captureW, g.captureH);
        } else {
            int sw = 0, sh = 0;
            vpx = plat_capture_window(g.targetWindow, g.captureW, g.captureH,
                                      NULL, 0, &sw, &sh);
        }
        if (!vpx) {
            popup("GONE", "Target vanished -- saving what we have.");
            stop_recording();
            return;
        }
        plat_video_write_frame(vpx, now);
        free(vpx);
        g.nframes++;
        if (now - g.recordStartMs > (uint64_t)VIDEO_MAX_SEC * 1000) {
            popup("MAX", "Hit the %d-minute video cap.", VIDEO_MAX_SEC / 60);
            stop_recording();
        }
        return;
    }

    if (now - g.lastFrameMs < (uint64_t)(GIF_DELAY_10MS * 10)) return;

    uint8_t* px;
    if (g.recordingMonitor) {
        px = plat_capture_rect(g.targetMonitor.x, g.targetMonitor.y,
                               g.targetMonitor.w, g.targetMonitor.h,
                               g.captureW, g.captureH);
    } else {
        int sw = 0, sh = 0;
        px = plat_capture_window(g.targetWindow, g.captureW, g.captureH,
                                 NULL, 0, &sw, &sh);
    }
    if (!px) {
        popup("GONE", "Target vanished -- saving what we have.");
        stop_recording();
        return;
    }
    /* Stream this frame straight into the GIF file. */
    GifWriteFrame(&g.gw, px, g.captureW, g.captureH, GIF_DELAY_10MS);
    free(px);
    g.nframes++;
    g.lastFrameMs = now;

    /* Size-aware auto-stop: once the file crosses the ceiling, stop.
     * Keeps output inside Discord/GitHub limits.                     */
    long fsize = ftell(g.gw.f);
    if (fsize > MAX_GIF_BYTES) {
        popup("SIZE", "Hit %ld MB cap (Discord/GitHub-safe). Auto-stopping.",
              (long)MAX_GIF_BYTES / (1024 * 1024));
        stop_recording();
        return;
    }
    if (g.nframes >= MAX_FRAMES) {
        popup("MAX", "Hit %d-frame cap (30s). Auto-stopping.", MAX_FRAMES);
        stop_recording();
    }
}

/* ── orb rendering ────────────────────────────────────────────────────── */

/* The CORE carries the state; the cage is always the same translucent
 * white shell. Splitting them means the colour has one job and reads at a
 * glance, instead of the whole object changing hue.
 *
 *   orange  idle, F5 armed        red     recording
 *   blue    idle, F5 released     green   editor open / playing back
 *   yellow  armed, pick a window
 *
 * Editing is deliberately checked before idle but after recording: you can
 * have the editor open while a capture runs, and the capture matters more. */
static void state_color(float* r, float* gc, float* b) {
    if (g.state == ORB_RECORDING && g.recordingCamera)
                                  { *r = 0.20f; *gc = 0.85f; *b = 1.00f; return; }
    if (g.state == ORB_RECORDING && g.recordingVideo)
                                  { *r = 1.00f; *gc = 0.20f; *b = 0.85f; return; }
    if (g.state == ORB_RECORDING) { *r = 1.00f; *gc = 0.15f; *b = 0.15f; return; }
    if (g.state == ORB_ARMED)     { *r = 1.00f; *gc = 0.85f; *b = 0.25f; return; }
    if (g.ed_open)                { *r = 0.20f; *gc = 0.95f; *b = 0.35f; return; }
    if (g.state == ORB_NOCAPTURE) { *r = 0.30f; *gc = 0.55f; *b = 1.00f; return; }
    *r = 1.00f; *gc = 0.55f; *b = 0.10f;                       /* idle */
}

static void draw_orb(int win_w, int win_h) {
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Draw the orb into a fixed ORB_SIZE viewport placed where the orb
     * actually is. During a ping the window is much larger so the rings
     * have room; pinning the viewport keeps the orb the same size AND the
     * same screen pixel, instead of scaling it and hoping it lines up. */
    /* The orb is always centred in its (larger) window. */
    /* Derive the orb's drawn size from the FRAMEBUFFER, never from
     * g.orb_size.
     *
     * Those two update at different moments. g.orb_size changes the instant
     * the wheel turns; the framebuffer follows when the window manager gets
     * round to it, which can be a frame later. Mixing them -- an ORB_SIZE
     * viewport centred in a win_w framebuffer -- offsets the orb by
     * 1.5 * (old - new) in BOTH axes while they disagree, which is 9 px for a
     * 6 px size step and is exactly the dx == dy flick measured off Joe's
     * capture. The window was in the right place the whole time; the orb was
     * being painted in the wrong part of it.
     *
     * The window is always three times the orb, so w/3 IS the orb size for
     * whatever framebuffer we actually have. A late framebuffer now shows a
     * one-frame-old SIZE, which nobody can see, instead of a jump in
     * POSITION, which everybody can. */
    int draw_size = ORB_SIZE;
    if (draw_size < 1) draw_size = 1;
    glViewport((win_w - draw_size) / 2, (win_h - draw_size) / 2,
               draw_size, draw_size);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, 1.0, 0.1, 100.0);   /* always square */

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -3.0f);
    glRotatef(g.rotationAngle, 0.5f, 1.0f, 0.3f);

    float r, gc, b;
    state_color(&r, &gc, &b);

    float pulse = 1.0f;
    float scalePulse = 1.0f;
    if (g.state == ORB_RECORDING) {
        double t = (plat_now_ms() - g.recordStartMs) / 1000.0;
        pulse = 0.7f + 0.5f * (float)fabs(sin(t * 6.0));
        scalePulse = 1.0f + 0.10f * (float)sin(t * 6.0);
    } else if (g.state == ORB_ARMED) {
        double t = plat_now_ms() / 1000.0;
        pulse = 0.7f + 0.4f * (float)fabs(sin(t * 4.0));
    } else if (g.ed_open && g.ed_playing) {
        double t = plat_now_ms() / 1000.0;
        pulse = 0.8f + 0.2f * (float)fabs(sin(t * 3.0));   /* gentle breathe */
    }
    glScalef(scalePulse, scalePulse, scalePulse);

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glLineWidth(1.5f);
    /* Cage: translucent white shell, independent of state. Kept faint on
     * purpose -- at 0.45 the wireframe washed the core out and the orb read
     * as a white blob with no state at all. */
    glColor4f(1.0f, 1.0f, 1.0f, 0.28f * pulse);
    for (int i = 0; i <= 16; i++) {
        float lat = (float)(M_PI * (-0.5 + (double)i / 16));
        float z = sinf(lat) * 0.5f;
        float zr = cosf(lat) * 0.5f;
        glBegin(GL_LINE_LOOP);
        for (int j = 0; j <= 24; j++) {
            float lng = (float)(2.0 * M_PI * j / 24.0);
            glVertex3f(cosf(lng) * zr, sinf(lng) * zr, z);
        }
        glEnd();
    }
    for (int j = 0; j <= 24; j++) {
        float lng = (float)(2.0 * M_PI * j / 24.0);
        glBegin(GL_LINE_STRIP);
        for (int i = 0; i <= 16; i++) {
            float lat = (float)(M_PI * (-0.5 + (double)i / 16));
            float z = sinf(lat) * 0.5f;
            float zr = cosf(lat) * 0.5f;
            glVertex3f(cosf(lng) * zr, sinf(lng) * zr, z);
        }
        glEnd();
    }
    /* Core: full strength and a little larger, so the state colour is what
     * you actually see through the cage. */
    glColor4f(r, gc, b, 0.97f * pulse);
    GLUquadric* quad = gluNewQuadric();
    gluSphere(quad, 0.34f, 20, 20);
    gluDeleteQuadric(quad);

    g.rotationAngle += 0.6f;
    if (g.rotationAngle > 360.0f) g.rotationAngle -= 360.0f;
}

/* ── editor mode ────────────────────────────────────────────────────────
 * Drag a .gif onto the orb: window grows to EDITOR_W x EDITOR_H, shows
 * current frame in the main pane, thumbnail strip along the bottom.
 * Keys:  Left/Right = step frame     [ = trim in    ] = trim out
 *        Space      = play/pause     S = save trimmed copy
 *        Esc        = close editor (orb shrinks back)                */

static void ed_upload_main(int frame_idx) {
    if (g.ed_main_tex_frame == frame_idx) return;
    if (!g.ed_main_tex) {
        glGenTextures(1, &g.ed_main_tex);
        glBindTexture(GL_TEXTURE_2D, g.ed_main_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, g.ed_main_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g.ed_r.width, g.ed_r.height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, g.ed_r.frames[frame_idx].rgba);
    g.ed_main_tex_frame = frame_idx;
}

/* Downscale a single frame to (dst_w, THUMB_H) via nearest-neighbor sampling.
 * Caller must free the returned buffer. */
static uint8_t* ed_make_thumb(int frame_idx, int dst_w) {
    uint8_t* out = (uint8_t*)malloc(dst_w * THUMB_H * 4);
    if (!out) return NULL;
    const uint8_t* src = g.ed_r.frames[frame_idx].rgba;
    int sw = g.ed_r.width, sh = g.ed_r.height;
    for (int y = 0; y < THUMB_H; y++) {
        int sy = y * sh / THUMB_H;
        for (int x = 0; x < dst_w; x++) {
            int sx = x * sw / dst_w;
            int di = (y * dst_w + x) * 4;
            int si = (sy * sw + sx) * 4;
            out[di+0] = src[si+0];
            out[di+1] = src[si+1];
            out[di+2] = src[si+2];
            out[di+3] = 255;
        }
    }
    return out;
}

/* Forward decls: callbacks installed on the editor window. */
static void on_key (GLFWwindow* w, int key, int sc, int action, int mods);
static void on_drop(GLFWwindow* w, int count, const char* paths[]);
static void on_ed_move(GLFWwindow* w, int x, int y);
static void on_ed_size(GLFWwindow* w, int cw, int ch);
static void on_ed_mouse(GLFWwindow* w, int button, int action, int mods);
static void on_ed_cursor(GLFWwindow* w, double mx, double my);
static void on_ed_char(GLFWwindow* w, unsigned int cp);
static void an_reset(void);
static void an_autosave(void);
static void ed_draw_arrows(int w, int h);

#define ED_MAX_FILES 512

static void ed_free_playlist(void) {
    if (!g.ed_files) return;
    for (int i = 0; i < g.ed_nfiles; i++) free(g.ed_files[i]);
    free(g.ed_files);
    g.ed_files = NULL;
    g.ed_nfiles = 0;
    g.ed_file_idx = 0;
}

/* Load a still image into g.ed_r as a single-frame "animation".
 * Reusing GifReader lets the whole existing render path -- main pane,
 * texture upload, fit-to-window -- work unchanged for stills.
 *
 * stb_image first (full resolution, portable). If it cannot read the file,
 * ask the OS for a thumbnail: on Windows that covers WebP/HEIC/RAW/PSD and
 * anything else with a registered codec, at reduced size but visible. */
/* Decode a .webp into a malloc'd RGBA buffer, or NULL. */
static uint8_t* load_webp(const char* path, int* out_w, int* out_h) {
    simplewebp* sw = NULL;
    if (simplewebp_load_from_filename(path, NULL, &sw) != SIMPLEWEBP_NO_ERROR)
        return NULL;
    size_t w = 0, h = 0;
    simplewebp_get_dimensions(sw, &w, &h);
    if (w == 0 || h == 0) { simplewebp_unload(sw); return NULL; }
    uint8_t* buf = (uint8_t*)malloc(w * h * 4);
    if (!buf) { simplewebp_unload(sw); return NULL; }
    if (simplewebp_decode(sw, buf, NULL) != SIMPLEWEBP_NO_ERROR) {
        free(buf); simplewebp_unload(sw); return NULL;
    }
    simplewebp_unload(sw);
    *out_w = (int)w; *out_h = (int)h;
    return buf;
}

static bool path_has_ext(const char* path, const char* ext) {
    size_t n = strlen(path), m = strlen(ext);
    if (n < m) return false;
    const char* e = path + n - m;
    for (size_t i = 0; i < m; i++) {
        char a = e[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static bool ed_load_still(const char* path) {
    int w = 0, h = 0, comp = 0;
    uint8_t* px = NULL;
    if (path_has_ext(path, ".webp")) px = load_webp(path, &w, &h);
    if (!px) px = stbi_load(path, &w, &h, &comp, 4);
    bool from_shell = false;
    if (!px) {
        int tw = 0, th = 0;
        px = plat_shell_thumbnail(path, 1024, &tw, &th);
        if (!px) {
            log_write("view", "cannot decode %s (%s)", path, stbi_failure_reason());
            return false;
        }
        w = tw; h = th;
        from_shell = true;
    }
    memset(&g.ed_r, 0, sizeof g.ed_r);
    g.ed_r.width  = w;
    g.ed_r.height = h;
    g.ed_r.nframes = 1;
    g.ed_r.frames = (GifFrame*)calloc(1, sizeof(GifFrame));
    if (!g.ed_r.frames) { free(px); return false; }
    /* GifReaderClose() frees frames[i].rgba with free(), and both stb and the
     * shell path hand back malloc'd buffers, so ownership transfers cleanly. */
    g.ed_r.frames[0].rgba     = px;
    g.ed_r.frames[0].delay_ms = 100;
    g.ed_r.frames[0].disposal = 0;
    log_write("view", "loaded %dx%d %s%s", w, h, path,
              from_shell ? " (shell thumbnail)" : "");
    return true;
}

static void ed_close(void) {
    if (!g.ed_open) return;
    an_autosave();
    /* Textures live in the editor context -- make it current to delete them. */
    if (g.ed_window) {
        glfwMakeContextCurrent(g.ed_window);
        if (g.ed_main_tex) { glDeleteTextures(1, &g.ed_main_tex); g.ed_main_tex = 0; }
        if (g.ed_thumb_tex) {
            glDeleteTextures(g.ed_thumb_prev_count > 0 ? g.ed_thumb_prev_count : 1,
                             g.ed_thumb_tex);
            free(g.ed_thumb_tex); g.ed_thumb_tex = NULL;
        }
        g.ed_thumb_prev_count = 0;
        glfwDestroyWindow(g.ed_window);
        g.ed_window = NULL;
    } else {
        free(g.ed_thumb_tex); g.ed_thumb_tex = NULL;
        g.ed_main_tex = 0;
    }
    GifReaderClose(&g.ed_r);
    ed_free_playlist();
    an_reset();
    g.ed_open = false;
    g.ed_playing = false;
    g.ed_main_tex_frame = -1;
    /* The orb never changed size or state, so nothing to restore there. */
    glfwMakeContextCurrent(g.window);
    settings_mark_dirty();
    popup("CLOSED", "Editor closed.");
}

static bool ed_load_media(const char* path);   /* defined below */

/* How many cells the filmstrip shows: frames for an animation, sibling
 * files for a still. */
static int ed_strip_count(void) {
    /* Stills show no filmstrip -- the folder can hold hundreds of files and
     * thumbnailing them all to scrub with arrow keys is wasted work. The
     * strip belongs to animations, where the cells are frames. */
    if (g.ed_is_static) return 0;
    return g.ed_r.nframes;
}

/* Thumbnail for strip cell `i`, sized dst_w x THUMB_H, malloc'd RGBA.
 * Animation -> that frame. Still -> that sibling file, via the OS thumbnail
 * cache when available (Explorer already rendered these, so a folder you
 * have browsed loads essentially instantly) and stb_image otherwise. */
static uint8_t* ed_make_strip_thumb(int i, int dst_w) {
    return ed_make_thumb(i, dst_w);   /* animations only; see ed_strip_count */
}

/* (Re)build the filmstrip textures for the current media. */
static void ed_build_thumbs(int win_w) {
    int count = ed_strip_count();
    if (count < 1) {
        /* No strip for this media -- release any textures from the last one. */
        if (g.ed_thumb_tex) {
            if (g.ed_thumb_prev_count > 0)
                glDeleteTextures(g.ed_thumb_prev_count, g.ed_thumb_tex);
            free(g.ed_thumb_tex);
            g.ed_thumb_tex = NULL;
        }
        g.ed_thumb_prev_count = 0;
        return;
    }

    if (g.ed_thumb_tex) {
        glDeleteTextures(g.ed_thumb_prev_count > 0 ? g.ed_thumb_prev_count : count,
                         g.ed_thumb_tex);
        free(g.ed_thumb_tex);
        g.ed_thumb_tex = NULL;
    }

    int per_slot = win_w / count;
    if (per_slot < 8)  per_slot = 8;
    if (per_slot > 96) per_slot = 96;
    int aspect_w = g.ed_r.height > 0 ? THUMB_H * g.ed_r.width / g.ed_r.height : THUMB_H;
    if (aspect_w < 8) aspect_w = 8;
    g.ed_thumb_w = per_slot < aspect_w ? per_slot : aspect_w;

    g.ed_thumb_tex = (unsigned*)calloc(count, sizeof(unsigned));
    if (!g.ed_thumb_tex) { g.ed_thumb_prev_count = 0; return; }
    glGenTextures(count, g.ed_thumb_tex);
    g.ed_thumb_prev_count = count;

    for (int i = 0; i < count; i++) {
        uint8_t* tp = ed_make_strip_thumb(i, g.ed_thumb_w);
        if (!tp) continue;
        glBindTexture(GL_TEXTURE_2D, g.ed_thumb_tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g.ed_thumb_w, THUMB_H, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, tp);
        free(tp);
    }
}

/* Switch to another file in the folder, keeping the window as it is. */
static void ed_goto_file(int idx) {
    if (!g.ed_open || !g.ed_files || g.ed_nfiles <= 0) return;
    if (idx < 0) idx = g.ed_nfiles - 1;
    if (idx >= g.ed_nfiles) idx = 0;
    if (idx == g.ed_file_idx && g.ed_main_tex_frame >= 0) return;

    char next[512];
    strncpy(next, g.ed_files[idx], sizeof next - 1);
    next[sizeof next - 1] = 0;

    /* Before anything is unloaded. Sitting further down -- after
     * ed_load_media has already replaced g.ed_r -- it wrote the pixels of the
     * NEW image over the OLD image's path, which is worse than the lost marks
     * it was added to prevent. */
    an_autosave();

    glfwMakeContextCurrent(g.ed_window);
    GifReaderClose(&g.ed_r);
    if (!ed_load_media(next)) {
        popup("BAD FILE", "Could not decode: %s", next);
        return;
    }
    g.ed_file_idx = idx;
    strncpy(g.ed_path, next, sizeof g.ed_path - 1);
    g.ed_path[sizeof g.ed_path - 1] = 0;
    an_reset();
    g.ed_browsing = true;      /* you have left the capture and gone walking */

    g.ed_frame = 0;
    g.ed_trim_in = 0;
    g.ed_trim_out = g.ed_r.nframes - 1;
    g.ed_playing = false;
    g.ed_main_tex_frame = -1;
    g.ed_last_advance_ms = plat_now_ms();

    /* Only rebuild the strip for animations -- for stills it lists the
     * folder, which has not changed. */
    if (!g.ed_is_static) {
        int ww, wh; glfwGetWindowSize(g.ed_window, &ww, &wh);
        ed_build_thumbs(ww);
    }

    const char* base = strrchr(next, '\\');
    const char* b2   = strrchr(next, '/');
    if (b2 > base) base = b2;
    char title[600];
    snprintf(title, sizeof title, "%s - %s",
             g.ed_is_static ? "Image Editor" : "GIF Editor",
             base ? base + 1 : next);
    glfwSetWindowTitle(g.ed_window, title);
    glfwMakeContextCurrent(g.window);
}

/* True when the path ends in .gif (case-insensitive). */
static bool path_is_gif(const char* path) {
    size_t n = strlen(path);
    if (n < 4) return false;
    const char* e = path + n - 4;
    return e[0] == '.' &&
           (e[1]=='g'||e[1]=='G') && (e[2]=='i'||e[2]=='I') && (e[3]=='f'||e[3]=='F');
}

/* Load whatever `path` points at into g.ed_r. Animated GIFs keep their
 * frames; everything else becomes a single-frame still. Sets ed_is_static,
 * which decides what Left/Right mean: frames for an animation, files for
 * a still -- because a JPG has no frames to step through. */
static bool ed_load_media(const char* path) {
    if (path_is_gif(path)) {
        if (!GifReaderOpen(&g.ed_r, path)) {
            /* A .gif we cannot parse: still try the still-image path. */
            if (!ed_load_still(path)) return false;
            g.ed_is_static = true;
            return true;
        }
        g.ed_is_static = (g.ed_r.nframes <= 1);
        return true;
    }
    if (!ed_load_still(path)) return false;
    g.ed_is_static = true;
    return true;
}

/* Decide whether a file is safe to hand to an image decoder.
 *
 * A picture cannot execute anything by itself. The real risk is a DECODER
 * bug -- a crafted file that makes a parser write outside its buffer, at
 * which point the attacker's data becomes the program's control flow. Our own
 * GIF reader is bounds-checked at every write, but stb_image is a large,
 * general parser with a real CVE history, and it is the thing most likely to
 * be reached by a hostile file.
 *
 * So: check the file's actual CONTENT before any decoder sees it, rather than
 * trusting the extension. Renaming evil.exe to holiday.png gets it past an
 * extension check and straight into a parser; it does not get past this.
 *
 * Deliberately an ALLOW-list. A deny-list of known-bad signatures is a losing
 * game -- there are always more formats than you thought of. Anything not
 * positively recognised as an image is refused.
 *
 * Returns 0 if it looks like an image, or a short reason if not. */
static const char* image_gate(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return "could not be opened";

    /* Size first: a decoder that allocates from a header field can be asked
     * for absurd amounts, and exhausting memory is its own kind of attack --
     * we watched low memory take a display driver down on this machine. */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return "could not be read"; }
    long sz = ftell(f);
    if (sz < 0)  { fclose(f); return "could not be measured"; }
    if (sz < 12) { fclose(f); return "too small to be an image"; }
    if (sz > (long)(256 * 1024 * 1024)) {
        fclose(f);
        return "larger than 256 MB -- refusing to decode it";
    }

    unsigned char h[32] = {0};
    rewind(f);
    size_t got = fread(h, 1, sizeof h, f);
    fclose(f);
    if (got < 12) return "too short to identify";

    /* Executables and scripts, named explicitly so the refusal can say so.
     * They would fail the allow-list below anyway; this is for the message. */
    if (h[0]=='M' && h[1]=='Z')                       return "a Windows executable";
    if (h[0]==0x7F && h[1]=='E' && h[2]=='L' && h[3]=='F') return "a Linux executable";
    if (h[0]=='#' && h[1]=='!')                       return "a script";

    /* The allow-list. */
    if (!memcmp(h, "GIF87a", 6) || !memcmp(h, "GIF89a", 6))            return NULL;
    if (h[0]==0x89 && !memcmp(h+1, "PNG\r\n\x1a\n", 7))                return NULL;
    if (h[0]==0xFF && h[1]==0xD8 && h[2]==0xFF)                        return NULL; /* JPEG */
    if (h[0]=='B' && h[1]=='M')                                        return NULL; /* BMP  */
    if (!memcmp(h, "RIFF", 4) && !memcmp(h+8, "WEBP", 4))              return NULL;
    if (!memcmp(h, "II*\0", 4) || !memcmp(h, "MM\0*", 4))              return NULL; /* TIFF, and most camera RAW */
    if (!memcmp(h, "8BPS", 4))                                         return NULL; /* PSD  */
    if (!memcmp(h, "DDS ", 4))                                         return NULL;
    if (h[0]==0 && h[1]==0 && (h[2]==1 || h[2]==2) && h[3]==0)         return NULL; /* ICO/CUR */
    if (!memcmp(h+4, "ftyp", 4))                                       return NULL; /* HEIC/AVIF */
    if (!memcmp(h, "\0\0\0\0", 4) && !memcmp(h+4, "jP", 2))            return NULL; /* JPEG 2000 */
    if (h[0]=='P' && h[1]>='1' && h[1]<='7')                           return NULL; /* PNM family */
    if (!memcmp(h, "<?xml", 5) || !memcmp(h, "<svg", 4))               return NULL; /* SVG, Linux */

    /* TGA has no signature at the front -- it is identified by a footer, or
     * by its extension. Allowed only when the extension agrees, which keeps
     * the hole as small as the format requires. */
    {
        const char* dot = strrchr(path, '.');
        if (dot && (!strcmp(dot, ".tga") || !strcmp(dot, ".TGA"))) return NULL;
    }

    return "not a recognised image format";
}

static bool ed_open_path(const char* path) {
    if (g_no3d) {
        popup_mode("NO 3D", "CANNOT",
                   "No 3D detected -- the viewer and editor need it. "
                   "Recording still works.");
        log_write("no3d", "refused to open %s: no GL context", path);
        return false;
    }

    /* Nothing reaches a decoder until its content says it is an image. */
    {
        const char* why = image_gate(path);
        if (why) {
            popup_mode("BLOCKED", "NOT IMAGE", "Refused: %s.", why);
            log_write("gate", "refused %s -- %s", path, why);
            return false;
        }
    }
    an_autosave();
    if (g.ed_open) ed_close();
    if (!ed_load_media(path)) {
        popup("BAD FILE", "Could not decode: %s", path);
        return false;
    }
    strncpy(g.ed_path, path, sizeof g.ed_path - 1);
    g.ed_path[sizeof g.ed_path - 1] = 0;

    /* Everything alongside it, so Left/Right can walk the folder. */
    g.ed_files = (char**)calloc(ED_MAX_FILES, sizeof(char*));
    if (g.ed_files) {
        g.ed_nfiles = plat_list_sibling_images(path, g.ed_files, ED_MAX_FILES,
                                               &g.ed_file_idx);
        if (g.ed_nfiles == 0) { free(g.ed_files); g.ed_files = NULL; }
    }

    /* --- create the editor as a real, separate, decorated window --- */
    int win_w = g.have_ed_geom && g.ed_w > 200 ? g.ed_w : EDITOR_W;
    int win_h = g.have_ed_geom && g.ed_h > 150 ? g.ed_h : EDITOR_H;

    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);   /* real title bar: move/resize */
    glfwWindowHint(GLFW_FLOATING,  GLFW_FALSE);  /* NOT always-on-top */
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES,   0);

    char title[600];
    const char* base = strrchr(path, '\\');
    const char* base2 = strrchr(path, '/');
    if (base2 > base) base = base2;
    snprintf(title, sizeof title, "%s - %s",
             g.ed_is_static ? "Image Editor" : "GIF Editor",
             base ? base + 1 : path);

    g.ed_window = glfwCreateWindow(win_w, win_h, title, NULL, NULL);
    if (!g.ed_window) {
        GifReaderClose(&g.ed_r);
        popup("ED FAIL", "Could not create editor window.");
        glfwMakeContextCurrent(g.window);
        return false;
    }

    /* Restore remembered position, but only if it lands on a monitor that
     * exists right now (defends against the monitor-wake shuffle). */
    if (g.have_ed_geom && rect_on_live_monitor(g.ed_x, g.ed_y, win_w, win_h)) {
        glfwSetWindowPos(g.ed_window, g.ed_x, g.ed_y);
    } else if (g.have_ed_geom) {
        log_write("cfg", "editor pos (%d,%d) is off all live monitors -- centering",
                  g.ed_x, g.ed_y);
    }

    glfwSetKeyCallback      (g.ed_window, on_key);
    glfwSetCharCallback     (g.ed_window, on_ed_char);
    glfwSetCursorPosCallback(g.ed_window, on_ed_cursor);
    glfwSetMouseButtonCallback(g.ed_window, on_ed_mouse);
    glfwSetDropCallback     (g.ed_window, on_drop);
    plat_window_allow_drops (g.ed_window);
    glfwSetWindowPosCallback(g.ed_window, on_ed_move);
    glfwSetWindowSizeCallback(g.ed_window, on_ed_size);

    /* Editor GL state lives in the editor's own context. */
    glfwMakeContextCurrent(g.ed_window);
    glfwSwapInterval(1);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    an_reset();
    g.ed_browsing = true;
    g.ed_open = true;
    g.ed_frame = 0;
    g.ed_trim_in = 0;
    g.ed_trim_out = g.ed_r.nframes - 1;
    g.ed_playing = false;
    g.ed_main_tex_frame = -1;
    g.ed_last_advance_ms = plat_now_ms();

    ed_build_thumbs(win_w);

    /* Record actual geometry now that the WM has placed it. */
    glfwGetWindowPos (g.ed_window, &g.ed_x, &g.ed_y);
    glfwGetWindowSize(g.ed_window, &g.ed_w, &g.ed_h);
    g.have_ed_geom = true;
    settings_mark_dirty();

    /* Orb stays a dime and keeps its own state; put its context back. */
    glfwMakeContextCurrent(g.window);
    popup("EDIT",
          "Editor: %s (%dx%d, %d frames). Arrows/[]/space/S/Esc",
          path, g.ed_r.width, g.ed_r.height, g.ed_r.nframes);
    return true;
}

/* Save the trimmed range [trim_in..trim_out] as a new GIF next to the source. */
static void ed_save_trimmed(void) {
    if (!g.ed_open) return;
    int a = g.ed_trim_in, b = g.ed_trim_out;
    if (a > b) { int t = a; a = b; b = t; }
    if (a < 0) a = 0;
    if (b >= g.ed_r.nframes) b = g.ed_r.nframes - 1;
    int n = b - a + 1;
    if (n <= 0) { popup("EMPTY TRIM", "Trim range is empty."); return; }

    /* Compose output path: same folder, _trim<a>_<b>.gif suffix. */
    char out[600];
    /* strip .gif extension if present */
    char base[512]; strncpy(base, g.ed_path, sizeof base - 1); base[sizeof base - 1] = 0;
    char* dot = strrchr(base, '.');
    if (dot && (dot[1]=='g'||dot[1]=='G')) *dot = 0;
    snprintf(out, sizeof out, "%s_trim_%d_%d.gif", base, a, b);

    GifWriter gw;
    if (!GifBegin(&gw, out, g.ed_r.width, g.ed_r.height, GIF_DELAY_10MS)) {
        popup("SAVE ERR", "Failed to write %s", out);
        return;
    }
    for (int i = a; i <= b; i++) {
        int d = g.ed_r.frames[i].delay_ms / 10;
        if (d < 2) d = 10;
        GifWriteFrame(&gw, g.ed_r.frames[i].rgba,
                      g.ed_r.width, g.ed_r.height, d);
    }
    GifEnd(&gw);
    log_write("editor", "saved trimmed %d frames [%d..%d] to %s", n, a, b, out);
    popup("TRIMMED", "Saved %d frames to %s", n, out);
    if (g.auto_clipboard)   plat_clipboard_copy_file(out);
    if (g.auto_open_folder) plat_open_folder_select(out);
}

/* Advance playback if playing. */
static void ed_tick(void) {
    if (!g.ed_open || !g.ed_playing) return;
    uint64_t now = plat_now_ms();
    int cur_delay = g.ed_r.frames[g.ed_frame].delay_ms;
    if (cur_delay < 20) cur_delay = 100;
    if (now - g.ed_last_advance_ms >= (uint64_t)cur_delay) {
        g.ed_frame++;
        if (g.ed_frame > g.ed_trim_out) g.ed_frame = g.ed_trim_in;
        g.ed_last_advance_ms = now;
    }
}

/* One place decides what "previous/next" means, so the arrow buttons and the
 * arrow keys can never drift apart: frames in an animation, files in a
 * still. */
static void ed_step(int dir) {
    if (!g.ed_open) return;
    if (g.ed_is_static) {
        ed_goto_file(g.ed_file_idx + dir);
        return;
    }
    if (g.ed_r.nframes <= 0) return;
    g.ed_frame = (g.ed_frame + dir + g.ed_r.nframes) % g.ed_r.nframes;
    g.ed_playing = false;
}

/* ── annotation ─────────────────────────────────────────────────────────
 * A screenshot is almost never finished when it is taken. Something in it
 * wants an arrow, a box, or an account number blacked out, and every one of
 * those is thirty seconds spent in somebody else's program.
 *
 * The model is deliberately NOT Greenshot's. Greenshot keeps each annotation
 * as a live object you can select and move afterwards, which is lovely and
 * costs a selection model, hit-testing, drag handles, z-order and a document
 * format. Here a stroke is rasterised into the image the moment you let go of
 * the mouse, and "I want that arrow somewhere else" is served by undo. One
 * pixel buffer is the whole document.
 *
 * What that buys, beyond the code that does not exist: what you see is
 * literally what you get. The preview during a drag is produced by the same
 * paint.h call that commits it, into the same buffer, so the saved PNG cannot
 * disagree with the screen. An editor that previews on the GPU and saves on
 * the CPU has two rasterisers, and eventually two answers.
 */

typedef enum {
    TOOL_ARROW, TOOL_BOX, TOOL_ELLIPSE, TOOL_LINE, TOOL_PEN,
    TOOL_MARK, TOOL_PIX, TOOL_TEXT, TOOL_NUM, TOOL_CROP, TOOL_COUNT
} EdTool;

static const char* TOOL_NAME[TOOL_COUNT] =
    { "ARR", "BOX", "ELL", "LIN", "PEN", "MRK", "PIX", "TXT", "NUM", "CRP" };

/* Eight colours, not a picker. A picker is a dialog, and the point of this
 * program is that nothing is a dialog. Red first, because it is red nine
 * times out of ten. */
#define AN_NCOLOR 8
static const uint32_t AN_COLOR[AN_NCOLOR] = {
    0xE02020, 0xFFD400, 0x20C040, 0x2090FF,
    0xFF20C0, 0xFFFFFF, 0x101010, 0xFF8000
};

/* Undo keeps the RECTANGLE a step disturbed, not the whole picture.
 *
 * The first version snapshotted the entire image per step, on the reasoning
 * that screenshots are small. A region grab is; a 4K screen is 33 MB, and
 * twelve of those on the undo stack plus twelve more on the redo stack is
 * 800 MB for having drawn twelve arrows. An arrow disturbs perhaps 40 KB.
 *
 * So a step stores its bounding box and the pixels that were under it. Crop
 * is the one operation that changes the image's dimensions, and it keeps a
 * whole-image entry -- flagged by w == 0 -- which is both rare and the only
 * case that genuinely needs one. */
#define AN_UNDO_MAX   24
#define AN_UNDO_BYTES (192u * 1024u * 1024u)   /* ceiling for both stacks */

typedef struct {
    int      x, y, w, h;     /* w == 0: whole-image entry (a crop) */
    int      img_w, img_h;   /* dimensions that entry restores to */
    uint8_t* px;
} AnStep;

#define AN_BAR_H    30
#define AN_BTN_W    44
#define AN_SW_W     20

static struct {
    EdTool   tool;
    int      color_idx;
    int      thick;
    int      step_next;          /* next NUM counter value */

    int      vx, vy, vw, vh;     /* where the image sits on screen */

    bool     dragging;
    int      x0, y0, x1, y1;     /* drag, in IMAGE pixels */
    int      px0, py0, px1, py1; /* previous preview bbox, for the dirty rect */
    uint8_t* preview;            /* committed image + the stroke in flight */
    int      pw, ph;

    bool     typing;
    char     text[80];
    int      tx, ty;             /* text anchor, in IMAGE pixels */

    AnStep   undo[AN_UNDO_MAX], redo[AN_UNDO_MAX];
    int      undo_n, redo_n;

    /* A pen stroke's extent is not known until the mouse comes up, so the
     * whole image is held for the duration and trimmed to the strokes's
     * bounding box at the end. One transient copy, not one per step. */
    uint8_t* pen_hold;
    int      pen_x0, pen_y0, pen_x1, pen_y1;

    bool     dirty;
} an;

static uint8_t* an_img(void) {
    return g.ed_r.nframes > 0 ? g.ed_r.frames[0].rgba : NULL;
}
static PaintImg an_target(void) {
    PaintImg im; im.px = an_img(); im.w = g.ed_r.width; im.h = g.ed_r.height;
    return im;
}
static uint32_t an_rgb(void) { return AN_COLOR[an.color_idx]; }
static bool an_active(void)  { return g.ed_open && g.ed_is_static && an_img(); }

static void an_free_stack(AnStep* st, int* n) {
    for (int i = 0; i < *n; i++) { free(st[i].px); st[i].px = NULL; }
    *n = 0;
}

static size_t an_step_bytes(const AnStep* s) {
    return (size_t)(s->w ? s->w : s->img_w) * (s->w ? s->h : s->img_h) * 4;
}

static void an_reset(void) {
    an_free_stack(an.undo, &an.undo_n);
    an_free_stack(an.redo, &an.redo_n);
    free(an.pen_hold); an.pen_hold = NULL;
    free(an.preview); an.preview = NULL; an.pw = an.ph = 0;
    an.dragging = false;
    an.typing   = false;
    an.text[0]  = 0;
    an.step_next = 1;
    an.dirty = false;
    if (an.thick < 1) an.thick = 3;
}

static void an_retexture(void) { g.ed_main_tex_frame = -1; }

static void an_swap_image(uint8_t* px, int w, int h) {
    free(g.ed_r.frames[0].rgba);
    g.ed_r.frames[0].rgba = px;
    g.ed_r.width = w; g.ed_r.height = h;
    free(an.preview); an.preview = NULL; an.pw = an.ph = 0;
    an_retexture();
}

/* Copy a region out of the current image. w == 0 asks for the whole thing. */
static bool an_grab(AnStep* out, int x, int y, int w, int h) {
    uint8_t* src = an_img();
    if (!src) return false;
    int W = g.ed_r.width, H = g.ed_r.height;
    out->img_w = W; out->img_h = H;
    if (w <= 0) {                                   /* whole image */
        out->x = out->y = out->w = out->h = 0;
        out->px = (uint8_t*)malloc((size_t)W * H * 4);
        if (!out->px) return false;
        memcpy(out->px, src, (size_t)W * H * 4);
        return true;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return false;
    out->x = x; out->y = y; out->w = w; out->h = h;
    out->px = (uint8_t*)malloc((size_t)w * h * 4);
    if (!out->px) return false;
    for (int r = 0; r < h; r++)
        memcpy(out->px + (size_t)r * w * 4,
               src + ((size_t)(y + r) * W + x) * 4, (size_t)w * 4);
    return true;
}

static void an_restore(const AnStep* s) {
    if (s->w == 0) {                                /* a crop, going back */
        uint8_t* cp = (uint8_t*)malloc((size_t)s->img_w * s->img_h * 4);
        if (!cp) return;
        memcpy(cp, s->px, (size_t)s->img_w * s->img_h * 4);
        an_swap_image(cp, s->img_w, s->img_h);
        return;
    }
    uint8_t* dst = an_img();
    if (!dst) return;
    int W = g.ed_r.width;
    for (int r = 0; r < s->h; r++)
        memcpy(dst + ((size_t)(s->y + r) * W + s->x) * 4,
               s->px + (size_t)r * s->w * 4, (size_t)s->w * 4);
    an_retexture();
}

/* Drop the oldest entries until the two stacks fit the ceiling. Whole-image
 * entries are the only ones large enough to matter, and a run of crops is the
 * only way to accumulate them. */
static void an_trim(AnStep* st, int* n) {
    size_t total = 0;
    for (int i = 0; i < an.undo_n; i++) total += an_step_bytes(&an.undo[i]);
    for (int i = 0; i < an.redo_n; i++) total += an_step_bytes(&an.redo[i]);
    while (*n > 1 && total > AN_UNDO_BYTES) {
        total -= an_step_bytes(&st[0]);
        free(st[0].px);
        memmove(st, st + 1, sizeof(AnStep) * (size_t)(*n - 1));
        (*n)--;
    }
}

static void an_push_step(AnStep s) {
    if (an.undo_n == AN_UNDO_MAX) {
        free(an.undo[0].px);
        memmove(an.undo, an.undo + 1, sizeof(AnStep) * (AN_UNDO_MAX - 1));
        an.undo_n--;
    }
    an.undo[an.undo_n++] = s;
    an_free_stack(an.redo, &an.redo_n);             /* a new edit forks time */
    an_trim(an.undo, &an.undo_n);
    an.dirty = true;
}

/* Record the pixels a step is about to disturb. */
static void an_push_rect(int x0, int y0, int x1, int y1) {
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    AnStep s;
    if (an_grab(&s, x0, y0, x1 - x0 + 1, y1 - y0 + 1)) an_push_step(s);
}

static void an_push_full(void) {
    AnStep s;
    if (an_grab(&s, 0, 0, 0, 0)) an_push_step(s);
}

static void an_move(AnStep* from, int* fn, AnStep* to, int* tn) {
    AnStep cur;
    const AnStep* s = &from[*fn - 1];
    bool have = s->w ? an_grab(&cur, s->x, s->y, s->w, s->h)
                     : an_grab(&cur, 0, 0, 0, 0);
    an_restore(s);
    free(from[*fn - 1].px);
    (*fn)--;
    if (have) {
        if (*tn == AN_UNDO_MAX) {
            free(to[0].px);
            memmove(to, to + 1, sizeof(AnStep) * (AN_UNDO_MAX - 1));
            (*tn)--;
        }
        to[(*tn)++] = cur;
    }
    an.dirty = true;
}

static void an_undo(void) {
    if (an.undo_n <= 0) { popup_mode("PNG", "UNDO", "Nothing to undo."); return; }
    an_move(an.undo, &an.undo_n, an.redo, &an.redo_n);
}

static void an_redo(void) {
    if (an.redo_n <= 0) { popup_mode("PNG", "REDO", "Nothing to redo."); return; }
    an_move(an.redo, &an.redo_n, an.undo, &an.undo_n);
}

/* ---- text -------------------------------------------------------------
 * The built-in 5x7 font is uppercase-only and looks like a calculator. Fine
 * for a toolbar; wrong for a label you are putting on a screenshot and
 * sending to somebody. plat_render_text hands back a coverage mask from the
 * real system font, and paint_mask does not care which one produced it. */
static void an_stamp_text(PaintImg im, int x, int y, const char* s,
                          int px_height, uint32_t rgb) {
    int mw = 0, mh = 0;
    uint8_t* mask = plat_render_text(s, px_height, &mw, &mh);
    if (mask) {
        paint_mask(im, x, y, mask, mw, mh, rgb);
        free(mask);
        return;
    }
    int scale = px_height / 7; if (scale < 1) scale = 1;
    for (const char* p = s; *p; p++) {
        unsigned char uc = (unsigned char)*p;
        if (uc >= 'a' && uc <= 'z') uc = (unsigned char)(uc - 'a' + 'A');
        if (uc < 128) {
            for (int r = 0; r < 7; r++)
                for (int c = 0; c < 5; c++)
                    if (GLYPH[uc][r] & (0x10 >> c))
                        paint_rect(im, x + c * scale, y + r * scale,
                                   x + c * scale + scale - 1,
                                   y + r * scale + scale - 1, rgb, 1, true);
        }
        x += 6 * scale;
    }
}

static int an_text_px(void) { return an.thick * 5 + 12; }

/* ---- strokes ---------------------------------------------------------- */

/* Draw the tool into `im`. Used for BOTH the live preview and the commit, so
 * the two cannot drift apart. */
static void an_stroke(PaintImg im, EdTool tool, int x0, int y0, int x1, int y1) {
    uint32_t c = an_rgb();
    switch (tool) {
    case TOOL_ARROW:   paint_arrow(im, x0, y0, x1, y1, c, an.thick); break;
    case TOOL_LINE:
    case TOOL_PEN:     paint_line(im, x0, y0, x1, y1, c, an.thick); break;
    case TOOL_BOX:     paint_rect(im, x0, y0, x1, y1, c, an.thick, false); break;
    case TOOL_ELLIPSE: paint_ellipse(im, x0, y0, x1, y1, c, an.thick, false); break;
    case TOOL_MARK:    paint_highlight(im, x0, y0, x1, y1, c); break;
    case TOOL_PIX:     paint_pixelate(im, x0, y0, x1, y1, an.thick * 3 + 4); break;
    case TOOL_NUM: {
        int r = an.thick * 3 + 8;
        paint_disc(im, x0, y0, r, c);
        char lbl[8]; snprintf(lbl, sizeof lbl, "%d", an.step_next);
        int h = r + 2;
        an_stamp_text(im, x0 - (int)strlen(lbl) * h / 4, y0 - h / 2,
                      lbl, h, 0xFFFFFF);
        break;
    }
    default: break;    /* CROP resizes on release; TEXT comes from typing */
    }
}

static void an_bbox(int x0, int y0, int x1, int y1,
                    int* bx0, int* by0, int* bx1, int* by1) {
    int pad = an.thick * 6 + 24;          /* arrow heads and discs overhang */
    *bx0 = (x0 < x1 ? x0 : x1) - pad;
    *bx1 = (x0 > x1 ? x0 : x1) + pad;
    *by0 = (y0 < y1 ? y0 : y1) - pad;
    *by1 = (y0 > y1 ? y0 : y1) + pad;
}

/* Rebuild the preview inside the dirty rect only.
 *
 * The naive version -- copy the whole image, draw, upload the whole image --
 * is a 14 MB memcpy and a 14 MB texture upload per mouse-move event on a
 * 1440p screenshot, which turns a smooth drag into a slideshow. Restoring and
 * uploading the union of the last and current bounding boxes keeps it instant
 * whatever the image size. */
static void an_preview_update(void) {
    uint8_t* src = an_img();
    if (!src) return;
    int W = g.ed_r.width, H = g.ed_r.height;
    size_t n = (size_t)W * H * 4;
    if (!an.preview || an.pw != W || an.ph != H) {
        free(an.preview);
        an.preview = (uint8_t*)malloc(n);
        if (!an.preview) return;
        memcpy(an.preview, src, n);
        an.pw = W; an.ph = H;
        an.px0 = 0; an.py0 = 0; an.px1 = W - 1; an.py1 = H - 1;
    }

    int bx0, by0, bx1, by1;
    an_bbox(an.x0, an.y0, an.x1, an.y1, &bx0, &by0, &bx1, &by1);
    /* Union with the previous box, or last frame's shape stays painted on the
     * preview after the mouse has moved away from it. */
    int ux0 = bx0 < an.px0 ? bx0 : an.px0, uy0 = by0 < an.py0 ? by0 : an.py0;
    int ux1 = bx1 > an.px1 ? bx1 : an.px1, uy1 = by1 > an.py1 ? by1 : an.py1;
    if (an.tool == TOOL_CROP) { ux0 = 0; uy0 = 0; ux1 = W - 1; uy1 = H - 1; }
    if (ux0 < 0) ux0 = 0;
    if (uy0 < 0) uy0 = 0;
    if (ux1 > W - 1) ux1 = W - 1;
    if (uy1 > H - 1) uy1 = H - 1;
    if (ux1 < ux0 || uy1 < uy0) return;

    for (int y = uy0; y <= uy1; y++)
        memcpy(an.preview + ((size_t)y * W + ux0) * 4,
               src        + ((size_t)y * W + ux0) * 4,
               (size_t)(ux1 - ux0 + 1) * 4);

    PaintImg pim; pim.px = an.preview; pim.w = W; pim.h = H;
    if (an.tool == TOOL_CROP) {
        /* Show what survives by dimming what does not: reads better than an
         * outline on a busy screenshot. */
        int cx0 = an.x0 < an.x1 ? an.x0 : an.x1, cx1 = an.x0 > an.x1 ? an.x0 : an.x1;
        int cy0 = an.y0 < an.y1 ? an.y0 : an.y1, cy1 = an.y0 > an.y1 ? an.y0 : an.y1;
        for (int y = uy0; y <= uy1; y++) {
            uint8_t* p = an.preview + ((size_t)y * W + ux0) * 4;
            for (int x = ux0; x <= ux1; x++, p += 4) {
                if (x >= cx0 && x <= cx1 && y >= cy0 && y <= cy1) continue;
                p[0] = (uint8_t)(p[0] / 3);
                p[1] = (uint8_t)(p[1] / 3);
                p[2] = (uint8_t)(p[2] / 3);
            }
        }
        paint_rect(pim, cx0, cy0, cx1, cy1, 0xFFFFFF, 1, false);
    } else {
        an_stroke(pim, an.tool, an.x0, an.y0, an.x1, an.y1);
    }

    an.px0 = bx0; an.py0 = by0; an.px1 = bx1; an.py1 = by1;

    if (g.ed_main_tex) {
        glBindTexture(GL_TEXTURE_2D, g.ed_main_tex);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, W);
        glTexSubImage2D(GL_TEXTURE_2D, 0, ux0, uy0,
                        ux1 - ux0 + 1, uy1 - uy0 + 1,
                        GL_RGBA, GL_UNSIGNED_BYTE,
                        an.preview + ((size_t)uy0 * W + ux0) * 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
}

static void an_commit(void) {
    if (an.tool == TOOL_CROP) {
        int cw = 0, ch = 0;
        uint8_t* out = paint_crop(an_target(), an.x0, an.y0, an.x1, an.y1, &cw, &ch);
        if (!out || cw < 4 || ch < 4) { free(out); an_retexture(); return; }
        an_push_full();          /* the one step that changes the dimensions */
        an_swap_image(out, cw, ch);
        popup_mode("PNG", "CROP", "Cropped to %dx%d.", cw, ch);
        return;
    }
    int bx0, by0, bx1, by1;
    an_bbox(an.x0, an.y0, an.x1, an.y1, &bx0, &by0, &bx1, &by1);
    an_push_rect(bx0, by0, bx1, by1);
    an_stroke(an_target(), an.tool, an.x0, an.y0, an.x1, an.y1);
    if (an.tool == TOOL_NUM) an.step_next++;
    an_retexture();
}

static void an_commit_text(void) {
    if (!an.text[0]) { an.typing = false; return; }
    int px = an_text_px();
    /* A generous superset: no glyph advances further than its own height, so
     * this cannot be narrower than the text that lands. Undoing a box too
     * large is free; undoing one too small leaves fragments behind. */
    an_push_rect(an.tx - 2, an.ty - 2,
                 an.tx + (int)strlen(an.text) * px + px,
                 an.ty + px * 2);
    an_stamp_text(an_target(), an.tx, an.ty, an.text, an_text_px(), an_rgb());
    an.typing = false;
    an.text[0] = 0;
    an_retexture();
}

/* ---- output ----------------------------------------------------------- */

static void an_copy(void) {
    uint8_t* px = an_img();
    if (!px) return;
    plat_clipboard_copy_image(px, g.ed_r.width, g.ed_r.height);
    popup_mode("PNG", "COPIED", "%dx%d on the clipboard.",
               g.ed_r.width, g.ed_r.height);
}

/* Save over the file that is open when it is a PNG, and beside it otherwise.
 * Overwriting is right here in a way it would not be in a general image
 * editor: this file is thirty seconds old, this program made it, and the
 * version anyone wants is the annotated one. */
static void an_save(void) {
    uint8_t* px = an_img();
    if (!px) return;
    char out[600];
    snprintf(out, sizeof out, "%s", g.ed_path);
    if (!path_has_ext(g.ed_path, ".png")) {
        char* dot = strrchr(out, '.');
        if (dot) *dot = 0;
        strncat(out, "_edited.png", sizeof out - strlen(out) - 1);
    }
    if (!png_write_rgba(out, px, g.ed_r.width, g.ed_r.height)) {
        popup_mode("PNG", "SAVE ERR", "Could not write %s", out);
        return;
    }
    an.dirty = false;
    log_write("edit", "saved %dx%d -> %s", g.ed_r.width, g.ed_r.height, out);
    popup_mode("PNG", "SAVED", "Saved %dx%d to %s",
               g.ed_r.width, g.ed_r.height, out);
    if (g.auto_clipboard) plat_clipboard_copy_file(out);
}

/* Annotations are never silently discarded.
 *
 * Take a screenshot while the editor is open with an unsaved arrow on it, or
 * page to the next file, and the old image is thrown away -- along with the
 * work. Prompting is not an option in a program whose whole premise is that
 * nothing is a dialog, and the right answer is not in doubt anyway: Ctrl+S
 * already overwrites, the file is a capture from a minute ago, and the
 * version anybody wants is the annotated one. So it just saves. */
static void an_autosave(void) {
    if (!an.dirty || !g.ed_open || !g.ed_is_static || !an_img()) return;
    an_save();
    log_write("edit", "auto-saved unsaved marks before leaving %s", g.ed_path);
}

/* ---- toolbar ----------------------------------------------------------
 * Hit regions are computed in one place and used by both the drawing and the
 * clicking, so a button cannot end up somewhere other than where it looks. */
typedef struct { int x, w, kind, idx; } AnHit;   /* kind 0=tool 1=colour 2=act */

static int an_layout(AnHit* out, int max) {
    int n = 0, x = 6;
    for (int i = 0; i < TOOL_COUNT && n < max; i++, n++) {
        out[n].x = x; out[n].w = AN_BTN_W; out[n].kind = 0; out[n].idx = i;
        x += AN_BTN_W + 2;
    }
    x += 10;
    for (int i = 0; i < AN_NCOLOR && n < max; i++, n++) {
        out[n].x = x; out[n].w = AN_SW_W; out[n].kind = 1; out[n].idx = i;
        x += AN_SW_W + 2;
    }
    x += 10;
    for (int i = 0; i < 5 && n < max; i++, n++) {      /* - + UND CPY SAV */
        int w = (i < 2) ? 20 : AN_BTN_W;
        out[n].x = x; out[n].w = w; out[n].kind = 2; out[n].idx = i;
        x += w + 2;
    }
    return n;
}

static void an_draw_bar(int win_w) {
    glColor4f(0.12f, 0.12f, 0.15f, 1.0f);
    glBegin(GL_QUADS);
    glVertex2f(0, 0); glVertex2f((float)win_w, 0);
    glVertex2f((float)win_w, (float)AN_BAR_H); glVertex2f(0, (float)AN_BAR_H);
    glEnd();

    AnHit hit[40];
    int n = an_layout(hit, 40);
    for (int i = 0; i < n; i++) {
        int x = hit[i].x, w = hit[i].w;
        bool sel = (hit[i].kind == 0 && hit[i].idx == (int)an.tool) ||
                   (hit[i].kind == 1 && hit[i].idx == an.color_idx);
        if (hit[i].kind == 1) {
            uint32_t c = AN_COLOR[hit[i].idx];
            glColor4f(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f,
                      (c & 0xFF) / 255.0f, 1.0f);
        } else {
            glColor4f(sel ? 0.95f : 0.22f, sel ? 0.55f : 0.22f,
                      sel ? 0.12f : 0.26f, 1.0f);
        }
        glBegin(GL_QUADS);
        glVertex2f((float)x, 5); glVertex2f((float)(x + w), 5);
        glVertex2f((float)(x + w), (float)(AN_BAR_H - 4));
        glVertex2f((float)x, (float)(AN_BAR_H - 4));
        glEnd();

        if (hit[i].kind != 1) {
            const char* lbl;
            char tmp[8];
            if (hit[i].kind == 0) lbl = TOOL_NAME[hit[i].idx];
            else {
                static const char* ACT[5] = { "-", "+", "UND", "CPY", "SAV" };
                lbl = ACT[hit[i].idx];
                if (hit[i].idx == 1) {          /* the + key shows the size */
                    snprintf(tmp, sizeof tmp, "%d", an.thick);
                    lbl = tmp;
                }
            }
            glColor4f(1, 1, 1, 1);
            int tw = (int)strlen(lbl) * 12;
            draw_string_at((float)(x + (w - tw) / 2 + 1), 10.0f, lbl, 2);
        }
        if (sel) {
            glColor4f(1.0f, 0.75f, 0.2f, 1.0f);
            glLineWidth(2.0f);
            glBegin(GL_LINE_LOOP);
            glVertex2f((float)x - 1, 3); glVertex2f((float)(x + w + 1), 3);
            glVertex2f((float)(x + w + 1), (float)(AN_BAR_H - 2));
            glVertex2f((float)x - 1, (float)(AN_BAR_H - 2));
            glEnd();
        }
    }
}

static bool an_bar_click(int mx, int my) {
    if (my < 0 || my >= AN_BAR_H) return false;
    AnHit hit[40];
    int n = an_layout(hit, 40);
    for (int i = 0; i < n; i++) {
        if (mx < hit[i].x || mx >= hit[i].x + hit[i].w) continue;
        if      (hit[i].kind == 0) an.tool = (EdTool)hit[i].idx;
        else if (hit[i].kind == 1) an.color_idx = hit[i].idx;
        else switch (hit[i].idx) {
            case 0: if (an.thick > 1)  an.thick--; break;
            case 1: if (an.thick < 20) an.thick++; break;
            case 2: an_undo(); break;
            case 3: an_copy(); break;
            case 4: an_save(); break;
        }
        return true;
    }
    return true;              /* the bar swallows clicks that miss a button */
}

/* Window pixels to image pixels. Deliberately NOT clamped to the view: a drag
 * past the edge should clip the shape at the border, which paint.h already
 * does, rather than drag the corner back inside. */
static bool an_to_image(int mx, int my, int* ix, int* iy) {
    if (an.vw <= 0 || an.vh <= 0) return false;
    int rx = mx - an.vx, ry = my - an.vy;
    *ix = (int)((int64_t)rx * g.ed_r.width  / an.vw);
    *iy = (int)((int64_t)ry * g.ed_r.height / an.vh);
    return rx >= 0 && ry >= 0 && rx < an.vw && ry < an.vh;
}

static void ed_draw(int win_w, int win_h) {
    glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!g.ed_open) return;

    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0, win_w, win_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glDisable(GL_DEPTH_TEST);

    /* Main pane area: everything between the toolbar and the thumbnail strip.
     * The toolbar only exists for stills -- there is nothing to annotate on a
     * frame of an animation that the next frame will not throw away. */
    int strip_h = (ed_strip_count() > 0) ? (THUMB_H + 6) : 0;
    int btn_h   = 24;
    int bar_h   = an_active() ? AN_BAR_H : 0;
    int main_area_h = win_h - strip_h - btn_h - bar_h;
    int main_area_top = bar_h;
    if (main_area_h < 40) main_area_h = 40;

    /* Fit main frame with aspect. */
    int mw = g.ed_r.width, mh = g.ed_r.height;
    double sx = (double)(win_w - 20) / mw;
    double sy = (double)(main_area_h - 20) / mh;
    double s = sx < sy ? sx : sy;
    int draw_w = (int)(mw * s);
    int draw_h = (int)(mh * s);
    int draw_x = (win_w - draw_w) / 2;
    int draw_y = main_area_top + (main_area_h - draw_h) / 2;

    /* Publish where the image landed, so the mouse handler maps clicks with
     * the same numbers that drew it rather than recomputing the layout and
     * getting it subtly different. */
    an.vx = draw_x; an.vy = draw_y; an.vw = draw_w; an.vh = draw_h;

    ed_upload_main(g.ed_frame);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g.ed_main_tex);
    glColor4f(1,1,1,1);
    glBegin(GL_QUADS);
    glTexCoord2f(0,0); glVertex2f((float)draw_x,          (float)draw_y);
    glTexCoord2f(1,0); glVertex2f((float)(draw_x+draw_w), (float)draw_y);
    glTexCoord2f(1,1); glVertex2f((float)(draw_x+draw_w), (float)(draw_y+draw_h));
    glTexCoord2f(0,1); glVertex2f((float)draw_x,          (float)(draw_y+draw_h));
    glEnd();
    glDisable(GL_TEXTURE_2D);

    /* Info line above the frame. */
    char info[96];
    if (g.ed_is_static) {
        snprintf(info, sizeof info, "IMAGE %d/%d   %dX%d",
                 g.ed_nfiles ? g.ed_file_idx + 1 : 1,
                 g.ed_nfiles ? g.ed_nfiles : 1,
                 g.ed_r.width, g.ed_r.height);
    } else {
        snprintf(info, sizeof info, "FRAME %d/%d  IN %d  OUT %d  %s",
                 g.ed_frame + 1, g.ed_r.nframes,
                 g.ed_trim_in, g.ed_trim_out,
                 g.ed_playing ? "PLAY" : "PAUSE");
    }
    glColor4f(0.9f, 0.9f, 1.0f, 1.0f);
    draw_string_at(10, (float)(bar_h + 4), info, 2);

    if (bar_h) {
        an_draw_bar(win_w);
        /* Typing happens on the image, so the caret has to be on the image
         * too -- a text box somewhere else would make you look away from the
         * thing you are labelling. */
        if (an.typing) {
            int sx = an.vx + (int)((int64_t)an.tx * an.vw / g.ed_r.width);
            int sy = an.vy + (int)((int64_t)an.ty * an.vh / g.ed_r.height);
            uint32_t c = an_rgb();
            glColor4f(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f,
                      (c & 0xFF) / 255.0f, 1.0f);
            char line[96];
            snprintf(line, sizeof line, "%s_", an.text);
            draw_string_at((float)sx, (float)sy, line, 2);
        }
    }

    /* Thumbnail strip along the bottom. */
    int strip_top = win_h - strip_h - btn_h;
    /* Background bar */
    glColor4f(0.14f, 0.14f, 0.16f, 1.0f);
    glBegin(GL_QUADS);
    glVertex2f(0,             (float)strip_top);
    glVertex2f((float)win_w,  (float)strip_top);
    glVertex2f((float)win_w,  (float)(strip_top + strip_h));
    glVertex2f(0,             (float)(strip_top + strip_h));
    glEnd();

    int strip_n = ed_strip_count();
    if (strip_n <= 0) goto draw_hints;   /* stills: image uses the full pane */
    int total_w = g.ed_thumb_w * strip_n;
    int strip_x_start = (win_w - total_w) / 2;
    if (strip_x_start < 0) strip_x_start = 0;
    glEnable(GL_TEXTURE_2D);
    for (int i = 0; i < strip_n; i++) {
        int tx = strip_x_start + i * g.ed_thumb_w;
        if (tx + g.ed_thumb_w > win_w) break;  /* clip if we overflow */
        if (!g.ed_thumb_tex || !g.ed_thumb_tex[i]) continue;
        glBindTexture(GL_TEXTURE_2D, g.ed_thumb_tex[i]);
        /* Animation: dim outside the trim range. Still: dim everything but
         * the file being shown. */
        bool in_range = g.ed_is_static
            ? (i == g.ed_file_idx)
            : (i >= g.ed_trim_in && i <= g.ed_trim_out);
        float dim = in_range ? 1.0f : 0.35f;
        glColor4f(dim, dim, dim, 1.0f);
        glBegin(GL_QUADS);
        glTexCoord2f(0,0); glVertex2f((float)tx,                (float)(strip_top + 3));
        glTexCoord2f(1,0); glVertex2f((float)(tx+g.ed_thumb_w), (float)(strip_top + 3));
        glTexCoord2f(1,1); glVertex2f((float)(tx+g.ed_thumb_w), (float)(strip_top + 3 + THUMB_H));
        glTexCoord2f(0,1); glVertex2f((float)tx,                (float)(strip_top + 3 + THUMB_H));
        glEnd();
    }
    glDisable(GL_TEXTURE_2D);

    /* Highlight the current cell. */
    int cur_cell = g.ed_is_static ? g.ed_file_idx : g.ed_frame;
    int cx = strip_x_start + cur_cell * g.ed_thumb_w;
    if (cx + g.ed_thumb_w <= win_w) {
        glColor4f(1.0f, 0.55f, 0.10f, 1.0f);
        glLineWidth(2.0f);
        glBegin(GL_LINE_LOOP);
        glVertex2f((float)cx,               (float)(strip_top + 2));
        glVertex2f((float)(cx+g.ed_thumb_w),(float)(strip_top + 2));
        glVertex2f((float)(cx+g.ed_thumb_w),(float)(strip_top + 3 + THUMB_H + 1));
        glVertex2f((float)cx,               (float)(strip_top + 3 + THUMB_H + 1));
        glEnd();
    }

draw_hints:
    /* Button hint row. */
    glColor4f(0.7f, 0.7f, 0.8f, 1.0f);
    const char* hints = g.ed_is_static
        ? "  A:ARROW R:BOX E:OVAL L:LINE P:PEN H:MARK X:PIX T:TEXT N:NUM C:CROP  "
          "[ ]:SIZE 1-8:COLOUR  CTRL+Z/C/S  PGUP/PGDN:FILE  ESC:CLOSE"
        : "  <-/->:STEP  []:TRIM  SPACE:PLAY  S:SAVE  PGUP/PGDN:FILE  ESC:CLOSE";
    draw_string_at(4, (float)(win_h - btn_h + 6), hints, 2);

    ed_draw_arrows(win_w, win_h);

    glEnable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
}

/* Where the on-screen prev/next arrows live, so the painter and the click
 * handler cannot disagree about it. */
#define ED_ARROW_W 46
static void ed_arrow_rects(int w, int h, int* lx, int* rx, int* ay, int* ah) {
    int strip_h = (ed_strip_count() > 0) ? (THUMB_H + 6) : 0;
    int btn_h   = 24;
    int top     = 24;
    int bot     = h - strip_h - btn_h;
    if (bot < top + 40) bot = top + 40;
    *ay = top;
    *ah = bot - top;
    *lx = 0;
    *rx = w - ED_ARROW_W;
}

/* Big translucent chevrons down each side -- the viewer was keyboard-only,
 * which is fine once you know it and useless the first time. */
static void ed_draw_arrows(int w, int h) {
    if (!g.ed_open || !g.ed_browsing) return;
    int lx, rx, ay, ah;
    ed_arrow_rects(w, h, &lx, &rx, &ay, &ah);
    float cy = (float)(ay + ah / 2);

    for (int side = 0; side < 2; side++) {
        float x0 = (float)(side == 0 ? lx : rx);
        /* Hit area, barely visible until it matters. */
        glColor4f(1.0f, 1.0f, 1.0f, 0.05f);
        glBegin(GL_QUADS);
        glVertex2f(x0,                 (float)ay);
        glVertex2f(x0 + ED_ARROW_W,    (float)ay);
        glVertex2f(x0 + ED_ARROW_W,    (float)(ay + ah));
        glVertex2f(x0,                 (float)(ay + ah));
        glEnd();

        float cx = x0 + ED_ARROW_W * 0.5f;
        /* Apex points AWAY from centre: left arrow points left. */
        float d  = (side == 0) ? 1.0f : -1.0f;
        glColor4f(1.0f, 0.62f, 0.18f, 0.85f);
        glLineWidth(4.0f);
        glBegin(GL_LINE_STRIP);
        glVertex2f(cx + d * 6.0f,  cy - 14.0f);
        glVertex2f(cx - d * 6.0f,  cy);
        glVertex2f(cx + d * 6.0f,  cy + 14.0f);
        glEnd();
    }
}

/* Returns -1 for the left arrow, +1 for the right, 0 for neither. */
static int ed_arrow_hit(int w, int h, double mx, double my) {
    if (!g.ed_browsing) return 0;
    int lx, rx, ay, ah;
    ed_arrow_rects(w, h, &lx, &rx, &ay, &ah);
    if (my < ay || my > ay + ah) return 0;
    if (mx >= lx && mx <= lx + ED_ARROW_W) return -1;
    if (mx >= rx && mx <= rx + ED_ARROW_W) return +1;
    return 0;
}

static void ed_step(int dir);   /* defined below */

/* Mouse motion only matters mid-stroke, so this is cheap the rest of the
 * time. Registered on the editor window in ed_open_path. */
static void on_ed_cursor(GLFWwindow* win, double mx, double my) {
    (void)win;
    if (!an.dragging) return;
    int ix, iy;
    an_to_image((int)mx, (int)my, &ix, &iy);
    if (an.tool == TOOL_PEN) {
        /* Freehand is a chain of committed segments rather than one shape:
         * there is no "the stroke so far" to re-preview, and each segment is
         * already where it belongs. The whole chain still undoes in one go,
         * because the undo snapshot was taken when the mouse went down. */
        PaintImg im = an_target();
        paint_line(im, an.x0, an.y0, ix, iy, an_rgb(), an.thick);
        if (ix < an.pen_x0) an.pen_x0 = ix;
        if (ix > an.pen_x1) an.pen_x1 = ix;
        if (iy < an.pen_y0) an.pen_y0 = iy;
        if (iy > an.pen_y1) an.pen_y1 = iy;
        an.x0 = ix; an.y0 = iy;
        an_retexture();
        return;
    }
    an.x1 = ix; an.y1 = iy;
    an_preview_update();
}

static void on_ed_mouse(GLFWwindow* win, int button, int action, int mods) {
    (void)mods;
    if (!g.ed_open) return;
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;
    double mx, my; glfwGetCursorPos(win, &mx, &my);
    int w, h; glfwGetWindowSize(win, &w, &h);

    if (action == GLFW_RELEASE) {
        if (an.dragging) {
            an.dragging = false;
            if (an.tool == TOOL_PEN) {
                /* Trim the held image down to the box the stroke actually
                 * touched, so a scribble in one corner does not cost a copy
                 * of the whole screen. */
                if (an.pen_hold) {
                    int pad = an.thick + 2;
                    int x0 = an.pen_x0 - pad, y0 = an.pen_y0 - pad;
                    int x1 = an.pen_x1 + pad, y1 = an.pen_y1 + pad;
                    if (x0 < 0) x0 = 0;
                    if (y0 < 0) y0 = 0;
                    if (x1 > g.ed_r.width  - 1) x1 = g.ed_r.width  - 1;
                    if (y1 > g.ed_r.height - 1) y1 = g.ed_r.height - 1;
                    if (x1 >= x0 && y1 >= y0) {
                        AnStep st;
                        st.x = x0; st.y = y0;
                        st.w = x1 - x0 + 1; st.h = y1 - y0 + 1;
                        st.img_w = g.ed_r.width; st.img_h = g.ed_r.height;
                        st.px = (uint8_t*)malloc((size_t)st.w * st.h * 4);
                        if (st.px) {
                            for (int r = 0; r < st.h; r++)
                                memcpy(st.px + (size_t)r * st.w * 4,
                                       an.pen_hold + ((size_t)(y0 + r) * (size_t)st.img_w + x0) * 4,
                                       (size_t)st.w * 4);
                            an_push_step(st);
                        }
                    }
                    free(an.pen_hold); an.pen_hold = NULL;
                }
                an_retexture();
                return;
            }
            an_commit();
        }
        return;
    }
    if (action != GLFW_PRESS) return;

    if (an_active()) {
        if (an_bar_click((int)mx, (int)my)) return;
        int ix, iy;
        if (an_to_image((int)mx, (int)my, &ix, &iy)) {
            /* A click on the image belongs to the tool. The side chevrons
             * still work, but only where they are not over the picture --
             * otherwise every arrow drawn near the edge would page the
             * folder instead. */
            if (an.typing) an_commit_text();
            if (an.tool == TOOL_TEXT) {
                an.typing = true; an.text[0] = 0; an.tx = ix; an.ty = iy;
                return;
            }
            an.dragging = true;
            an.x0 = an.x1 = ix; an.y0 = an.y1 = iy;
            an.px0 = ix; an.py0 = iy; an.px1 = ix; an.py1 = iy;
            if (an.tool == TOOL_PEN) {
                size_t n = (size_t)g.ed_r.width * g.ed_r.height * 4;
                free(an.pen_hold);
                an.pen_hold = (uint8_t*)malloc(n);
                if (an.pen_hold) memcpy(an.pen_hold, an_img(), n);
                an.pen_x0 = an.pen_x1 = ix;
                an.pen_y0 = an.pen_y1 = iy;
            }
            else an_preview_update();
            return;
        }
    }
    int hit = ed_arrow_hit(w, h, mx, my);
    if (hit) ed_step(hit);
}

/* Typed characters, for the text tool. Only consumed while typing, so every
 * other key keeps its normal meaning. */
static void on_ed_char(GLFWwindow* win, unsigned int cp) {
    (void)win;
    if (!an.typing || cp < 32 || cp > 126) return;
    size_t n = strlen(an.text);
    if (n + 1 < sizeof an.text) { an.text[n] = (char)cp; an.text[n + 1] = 0; }
}

/* Returns true when annotation has taken the key. Placed before the viewer's
 * own bindings, and only ever active for stills, so nothing an animation
 * needs is shadowed. */
static bool an_key(int key, int action, int mods) {
    if (!an_active()) return false;
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return false;
    bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;

    if (an.typing) {
        if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) { an_commit_text(); return true; }
        if (key == GLFW_KEY_ESCAPE) { an.typing = false; an.text[0] = 0; return true; }
        if (key == GLFW_KEY_BACKSPACE) {
            size_t n = strlen(an.text);
            if (n) an.text[n - 1] = 0;
            return true;
        }
        return true;         /* everything else is text, not a shortcut */
    }

    if (ctrl) {
        switch (key) {
        case GLFW_KEY_Z: if (mods & GLFW_MOD_SHIFT) an_redo(); else an_undo(); return true;
        case GLFW_KEY_Y: an_redo(); return true;
        case GLFW_KEY_C: an_copy(); return true;
        case GLFW_KEY_S: an_save(); return true;
        default: return false;
        }
    }

    switch (key) {
    case GLFW_KEY_A: an.tool = TOOL_ARROW;   return true;
    case GLFW_KEY_R: an.tool = TOOL_BOX;     return true;
    case GLFW_KEY_E: an.tool = TOOL_ELLIPSE; return true;
    case GLFW_KEY_L: an.tool = TOOL_LINE;    return true;
    case GLFW_KEY_P: an.tool = TOOL_PEN;     return true;
    case GLFW_KEY_H: an.tool = TOOL_MARK;    return true;
    case GLFW_KEY_X: an.tool = TOOL_PIX;     return true;
    case GLFW_KEY_T: an.tool = TOOL_TEXT;    return true;
    case GLFW_KEY_N: an.tool = TOOL_NUM;     return true;
    case GLFW_KEY_C: an.tool = TOOL_CROP;    return true;
    case GLFW_KEY_S: an_save(); return true;   /* not the GIF trim-save */
    case GLFW_KEY_LEFT_BRACKET:  if (an.thick > 1)  an.thick--; return true;
    case GLFW_KEY_RIGHT_BRACKET: if (an.thick < 20) an.thick++; return true;
    case GLFW_KEY_ESCAPE:
        if (an.dragging) {          /* abandon the stroke, keep the editor */
            an.dragging = false;
            an_retexture();
            return true;
        }
        return false;
    default: break;
    }
    if (key >= GLFW_KEY_1 && key <= GLFW_KEY_8) {
        an.color_idx = key - GLFW_KEY_1;
        return true;
    }
    return false;
}

static void ed_key(int key, int action) {
    if (!g.ed_open) return;
    /* Arrows repeat while held; everything else fires on press only. */
    bool repeatable = (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT ||
                       key == GLFW_KEY_PAGE_UP || key == GLFW_KEY_PAGE_DOWN);
    if (action == GLFW_REPEAT && !repeatable) return;
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
    switch (key) {
    case GLFW_KEY_LEFT:  ed_step(-1); break;
    case GLFW_KEY_RIGHT: ed_step(+1); break;
    /* Always available, so an animation can still be paged past. */
    case GLFW_KEY_PAGE_UP:
        ed_goto_file(g.ed_file_idx - 1);
        break;
    case GLFW_KEY_PAGE_DOWN:
        ed_goto_file(g.ed_file_idx + 1);
        break;
    case GLFW_KEY_HOME:
        ed_goto_file(0);
        break;
    case GLFW_KEY_END:
        ed_goto_file(g.ed_nfiles - 1);
        break;
    case GLFW_KEY_LEFT_BRACKET:
        g.ed_trim_in = g.ed_frame;
        if (g.ed_trim_in > g.ed_trim_out) g.ed_trim_out = g.ed_trim_in;
        popup("IN", "Trim in = frame %d", g.ed_trim_in);
        break;
    case GLFW_KEY_RIGHT_BRACKET:
        g.ed_trim_out = g.ed_frame;
        if (g.ed_trim_out < g.ed_trim_in) g.ed_trim_in = g.ed_trim_out;
        popup("OUT", "Trim out = frame %d", g.ed_trim_out);
        break;
    case GLFW_KEY_SPACE:
        g.ed_playing = !g.ed_playing;
        g.ed_last_advance_ms = plat_now_ms();
        break;
    case GLFW_KEY_S:
        if (g.ed_is_static) popup("NO TRIM", "Trim/save applies to animated GIFs.");
        else                ed_save_trimmed();
        break;
    case GLFW_KEY_ESCAPE:
        ed_close();
        break;
    default: break;
    }
}

/* Scroll over the orb to resize it. No chrome, no mode, no handles -- and
 * handles would not work anyway now that the window is clipped to a circle,
 * since the corners are outside the window and cannot be clicked. */
static void on_scroll(GLFWwindow* win, double dx, double dy) {
    (void)win; (void)dx;
    if (g.ping_active) return;          /* window is enlarged; wait it out */
    if (dy == 0.0) return;

    /* Growing about the centre needs the real centre, not a remembered one. */
    orb_sync_from_window();
    int old = g.orb_size;
    /* Proportional, but gently. (size/8)+2 moved ~14% per notch, so the
     * ladder ran 39,46,54,63,73,84,96,110,126,144,164,187,212,240 -- big
     * enough leaps that resizing read as jumping between sizes rather
     * than scaling. /24 is ~4%, roughly three times as many stops, and
     * still proportional so it stays smooth at both ends. */
    int step = (g.orb_size / 24) + 1;
    g.orb_size += (dy > 0 ? step : -step);

    /* Keep the size EVEN. The orb is centred with orb_size/2, which truncates
     * on odd sizes, so as the size stepped odd-even-odd the drawn centre
     * alternated between exact and half a pixel high -- a half-pixel shiver
     * on every notch. Measured over the full ladder the error sequence was
     * 0, 0, -0.5, 0, -0.5 ... ; forcing even makes it 0 at every step.
     * (Integer only, per the house rule -- no rounding, just parity.) */
    g.orb_size &= ~1;

    if (g.orb_size < ORB_SIZE_MIN) g.orb_size = ORB_SIZE_MIN;
    if (g.orb_size > ORB_SIZE_MAX) g.orb_size = ORB_SIZE_MAX;
    g.orb_size &= ~1;                    /* the clamps must not undo it */
    if (g.orb_size == old) return;

    /* Grow about the centre so the orb does not crawl across the screen. */
    int cx = g.orb_x + old / 2;
    int cy = g.orb_y + old / 2;
    g.orb_x = cx - g.orb_size / 2;
    g.orb_y = cy - g.orb_size / 2;
    /* Growing near an edge can push it over one. */
    clamp_orb_to_monitors(&g.orb_x, &g.orb_y);

    /* One call, not two. Resizing then moving leaves the window briefly at
     * the new size in the old place, and because the orb is centred in a 3x
     * window that shows up as a diagonal flick of half the size change --
     * measured at 9 to 18 px on Joe's capture, always with dx exactly equal
     * to dy, which is the giveaway. */
    /* Move only. The window never changes size now, so there is no
     * resize for the framebuffer to lag behind. */
    orb_move_to(g.orb_x, g.orb_y);
    orb_apply_region(false);

    g.want_orb_x = g.orb_x; g.want_orb_y = g.orb_y;
    anchor_from_position(g.orb_x, g.orb_y, g.orb_size, g.orb_size);
    settings_mark_dirty();
    popup("SIZE", "Orb %d px (scroll over it to resize).", g.orb_size);
}

/* Last error GLFW reported. Kept rather than printed on the spot, because
 * the useful moment to say it is when the call that provoked it fails. */
static char g_glfw_err[256];

static void on_glfw_error(int code, const char* desc) {
    snprintf(g_glfw_err, sizeof g_glfw_err, "%s (GLFW error %d)",
             desc ? desc : "unknown", code);
    log_write("glfw", "%s", g_glfw_err);
}

static void on_key(GLFWwindow* win, int key, int sc, int action, int mods) {
    (void)win; (void)sc;
    if (key == GLFW_KEY_F1 && action == GLFW_PRESS) { help_open(); return; }
    /* Annotation gets first refusal, and only for stills -- so the animation
     * bindings underneath keep working untouched. */
    if (an_key(key, action, mods)) return;
    if (g.ed_open) ed_key(key, action);
}

/* Editor window moved / resized -- remember it (debounced save). */
static void on_ed_move(GLFWwindow* win, int x, int y) {
    (void)win;
    g.ed_x = x; g.ed_y = y;
    g.have_ed_geom = true;
    settings_mark_dirty();
}
static void on_ed_size(GLFWwindow* win, int cw, int ch) {
    (void)win;
    if (cw > 0 && ch > 0) { g.ed_w = cw; g.ed_h = ch; g.have_ed_geom = true; }
    settings_mark_dirty();
}

/* ── locate-me ping ─────────────────────────────────────────────────────
 * The orb is 68px and easy to lose. Clicking our taskbar button fires an
 * expanding ring. To have room to draw outside the dime we briefly grow
 * the orb window to PING_SIZE (keeping the orb visually centred and the
 * same apparent size), then shrink back. */

static void ping_start_for(int duration_ms) {
    if (g.ping_active) return;
    if (g.state == ORB_RECORDING) return;   /* don't disturb a capture */
    if (g.isDragging) return;

    /* Nothing resizes and nothing moves -- the window is already big enough.
     * Just open the region so the rings have somewhere to paint. The orb is
     * always dead centre, so there is no offset to track and no clamping to
     * get wrong. Deliberately NOT click-through: the orb must stay usable
     * while the rings are running. */
    g.ping_active   = true;
    g.ping_ms       = duration_ms;
    g.ping_start_ms = plat_now_ms();
    orb_apply_region(true);
    log_write("ping", "locate ring, %d ms", duration_ms);
}

/* Any click ends the ping immediately, so it can never be in the way. */
static void ping_cancel(void) {
    if (!g.ping_active) return;
    g.ping_active = false;
    orb_apply_region(false);
}

static void ping_tick(void) {
    if (!g.ping_active) return;
    if (plat_now_ms() - g.ping_start_ms < (uint64_t)g.ping_ms) return;
    g.ping_active = false;
    orb_apply_region(false);          /* back to a round, clickable dime */
}

/* Every couple of seconds, notice whether the display topology changed.
 *
 * The morning failure mode: monitor 3 is slow, Windows evacuates windows off
 * it before it powers up, we launch seeing only two monitors, and the parked
 * position looks invalid. Rather than accept that permanently, keep the
 * user's chosen spot and move back to it the moment its monitor appears. */
static void tick_monitor_rehome(void) {
    uint64_t now = plat_now_ms();
    if (now - g.last_monitor_check_ms < 2000) return;
    g.last_monitor_check_ms = now;
    if (g.state == ORB_RECORDING || g.ping_active || g.isDragging) return;

    PlatMonitor mons[GIF_MAX_MONITORS];
    int n = plat_enum_monitors(mons, GIF_MAX_MONITORS);
    bool changed = (n != g.last_monitor_count);
    g.last_monitor_count = n;

    if (!g.orb_displaced) return;
    if (!g.have_anchor) return;

    int nx, ny; bool exact = false;
    if (!anchor_to_position(ORB_SIZE, ORB_SIZE, &nx, &ny, &exact)) return;
    if (!exact) return;                      /* preferred display still absent */
    if (nx == g.orb_x && ny == g.orb_y) { g.orb_displaced = false; return; }

    g.orb_x = nx; g.orb_y = ny;
    g.want_orb_x = nx; g.want_orb_y = ny;
    g.orb_displaced = false;
    orb_move_to(g.orb_x, g.orb_y);
    log_write("cfg", "display %s returned (%d live%s) -- orb home to (%d,%d)",
              g.anchor_mon, n, changed ? ", topology changed" : "",
              g.orb_x, g.orb_y);
    popup("HOME", "Display came back -- orb returned to its corner.");
    ping_start_for(PING_MS_LONG);
}

/* Expanding rings, drawn in 2D over the orb. */
static void draw_ping_rings(int w, int h) {
    if (!g.ping_active) return;
    double t = (double)(plat_now_ms() - g.ping_start_ms) / (double)g.ping_ms;
    if (t < 0) t = 0;
    if (t > 1) t = 1;

    float r, gc, b;
    state_color(&r, &gc, &b);

    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glDisable(GL_DEPTH_TEST);

    float cx = w * 0.5f, cy = h * 0.5f;
    float rmax = (float)(w < h ? w : h) * 0.48f;
    float rmin = ORB_SIZE * 0.5f;
    if (rmax < rmin + 4.0f) rmax = rmin + 4.0f;

    /* Staggered rings so it reads as a pulse rather than one blip. A long
     * ping repeats the wave several times -- it exists to be FOUND, and one
     * quick flash is easy to miss if you were looking elsewhere. */
    int waves = (g.ping_ms >= PING_MS_LONG) ? 9 : 3;
    for (int k = 0; k < waves; k++) {
        double tk = t - k * 0.11;
        if (tk <= 0) continue;
        tk = tk - (double)((int)tk);      /* repeat the sweep */
        if (tk <= 0 || tk >= 1) continue;
        float rad   = rmin + (float)tk * (rmax - rmin);
        float alpha = (float)(1.0 - tk) * 0.9f;
        glLineWidth(3.0f - (float)k * 0.6f);
        glColor4f(r, gc, b, alpha);
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < 48; i++) {
            double a = 2.0 * M_PI * i / 48.0;
            glVertex2f(cx + rad * (float)cos(a), cy + rad * (float)sin(a));
        }
        glEnd();
    }

    glEnable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
}

static void on_drop(GLFWwindow* win, int count, const char* paths[]) {
    (void)win;
    if (count < 1) return;
    const char* p = paths[0];
    const char* dot = strrchr(p, '.');
    if (!dot) { popup("NO EXT", "No file extension: %s", p); return; }

    char ext[24];
    snprintf(ext, sizeof ext, "%s", dot);
    for (char* c = ext; *c; c++)
        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');

    /* Video: hand it to whatever the user already plays video with. Building
     * a player would mean decode, seek, sync and transport controls -- and
     * VLC (or whatever they chose) is already better at all four. */
    static const char* VIDEO[] = { ".mp4",".mkv",".avi",".mov",".webm",
                                   ".wmv",".m4v",".mpg",".mpeg",".flv", NULL };
    for (int i = 0; VIDEO[i]; i++) {
        if (!strcmp(ext, VIDEO[i])) {
            plat_open_with_default_app(p);
            popup_mode("VIDEO", "OPENED", "Opened %s in the default player.", p);
            return;
        }
    }

    /* Images: accept anything this machine has a decoder for. */
    char probe[32];
    snprintf(probe, sizeof probe, "%s,", ext);
    bool accept = (g_img_exts[0] && strstr(g_img_exts, probe) != NULL);
    if (!accept) {
        /* Our own decoders on top of whatever the OS reported. */
        static const char* MINE[] = { ".tga",".psd",".webp", NULL };
        for (int i = 0; MINE[i] && !accept; i++)
            if (!strcmp(ext, MINE[i])) accept = true;
    }
    if (!accept) { popup("NOT IMG", "No decoder for %s on this machine.", ext); return; }
    ed_open_path(p);
}

static void draw_popup_overlay(int w, int h) {

    int rx, ry, rw, rh, scale;
    if (!toast_rect(w, h, &rx, &ry, &rw, &rh, &scale)) return;
    int cap = (w - 4) / (6 * scale);
    if (cap < 4)  cap = 4;
    if (cap > 20) cap = 20;
    int nc = (int)strlen(g.popupShort);
    if (nc > cap) nc = cap;
    int nl = (int)strlen(g.popupLabel);
    if (nl > cap) nl = cap;
    int line_h = 7 * scale + 2;
    int ty = ry + 2;

    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glDisable(GL_DEPTH_TEST);

    /* translucent black bar */
    glColor4f(0.0f, 0.0f, 0.0f, 0.85f);
    glBegin(GL_QUADS);
    glVertex2f((float)rx,        (float)ry);
    glVertex2f((float)(rx + rw), (float)ry);
    glVertex2f((float)(rx + rw), (float)(ry + rh));
    glVertex2f((float)rx,        (float)(ry + rh));
    glEnd();

    /* white text */
    char show[24] = {0};
    if (nl > 0) {
        /* Mode line, dimmer, sitting above the state word. */
        char lab[24] = {0};
        for (int i = 0; i < nl && i < 23; i++) lab[i] = g.popupLabel[i];
        int lx = rx + (rw - nl * 6 * scale) / 2;
        glColor4f(1.00f, 0.72f, 0.30f, 0.95f);
        draw_string_at((float)lx, (float)ty, lab, scale);
        ty += line_h;
    }
    for (int i = 0; i < nc && i < 23; i++) show[i] = g.popupShort[i];
    int sx = rx + (rw - nc * 6 * scale) / 2;
    glColor4f(1.0f, 1.0f, 1.0f, 0.95f);
    draw_string_at((float)sx, (float)ty, show, scale);

    glEnable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
}

/* ── input dispatch ───────────────────────────────────────────────────── */

/* Snapshot of everything the menu draws. */
static PlatMenuState menu_state(void) {
    PlatMenuState st;
    for (int i = 0; i < PLAT_HK_COUNT; i++) st.hotkey_on[i] = g.hotkey_on[i];
    st.auto_open = g.auto_open_folder;
    st.auto_clip = g.auto_clipboard;
    st.shot_editor = g.shot_editor;
    st.tray_only = g.tray_only;
    st.audio_src = g.audio_src;
    st.dbl_video = g.dbl_video;
    /* Asked of the OS every time the menu opens, not remembered -- the
     * user can remove the entry from Settings and the tick must follow. */
    st.run_at_startup = plat_get_run_at_startup();
    return st;
}

static void refresh_monitors(void) {
    g.nmonitors = plat_enum_monitors(g.monitors, GIF_MAX_MONITORS);
}

static void handle_f7(void);            /* defined below */
static void start_video_window(void* fg);
static void handle_f5(void);
static void start_video_monitor(PlatMonitor m);
static void handle_f8(void);            /* defined below */
static void start_camera_record(int index);
static void handle_shot_region(void);
static void shoot_window_handle(void* fg);
static void shot_delayed_start(int seconds);

/* Arm, and say what the next click will do.
 *
 * Every "... window" item in the menu comes through here rather than acting
 * on the focused window, because opening the menu makes the ORB the
 * foreground window -- so those items used to fail with "not self", every
 * time. Arming is also simply the better interaction: point at what you want
 * instead of having to focus it first and then find the menu. */
static void arm_for(int mode) {
    if (g.state == ORB_RECORDING) { stop_recording(); return; }
    g.state       = ORB_ARMED;
    g.armed_mode  = mode;
    g.armed_video = (mode == ARM_MP4);
    popup_mode(mode == ARM_SHOT ? "PNG" : (mode == ARM_MP4 ? "MP4" : "GIF"),
               "ARMED", "Click the window you want to %s.",
               mode == ARM_SHOT ? "capture" : "record");
}

static void handle_menu_command(int cmd) {
    if (cmd == 0) return;
    if (cmd >= PLAT_MENU_HOTKEY_BASE &&
        cmd < PLAT_MENU_HOTKEY_BASE + PLAT_HK_COUNT) {
        int i = cmd - PLAT_MENU_HOTKEY_BASE;
        bool want = !g.hotkey_on[i];
        bool got  = apply_hotkey(i, want);
        if (i == 0) g.state = got ? ORB_IDLE : ORB_NOCAPTURE;
        popup_mode(HK_NAMES[i], got ? "CAPTURED" : "RELEASED",
                   "%s %s", HK_NAMES[i],
                   got ? "captured by the orb."
                       : (want ? "refused -- another app owns it."
                               : "released to other applications."));
        settings_mark_dirty();
    } else if (cmd == PLAT_MENU_HIDE_ORB) {
        /* Hiding without a tray icon would strand the user: no orb, no
         * taskbar button, no way back short of Task Manager. So switch
         * tray-only on as part of the same action rather than letting the
         * two settings disagree. */
        if (!g.tray_only) {
            g.tray_only = true;
            plat_tray_set(g.window, true, "ORB_Recorder");
        }
        g_orb_hidden = true;
        plat_window_set_visible(g.window, false);
        settings_mark_dirty();
        log_write("orb", "hidden -- tray icon is the way back");
    } else if (cmd == PLAT_MENU_RUN_AT_STARTUP) {
        bool now = !plat_get_run_at_startup();
        if (plat_set_run_at_startup(now)) {
            log_write("cfg", "run at startup -> %s", now ? "on" : "off");
            popup_mode("START", now ? "ON" : "OFF",
                       now ? "ORB_Recorder will start when you log in."
                           : "ORB_Recorder will no longer start at login.");
        } else {
            popup_mode("START", "FAILED", "Could not change the startup entry.");
        }
    } else if (cmd == PLAT_MENU_SHOT_DELAY_5) {
        shot_delayed_start(5);
    } else if (cmd == PLAT_MENU_SHOT_REGION) {
        handle_shot_region();
    } else if (cmd == PLAT_MENU_SHOT_WINDOW) {
        arm_for(ARM_SHOT);
    } else if (cmd == PLAT_MENU_RECORD_GIF) {
        arm_for(ARM_GIF);
    } else if (cmd == PLAT_MENU_DBL_GIF || cmd == PLAT_MENU_DBL_MP4) {
        g.dbl_video = (cmd == PLAT_MENU_DBL_MP4);
        popup_mode(g.dbl_video ? "MP4" : "GIF", "DBL-CLICK",
                   "Double-clicking the orb now arms %s.",
                   g.dbl_video ? "video" : "GIF");
        settings_mark_dirty();
    } else if (cmd == PLAT_MENU_TOGGLE_AUTOOPEN) {
        g.auto_open_folder = !g.auto_open_folder;
        popup(g.auto_open_folder ? "OPEN ON" : "OPEN OFF",
              "Auto-open folder: %s", g.auto_open_folder ? "ON" : "OFF");
        settings_mark_dirty();
    } else if (cmd >= PLAT_MENU_SHOT_ED_BASE &&
               cmd <= PLAT_MENU_SHOT_ED_BASE + 2) {
        g.shot_editor = cmd - PLAT_MENU_SHOT_ED_BASE;
        static const char* WHAT[3] = { "nothing -- they are just saved",
                                       "the built-in editor",
                                       "the system image editor" };
        popup_mode("PNG", "AFTER", "Screenshots now open %s.", WHAT[g.shot_editor]);
        settings_mark_dirty();
    } else if (cmd == PLAT_MENU_OPEN_EDITOR) {
        char pick[512];
        if (plat_open_file_dialog(pick, sizeof pick) && ed_open_path(pick))
            g.ed_browsing = true;
    } else if (cmd == PLAT_MENU_HELP) {
        help_open();
    } else if (cmd == PLAT_MENU_EDIT_SETTINGS) {
        /* Six values and three of them are already menu checkboxes -- a
         * dialog framework would cost more than it returns. Hand the .ini
         * to whatever edits .ini files; changes load on next start. */
        settings_save();
        plat_open_with_default_app(g_cfg_path);
        popup("SETTINGS", "Opened %s (re-read on next start).", g_cfg_path);
    } else if (cmd == PLAT_MENU_TOGGLE_TRAY) {
        g.tray_only = !g.tray_only;
        plat_tray_set(g.window, g.tray_only, "ORB_Recorder");
        popup(g.tray_only ? "TRAY ON" : "TRAY OFF",
              "Tray-only: %s", g.tray_only ? "ON (taskbar button hidden)" : "OFF");
        settings_mark_dirty();
        orb_apply_region(false);        /* the hide/show cycle drops the region */
    } else if (cmd == PLAT_MENU_RECORD_VIDEO) {
        arm_for(ARM_MP4);
    } else if (cmd == PLAT_MENU_RECORD_VIDEO_RGN) {
        handle_f8();
    } else if (cmd == PLAT_MENU_RECORD_REGION) {
        start_recording_region();
    } else if (cmd == PLAT_MENU_TOGGLE_CLIP) {
        g.auto_clipboard = !g.auto_clipboard;
        popup(g.auto_clipboard ? "CLIP ON" : "CLIP OFF",
              "Copy GIF to clipboard: %s", g.auto_clipboard ? "ON" : "OFF");
        settings_mark_dirty();
    } else if (cmd == PLAT_MENU_RESTART_ADMIN) {
        settings_save();                 /* keep geometry across the relaunch */
        if (plat_restart_elevated()) {
            log_write("admin", "elevated instance launched; exiting this one");
            glfwSetWindowShouldClose(g.window, GLFW_TRUE);
        } else {
            popup("UAC NO", "Elevation declined -- still running as normal user.");
        }
    } else if (cmd == PLAT_MENU_QUIT) {
        popup("BYE", "Quitting.");
        glfwSetWindowShouldClose(g.window, GLFW_TRUE);
    } else if (cmd >= PLAT_MENU_CAMERA_BASE &&
               cmd < PLAT_MENU_CAMERA_BASE + PLAT_MAX_CAMERAS) {
        start_camera_record(cmd - PLAT_MENU_CAMERA_BASE);
    } else if (cmd >= PLAT_MENU_AUDIO_BASE && cmd < PLAT_MENU_AUDIO_BASE + 4) {
        static const char* NAMES[4] = { "OFF", "SYSTEM", "MIC", "BOTH" };
        g.audio_src = cmd - PLAT_MENU_AUDIO_BASE;
        popup_mode("SOUND", NAMES[g.audio_src],
                   "Video sound source: %s", NAMES[g.audio_src]);
        settings_mark_dirty();
    } else if (cmd >= PLAT_MENU_VMONITOR_BASE &&
               cmd < PLAT_MENU_VMONITOR_BASE + GIF_MAX_MONITORS) {
        int idx = cmd - PLAT_MENU_VMONITOR_BASE;
        if (idx >= 0 && idx < g.nmonitors) start_video_monitor(g.monitors[idx]);
    } else if (cmd >= PLAT_MENU_MONITOR_BASE &&
               cmd < PLAT_MENU_MONITOR_BASE + GIF_MAX_MONITORS) {
        int idx = cmd - PLAT_MENU_MONITOR_BASE;
        if (idx >= 0 && idx < g.nmonitors) {
            start_recording_monitor(g.monitors[idx]);
        }
    }
}

static void on_mouse_button(GLFWwindow* win, int button, int action, int mods) {
    (void)win; (void)mods;
    /* The rings must never be in the way -- touching the orb ends them. */
    if (action == GLFW_PRESS) ping_cancel();
    uint64_t now = plat_now_ms();
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_PRESS) {
            if (now - g.lastClickMs < 400) {
                if (g.state == ORB_IDLE || g.state == ORB_NOCAPTURE) {
                    g.state = ORB_ARMED;
                    g.armed_video = g.dbl_video;
                    g.armed_mode  = g.dbl_video ? ARM_MP4 : ARM_GIF;
                    popup_mode(g.dbl_video ? "MP4" : "GIF", "ARMED",
                          "Click the window you want to record. "
                          "Escape or double-click orb to stop.");
                } else if (g.state == ORB_ARMED) {
                    g.state = g.hotkey_on[0] ? ORB_IDLE : ORB_NOCAPTURE;
                    popup_mode(g.armed_video ? "MP4" : "GIF", "DISARMED",
                               "Disarmed.");
                    g.armed_video = false;
                } else if (g.state == ORB_RECORDING) {
                    stop_recording();
                }
                g.lastClickMs = 0;
            } else {
                g.lastClickMs = now;
                /* Grab offset must be measured from where the orb actually
                 * is, or the first drag tick teleports it to g.orb_x and
                 * leaves the cursor behind. */
                orb_sync_from_window();
                int cx, cy; plat_get_cursor(&cx, &cy);
                g.dragOffsetX = cx - g.orb_x;
                g.dragOffsetY = cy - g.orb_y;
                g.isDragging = true;
            }
        }
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) {
        refresh_monitors();
        PlatMenuState st = menu_state();
        int cmd = plat_show_menu(g.window, &st, g.monitors, g.nmonitors);
        handle_menu_command(cmd);
    }
}

/* One frame of the orb. Factored out of the main loop because the modal tick
 * needs it too: while a popup menu is up the main loop is not running, and an
 * orb frozen mid-pulse in the middle of a recording looks like a hang. */
static void draw_orb_frame(void) {
    if (g_no3d) {
        /* Colour still carries the state -- that is the orb's whole job,
         * and it is the part that must survive losing the driver. */
        float rf, gf, bf;
        state_color(&rf, &gf, &bf);
        plat_draw_orb_2d(g.window, ORB_WIN_SIZE, ORB_SIZE,
                         (unsigned char)(rf * 255.0f),
                         (unsigned char)(gf * 255.0f),
                         (unsigned char)(bf * 255.0f));
        return;
    }
    glfwMakeContextCurrent(g.window);
    int w, h; glfwGetFramebufferSize(g.window, &w, &h);
    draw_orb(w, h);
    glViewport(0, 0, w, h);              /* rings + toast use the whole window */
    draw_ping_rings(w, h);
    draw_popup_overlay(w, h);
    glfwSwapBuffers(g.window);
}

/* Everything that must not stop just because a menu is open: the frames being
 * recorded, and the orb that is being recorded with them. Deliberately not the
 * whole main loop -- input, dragging and window moves belong to the menu while
 * the menu has the mouse. */
static void modal_tick(void) {
    tick_recording();
}

static void tick_armed_picker(void) {
    if (g.state != ORB_ARMED) return;
    if (!plat_left_button_down()) return;
    void* h = plat_window_at_cursor();
    if (!h) return;
    if (plat_handles_equal(h, g.native)) return;
    /* The armed state remembers what asked for it. */
    int mode = g.armed_mode;
    g.armed_video = false;
    g.armed_mode  = ARM_GIF;
    if (mode == ARM_MP4) {
        start_video_window(h);
    } else if (mode == ARM_SHOT) {
        g.state = ORB_IDLE;          /* a screenshot is not a recording */
        shoot_window_handle(h);
    } else {
        start_recording_window(h);
    }
    plat_sleep_ms(150);
}

static void tick_drag(void) {
    if (!g.isDragging) return;
    if (!plat_left_button_down()) {
        g.isDragging = false;
        settings_mark_dirty();      /* remember where the orb was parked */
        return;
    }
    int cx, cy; plat_get_cursor(&cx, &cy);
    int nx = cx - g.dragOffsetX;
    int ny = cy - g.dragOffsetY;
    clamp_orb_to_monitors(&nx, &ny);
    orb_move_to(nx, ny);
    g.orb_x = nx; g.orb_y = ny;
    /* A deliberate drag -- this IS the preference now. Record it as a
     * corner on a named display, not as a raw coordinate. */
    g.want_orb_x = nx; g.want_orb_y = ny;
    g.have_orb_pos = true;
    g.orb_displaced = false;
    anchor_from_position(nx, ny, ORB_SIZE, ORB_SIZE);
}

static void handle_f5(void) {
    if (g.state == ORB_RECORDING) { stop_recording(); return; }
    if (g.state == ORB_NOCAPTURE) return;
    void* fg = plat_get_foreground_window();
    if (!fg) { popup("NO FG", "No foreground window."); return; }
    if (plat_handles_equal(fg, g.native)) {
        popup("NOT SELF",
              "Focused window is the orb. Focus another window and hit F5.");
        return;
    }
    start_recording_window(fg);
}

/* F7: same target as F5 (the focused window), recorded as MP4 with sound. */
static void start_video_window(void* fg) {
    g.targetWindow = fg;
    g.recordingMonitor = false;

    char title[128]; int sw = 0, sh = 0;
    uint8_t* probe = plat_capture_window(fg, 8, 8, title, sizeof title, &sw, &sh);
    if (probe) free(probe);

    /* Video has real bitrate behind it, so it does not need the GIF's tiny
     * ceiling -- capture at up to 1080p instead of 720p. */
    int mw = 1920, mh = 1080;
    if (sw > 0 && sh > 0) {
        double s = (double)mw / sw;
        double sy = (double)mh / sh;
        if (sy < s) s = sy;
        if (s > 1.0) s = 1.0;
        g.captureW = ((int)(sw * s)) & ~1;
        g.captureH = ((int)(sh * s)) & ~1;
    } else {
        g.captureW = 1280; g.captureH = 720;
    }
    if (g.captureW < 16 || g.captureH < 16) { g.captureW = 1280; g.captureH = 720; }

    char lbl[64];
    sanitize_label(title[0] ? title : "capture", lbl, sizeof lbl);
    if (strlen(lbl) > 48) lbl[48] = 0;
    snprintf(g.record_label, sizeof g.record_label, "%s", lbl[0] ? lbl : "capture");

    if (!start_video_common()) return;
    popup_mode("MP4", "REC",
          "REC video %dx%d @ %dfps with sound: %s  (Esc or F7 to stop)",
          g.captureW, g.captureH, VIDEO_FPS, title[0] ? title : "(no title)");
}

/* F7: record whatever has focus, exactly as F5 does for GIFs. */
static void handle_f7(void) {
    if (g.state == ORB_RECORDING) { stop_recording(); return; }
    void* fg = plat_get_foreground_window();
    if (!fg) { popup_mode("MP4", "NO FG", "No foreground window."); return; }
    if (plat_handles_equal(fg, g.native)) {
        popup_mode("MP4", "NOT SELF",
                   "Focused window is the orb. Focus another and hit F7.");
        return;
    }
    start_video_window(fg);
}

/* Record an entire monitor as video -- the counterpart to the GIF
 * "Record monitor" item, which existed while video only had window/region. */
static void start_video_monitor(PlatMonitor m) {
    if (g.state == ORB_RECORDING) { stop_recording(); return; }
    g.targetWindow = NULL;
    g.recordingMonitor = true;
    g.targetMonitor = m;
    sanitize_label(m.name, g.record_label, sizeof g.record_label);

    int mw = 1920, mh = 1080;
    double sc = (double)mw / m.w;
    double sy = (double)mh / m.h;
    if (sy < sc) sc = sy;
    if (sc > 1.0) sc = 1.0;
    g.captureW = ((int)(m.w * sc)) & ~1;
    g.captureH = ((int)(m.h * sc)) & ~1;
    if (g.captureW < 16) g.captureW = 16;
    if (g.captureH < 16) g.captureH = 16;

    if (!start_video_common()) return;
    popup_mode("MP4", "REC MON",
               "REC %s as video %dx%d @ %dfps  (Esc to stop)",
               m.name, g.captureW, g.captureH, VIDEO_FPS);
}

/* -- screenshot ---------------------------------------------------------
 * Joe: "every screen shot I want to share, so I got to open a folder
 * myself find the location (which can take for ever) find the file and
 * then I can drag and drop it."
 *
 * So the point of this is not the capture -- every tool captures. It is
 * that the file is on the clipboard and the folder is in front of you
 * before you have finished letting go of the mouse. Three things happen at
 * once: the PNG is written, the IMAGE goes on the clipboard so it can be
 * pasted straight into a chat box, and the FILE goes on the clipboard too
 * so it can be pasted as an attachment. Then the folder is raised.       */

static void save_screenshot(const uint8_t* rgba, int w, int h, const char* label) {
    if (!rgba || w <= 0 || h <= 0) { popup_mode("PNG", "EMPTY", "Nothing captured."); return; }

    char folder[512]; plat_get_output_dir(folder, sizeof folder);
    time_t tt = time(NULL);
    struct tm lt = *localtime(&tt);
    char path[700];
    snprintf(path, sizeof path, "%s/%s_%04d%02d%02d_%02d%02d%02d.png",
             folder, (label && label[0]) ? label : "shot",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             lt.tm_hour, lt.tm_min, lt.tm_sec);

    if (!png_write_rgba(path, rgba, w, h)) {
        popup_mode("PNG", "SAVE ERR", "Could not write %s", path);
        return;
    }

    /* Image first, then file: the second SetClipboardData would clear the
     * first if they went the other way round, and the file form is the one
     * that also survives being pasted into Explorer. */
    plat_clipboard_copy_image(rgba, w, h);
    if (g.auto_clipboard) plat_clipboard_copy_file(path);

    const char* why = plat_capture_unavailable();
    log_write("shot", "%dx%d -> %s%s", w, h, path, why ? " (LIKELY BLANK)" : "");
    if (why) popup_mode("PNG", "PROBABLY BLANK", "%s", why);
    else     popup_mode("PNG", "SAVED", "Saved %dx%d to %s", w, h, path);

    /* Raise the folder. plat_open_folder_select reuses a window already
     * showing it rather than stacking up a new one each time. */
    if (g.auto_open_folder) plat_open_folder_select(path);

    /* Then the editor, last, so it lands on top of the folder rather than
     * under it -- the shot is what you came for; the folder is where it
     * went. Every screenshot route ends up here, so region, window and the
     * delayed whole-screen capture all behave the same way. */
    if (g.shot_editor == 1) {
        /* Truncating here would open the editor on a different file --
         * or on nothing -- so refuse rather than guess. */
        if (snprintf(g.pending_edit, sizeof g.pending_edit, "%s", path)
                >= (int)sizeof g.pending_edit) {
            g.pending_edit[0] = 0;
            log_write("shot", "path too long to hand to the editor: %s", path);
        }
    } else if (g.shot_editor == 2) {
        if (!plat_open_in_editor(path))
            log_write("shot", "no image editor could be launched for %s", path);
    }
}

/* F4: drag a rectangle, get a PNG. */
/* Delayed capture runs on its OWN THREAD, and it has to.
 *
 * The obvious implementation -- check a deadline in the main loop -- does not
 * work, and the reason is the whole point of the feature. TrackPopupMenu is a
 * MODAL message loop: while a menu is open the main loop does not run at all.
 * So the timer could only ever fire after the menu closed, which is precisely
 * when there is no longer a menu to photograph. First attempt did exactly
 * that.
 *
 * A background thread keeps counting regardless of what is modal on the UI
 * thread. Screen capture through GDI is safe from another thread; it reads
 * the desktop, it does not touch our window. */
typedef struct { int seconds; } ShotDelayJob;

static void shot_delayed_job(void* arg) {
    ShotDelayJob* j = (ShotDelayJob*)arg;
    int secs = j->seconds;
    free(j);

    /* Count down out loud, but only while nothing modal is up -- popup_mode
     * touches shared state, and a menu being open is the normal case here. */
    for (int t = secs; t > 0; t--) {
        plat_sleep_ms(1000);
        if (g_shot_cancel) { g_shot_cancel = false; g_shot_pending = false; return; }
    }
    if (g_shot_cancel) { g_shot_cancel = false; g_shot_pending = false; return; }

    /* An open menu if there is one, otherwise the monitor under the cursor.
     *
     * Capturing a whole 3440x1440 screen to show a menu 300 px wide is not
     * what the feature is for. Windows gives popup menus their own window
     * class, so when one is open it can be captured exactly -- which makes
     * "captures menus" literally true rather than approximately true. */
    int mx, my, mw, mh;
    if (plat_find_open_menu(&mx, &my, &mw, &mh)) {
        uint8_t* mpx = plat_capture_rect(mx, my, mw, mh, mw, mh);
        if (mpx) {
            char mlabel[64];
            snprintf(mlabel, sizeof mlabel, "Menu_%dx%d", mw, mh);
            save_screenshot(mpx, mw, mh, mlabel);
            free(mpx);
            log_write("shot", "delayed capture: menu %dx%d at (%d,%d)", mw, mh, mx, my);
            g_shot_pending = false;
            return;
        }
    }

    /* The monitor under the cursor, not the whole desktop.
     *
     * With three screens the virtual desktop is 7280x1440 and almost all of
     * it is empty -- a screenshot nobody wants and a file nobody can post.
     * A menu opens where the pointer is, so that is the screen to capture. */
    PlatMonitor mons[GIF_MAX_MONITORS];
    int n = plat_enum_monitors(mons, GIF_MAX_MONITORS);
    int cx = 0, cy = 0;
    plat_get_cursor(&cx, &cy);
    int x0 = 0, y0 = 0, w = 0, h = 0;
    for (int i = 0; i < n; i++) {
        if (cx >= mons[i].x && cx < mons[i].x + mons[i].w &&
            cy >= mons[i].y && cy < mons[i].y + mons[i].h) {
            x0 = mons[i].x; y0 = mons[i].y; w = mons[i].w; h = mons[i].h;
            break;
        }
    }
    if (w == 0 && n > 0) { x0 = mons[0].x; y0 = mons[0].y; w = mons[0].w; h = mons[0].h; }
    if (w >= 16 && h >= 16) {
        uint8_t* px = plat_capture_rect(x0, y0, w, h, w, h);
        if (px) {
            char label[64];
            snprintf(label, sizeof label, "Screen_%dx%d", w, h);
            save_screenshot(px, w, h, label);
            free(px);
        } else {
            log_write("shot", "delayed capture failed");
        }
    }
    g_shot_pending = false;
}

static void shot_delayed_start(int seconds) {
    if (g.state == ORB_RECORDING) {
        popup_mode("PNG", "BUSY", "Stop recording first.");
        return;
    }
    if (g_shot_pending) {
        popup_mode("PNG", "WAITING", "A delayed shot is already counting down.");
        return;
    }
    ShotDelayJob* j = (ShotDelayJob*)malloc(sizeof *j);
    if (!j) return;
    j->seconds = seconds;
    g_shot_pending = true;
    g_shot_cancel  = false;
    popup_mode("PNG", "TIMER",
               "Whole screen in %d seconds -- open whatever you want to "
               "capture and leave it open.", seconds);
    log_write("shot", "delayed whole-screen capture in %ds", seconds);
    plat_run_background(shot_delayed_job, j);
}

static void handle_shot_region(void) {
    if (g.state == ORB_RECORDING) { popup_mode("PNG", "BUSY", "Stop recording first."); return; }
    int rx, ry, rw, rh;
    if (!region_select(&rx, &ry, &rw, &rh)) {
        popup_mode("PNG", "NO BOX", "Screenshot cancelled.");
        return;
    }
    uint8_t* px = plat_capture_rect(rx, ry, rw, rh, rw, rh);   /* native size */
    if (!px) { popup_mode("PNG", "FAILED", "Capture failed."); return; }
    char label[64];
    snprintf(label, sizeof label, "Shot_%dx%d", rw, rh);
    save_screenshot(px, rw, rh, label);
    free(px);
}

/* Menu: PNG of whatever has focus. */
/* Screenshot a window we were handed, rather than whatever has focus.
 * The menu needs this: showing a popup menu makes the ORB the foreground
 * window, so a menu item that shoots "the focused window" shoots the orb. */
static void shoot_window_handle(void* fg) {
    if (g.state == ORB_RECORDING) { popup_mode("PNG", "BUSY", "Stop recording first."); return; }
    if (!fg) { popup_mode("PNG", "NO WINDOW", "No window picked."); return; }
    if (plat_handles_equal(fg, g.native)) {
        popup_mode("PNG", "NOT SELF", "Pick a window other than the orb.");
        return;
    }
    char title[128]; int sw = 0, sh = 0;
    uint8_t* probe = plat_capture_window(fg, 8, 8, title, sizeof title, &sw, &sh);
    if (probe) free(probe);
    if (sw <= 0 || sh <= 0) { popup_mode("PNG", "FAILED", "Capture failed."); return; }

    uint8_t* px = plat_capture_window(fg, sw, sh, NULL, 0, NULL, NULL);
    if (!px) { popup_mode("PNG", "FAILED", "Capture failed."); return; }
    char label[64];
    sanitize_label(title[0] ? title : "window", label, sizeof label);
    if (strlen(label) > 48) label[48] = 0;
    save_screenshot(px, sw, sh, label);
    free(px);
}

/* F8: pick a rectangle (same sheet F6 uses), record it as MP4 with sound. */
static void handle_f8(void) {
    if (g.state == ORB_RECORDING) { stop_recording(); return; }
    int rx, ry, rw, rh;
    if (!region_select(&rx, &ry, &rw, &rh)) {
        popup_mode("MP4", "NO BOX", "Region select cancelled.");
        return;
    }
    PlatMonitor m;
    m.x = rx; m.y = ry; m.w = rw; m.h = rh;
    snprintf(m.name, sizeof m.name, "Region %dx%d", rw, rh);
    snprintf(g.record_label, sizeof g.record_label, "Region_%dx%d", rw, rh);

    g.targetWindow = NULL;
    g.recordingMonitor = true;
    g.targetMonitor = m;

    /* Video carries real bitrate, so keep the region at native size up to
     * 1080p rather than squeezing it the way a GIF has to be squeezed. */
    int mw = 1920, mh = 1080;
    double sc = (double)mw / rw;
    double sy = (double)mh / rh;
    if (sy < sc) sc = sy;
    if (sc > 1.0) sc = 1.0;
    g.captureW = ((int)(rw * sc)) & ~1;
    g.captureH = ((int)(rh * sc)) & ~1;
    if (g.captureW < 16) g.captureW = 16;
    if (g.captureH < 16) g.captureH = 16;

    if (!start_video_common()) return;
    popup_mode("MP4", "REC BOX",
               "REC region %dx%d @ %dfps with sound at (%d,%d)  (Esc or F8 to stop)",
               g.captureW, g.captureH, VIDEO_FPS, rx, ry);
}

/* Open a camera AND confirm it is actually giving us pictures.
 *
 * Opening proves nothing: a camera another application is streaming opens
 * perfectly and then delivers nothing. Measured on this machine -- cameras 0
 * and 2 opened at 640x480 and returned zero frames while camera 1 returned
 * three. Recording one of those produced megabytes of sample data with no
 * moov box: an unplayable file that looked like a successful recording.
 *
 * So a frame has to arrive before we commit. Used by BOTH the F9 automatic
 * pick and an explicit choice from the menu -- picking a camera someone else
 * is using should fail with a clear message, not hand back a broken file.
 *
 * It is also the polite behaviour. Windows will happily let two processes
 * read one camera, which means a recorder can sit on the stream of a call
 * already in progress. Refusing a camera that is already delivering to
 * someone else is not a security control -- any program can still do it --
 * but this program should not do it by accident. */
static bool camera_open_live(int index, int* cw, int* ch) {
    if (!plat_camera_open(index, cw, ch)) return false;
    for (int t = 0; t < 12; t++) {
        uint8_t* probe = plat_camera_read(*cw, *ch);
        if (probe) { free(probe); return true; }
        plat_sleep_ms(40);
    }
    plat_camera_close();
    return false;
}

/* F9: record a camera straight to its own MP4. No compositing -- this is
 * the camera as a source, not a webcam overlay on a screen capture. */
static void start_camera_record(int index) {
    if (g.state == ORB_RECORDING) { stop_recording(); return; }

    char cams[PLAT_MAX_CAMERAS][64];
    int ncam = plat_camera_list(cams, PLAT_MAX_CAMERAS);
    if (ncam <= 0) {
        popup_mode("CAM", "NO CAM", "No camera found.");
        return;
    }
    /* F9 passes -1 meaning "any free one". It used to mean camera 0, which
     * on a machine with three cameras and two of them in use reported BUSY
     * every time while a perfectly free camera sat next to them. Picking a
     * specific one from the menu still honours that choice exactly. */
    /* F9 passes -1 meaning "whichever one works". Trying to pre-screen with
     * an availability probe was a dead end: Media Foundation shares cameras,
     * so nothing ever reports busy. Opening it IS the test, so just try each
     * in turn -- the first that opens is the one to use. */
    int cw = 0, ch = 0;
    if (index < 0) {
        index = -1;
        for (int i = 0; i < ncam; i++) {
            if (!camera_open_live(i, &cw, &ch)) continue;
            index = i;
            break;
        }
        if (index < 0) {
            char why[300];
            plat_camera_last_error(why, sizeof why);
            popup_mode("CAM", "CAM ERR", "No camera would open: %s",
                       why[0] ? why : "unknown reason.");
            log_write("camera", "none of %d cameras opened: %s", ncam,
                      why[0] ? why : "(no reason)");
            return;
        }
    } else if (!camera_open_live(index, &cw, &ch)) {
        /* Ask whether it is actually contended rather than assuming it. A
         * camera can fail to open for reasons that have nothing to do with
         * another application, and blaming one sends the user looking for
         * something that is not there. */
        /* Say what actually went wrong. "Busy" covered four different
         * faults, and only one of them was another application. */
        char why[300];
        plat_camera_last_error(why, sizeof why);
        if (!why[0]) {
            /* It opened and then gave us nothing, which in practice means
             * another application is streaming it. */
            popup_mode("CAM", "IN USE",
                       "%s is delivering to another application -- "
                       "pick a different camera.", cams[index]);
            log_write("camera", "%s opened but delivered no frames -- in use",
                      cams[index]);
        } else {
            popup_mode("CAM", "CAM ERR", "%s: %s", cams[index], why);
            log_write("camera", "%s failed: %s", cams[index], why);
        }
        return;
    }
    g.captureW = cw & ~1;
    g.captureH = ch & ~1;
    if (g.captureW < 16 || g.captureH < 16) { g.captureW = 640; g.captureH = 480; }

    g.targetWindow = NULL;
    g.recordingMonitor = false;
    g.recordingCamera = true;

    /* A camera wants the MICROPHONE, not the desktop.
     *
     * The sound source setting exists for screen recording, where "system"
     * is the obvious default. Pointing a camera at yourself and capturing
     * desktop audio is almost never the intent -- and worse, WASAPI loopback
     * emits no packets at all when nothing is playing (not silent packets:
     * none), so a camera recording on a quiet machine produced a video with
     * an audio track of zero duration. That is what "no sound" looked like.
     *
     * An explicit choice of MIC or BOTH is honoured; only the default is
     * overridden, and the popup says which was used. */
    int saved_audio = g.audio_src;
    if (g.audio_src == PLAT_AUDIO_SYSTEM) g.audio_src = PLAT_AUDIO_MIC;
    sanitize_label(cams[index], g.record_label, sizeof g.record_label);

    bool started = start_video_common();
    g.audio_src = saved_audio;          /* the override is per-recording only */
    if (!started) { plat_camera_close(); g.recordingCamera = false; return; }
    popup_mode("CAM", "REC",
               "REC camera %s at %dx%d with %s  (Esc or F9 to stop)",
               cams[index], g.captureW, g.captureH,
               saved_audio == PLAT_AUDIO_SYSTEM ? "microphone"
                 : (saved_audio == PLAT_AUDIO_MIC  ? "microphone"
                 : (saved_audio == PLAT_AUDIO_BOTH ? "mic + system" : "no sound")));
}

static void handle_escape(void) {
    if (g_shot_pending) {
        g_shot_cancel = true;
        popup_mode("PNG", "CANCEL", "Delayed screenshot cancelled.");
        return;
    }
    if (g.state == ORB_RECORDING) stop_recording();
}

/* ── GL setup ─────────────────────────────────────────────────────────── */

static void init_gl(void) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
}


/* ── main ─────────────────────────────────────────────────────────────── */

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    memset(&g, 0, sizeof g);

    /* Refuse to start twice. A second copy cannot take the global hotkeys --
     * the first already owns them -- so it would come up with every capture
     * key dead and a menu of unchecked boxes. Joe hit exactly that: the log
     * read "F4/F6/F7/F8/F9 refused -- another app owns it" and it looked like
     * a broken install rather than a duplicate. */
    if (!plat_single_instance()) {
        fprintf(stderr, "ORB_Recorder is already running.\n");
        return 0;
    }

    g.state = ORB_IDLE;
    g.hotkey_on[0] = true;
    g.auto_open_folder = true;
    g.auto_clipboard = true;
    g.shot_editor    = 1;
    g.audio_src      = PLAT_AUDIO_SYSTEM;   /* settings_load may override */
    for (int i = 0; i < PLAT_HK_COUNT; i++) g.hotkey_on[i] = true;
    g.gw_open = false;
    g.captureW = TARGET_W; g.captureH = TARGET_H;
    g.orb_size = ORB_SIZE_DEFAULT;      /* settings_load may override */

    log_open();
    log_write("boot", "ORB_Recorder starting; log=%s", g_log_path);
    settings_load();   /* geometry + toggles; may override the defaults above */
    {
        int nfmt = plat_list_image_extensions(g_img_exts, sizeof g_img_exts);
        log_write("boot", "%d image extensions available from the OS: %s",
                  nfmt, g_img_exts[0] ? g_img_exts : "(none)");
    }

    /* GLFW knows exactly why it failed. Ask it, rather than guessing.
     *
     * This used to say "no display?" whatever happened -- the right guess on a
     * headless box and useless everywhere else. A missing GLX extension, a
     * driver that cannot create a context, and an unreachable X server are
     * three different problems with three different fixes, and they all
     * looked identical from here. */
    glfwSetErrorCallback(on_glfw_error);

    if (!glfwInit()) {
        log_write("boot", "glfwInit failed: %s",
                  g_glfw_err[0] ? g_glfw_err : "no reason given");
        fprintf(stderr, "glfwInit failed: %s\n",
                g_glfw_err[0] ? g_glfw_err : "no reason given");
        log_close();
        return 1;
    }
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES, 4);

    g.window = glfwCreateWindow(ORB_WIN_SIZE, ORB_WIN_SIZE,
                                "ORB_Recorder", NULL, NULL);

    /* Multisampling is the first thing a sick driver refuses. Retry without
     * it before concluding there is no 3D at all. */
    if (!g.window) {
        log_write("boot", "GL window failed with MSAA -- retrying without");
        glfwWindowHint(GLFW_SAMPLES, 0);
        g.window = glfwCreateWindow(ORB_WIN_SIZE, ORB_WIN_SIZE,
                                    "ORB_Recorder", NULL, NULL);
    }

    /* Still nothing: no usable GL. Do NOT exit.
     *
     * This is the line that lost the program when Joe's 4090 went to code 43
     * on 2026-08-14 -- the orb vanished along with the driver, taking the
     * recorder with it. But recording never needed OpenGL: capture is GDI and
     * Media Foundation. Only the PICTURE of the orb needs a context. So drop
     * to a window with no client API, paint it with 2D primitives, and keep
     * every capture path alive. */
    if (!g.window) {
        log_write("boot", "no usable GL context -- starting in 2D mode");
        g_no3d = true;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_FALSE);
        g.window = glfwCreateWindow(ORB_WIN_SIZE, ORB_WIN_SIZE,
                                    "ORB_Recorder", NULL, NULL);
    }

    if (!g.window) {
        log_write("boot", "could not create a window at all -- giving up");
        fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate(); log_close();
        return 1;
    }
    if (!g_no3d) glfwMakeContextCurrent(g.window);
    glfwSwapInterval(1);

    /* Restore the parked orb position -- but only onto a monitor that
     * exists right now. Otherwise fall back to a visible default. */
    if (g.have_anchor) {
        /* Resolve "that corner of that display". If the display is asleep or
         * gone, we still honour the CORNER on whatever is available -- so it
         * lands bottom-right of the primary rather than at some stale pixel,
         * and returns to the real screen when it wakes. */
        int nx, ny; bool exact = false;
        if (anchor_to_position(ORB_SIZE, ORB_SIZE, &nx, &ny, &exact)) {
            clamp_orb_to_monitors(&nx, &ny);
            g.orb_x = nx; g.orb_y = ny;
            /* Sync the preference too. Without this settings_save() keeps
             * writing whatever coordinate was loaded -- so an off-screen
             * position survives every restart even after we have recovered
             * from it. */
            g.want_orb_x = nx; g.want_orb_y = ny;
            g.orb_displaced = !exact;
            orb_move_to(g.orb_x, g.orb_y);
            /* Recompute from where it actually landed: an anchor that was
             * saturated at 100%/100% by a bad drag would otherwise keep
             * pinning the orb into the very corner, under the taskbar. */
            anchor_from_position(g.orb_x, g.orb_y, ORB_SIZE, ORB_SIZE);
            settings_mark_dirty();
            log_write("boot", "orb anchored to %s at %d.%d%%/%d.%d%% -> (%d,%d)%s",
                      g.anchor_mon[0] ? g.anchor_mon : "(primary)",
                      g.anchor_fx / 10, g.anchor_fx % 10,
                      g.anchor_fy / 10, g.anchor_fy % 10, nx, ny,
                      exact ? "" : "  [preferred display absent -- will return]");
        }
    } else if (g.have_orb_pos &&
               rect_on_live_monitor(g.want_orb_x, g.want_orb_y, ORB_SIZE, ORB_SIZE)) {
        /* Settings written before anchors existed. */
        g.orb_x = g.want_orb_x; g.orb_y = g.want_orb_y;
        orb_move_to(g.orb_x, g.orb_y);
        anchor_from_position(g.orb_x, g.orb_y, ORB_SIZE, ORB_SIZE);
        log_write("boot", "restored orb position (%d,%d), anchor derived",
                  g.orb_x, g.orb_y);
    } else {
        /* First run: park it bottom-right -- the same corner the recovery
         * path below already picks. This was a hardcoded (50,50), the
         * top-left, which is where titlebars, menus and half the world's
         * window buttons live, so a new user's first sight of the orb was of
         * it sitting on top of something. Nobody here noticed, because every
         * machine we own has a settings file older than the question. */
        PlatMonitor mons[GIF_MAX_MONITORS];
        int n = plat_enum_monitors(mons, GIF_MAX_MONITORS);
        if (n > 0) {
            int pi = primary_monitor_index(mons, n);
            g.orb_x = mons[pi].x + mons[pi].w - g.orb_size * 2;
            g.orb_y = mons[pi].y + mons[pi].h - g.orb_size * 3;
        } else {
            g.orb_x = 50; g.orb_y = 50;
        }
        g.want_orb_x = g.orb_x;
        g.want_orb_y = g.orb_y;
        g.have_orb_pos = true;
        g.orb_displaced = false;
        orb_move_to(g.orb_x, g.orb_y);
        anchor_from_position(g.orb_x, g.orb_y, ORB_SIZE, ORB_SIZE);
        log_write("boot", "first run: parked the orb at (%d,%d) on %d monitor(s)",
                  g.orb_x, g.orb_y, n);
    }
    plat_window_setup(g.window);
    /* setup() forces the window to 50,50 -- put it back. orb_move_to(), NOT
     * plat_window_move(): the window is three times the orb and the orb sits
     * centred in it, so passing orb coordinates straight to the window puts
     * the orb one full orb_size down and right. Against a bottom-right
     * anchor that is enough to push it off the screen entirely. */
    orb_move_to(g.orb_x, g.orb_y);
    /* Belt and braces: whatever the anchor produced, if the orb did not end
     * up on a live monitor, fall back to somewhere visible. A mispositioned
     * orb is invisible and therefore unrecoverable without editing the
     * settings file by hand -- worth one cheap check at startup. */
    if (!rect_on_live_monitor(g.orb_x, g.orb_y, g.orb_size, g.orb_size)) {
        log_write("boot", "orb at (%d,%d) is off every live monitor -- recovering",
                  g.orb_x, g.orb_y);
        PlatMonitor mons[GIF_MAX_MONITORS];
        int n = plat_enum_monitors(mons, GIF_MAX_MONITORS);
        if (n > 0) {
            int pi = primary_monitor_index(mons, n);
            g.orb_x = mons[pi].x + mons[pi].w - g.orb_size * 2;
            g.orb_y = mons[pi].y + mons[pi].h - g.orb_size * 3;
        } else {
            g.orb_x = 50; g.orb_y = 50;
        }
        g.want_orb_x = g.orb_x; g.want_orb_y = g.orb_y;
        orb_move_to(g.orb_x, g.orb_y);
        anchor_from_position(g.orb_x, g.orb_y, g.orb_size, g.orb_size);
        settings_mark_dirty();
    }

    /* If capture cannot work here, say so at once and in the log, rather
     * than letting the first screenshot come back black and look saved. */
    {
        const char* why = plat_capture_unavailable();
        if (why) {
            log_write("boot", "CAPTURE UNAVAILABLE: %s", why);
            popup_mode("PNG", "NO CAPTURE", "%s", why);
        }
    }

    /* Recording must survive our own popup menus. */
    plat_set_modal_tick(modal_tick);
    plat_window_load_icon(g.window);
    /* Only the round part of the square window is the window -- clicks in
     * the transparent corners go to whatever is behind it. */
    orb_apply_region(false);
    if (g.tray_only) {
        plat_tray_set(g.window, true, "ORB_Recorder");
        orb_apply_region(false);        /* the hide/show cycle drops the region */
    }
    if (g_orb_hidden) {
        /* Hidden last time. Only honour that if the tray icon is there to get
         * it back -- an invisible program with no way to reach it is not a
         * state worth restoring faithfully. */
        if (g.tray_only) plat_window_set_visible(g.window, false);
        else             g_orb_hidden = false;
    }
    g.native = plat_get_orb_native_handle(g.window);
    log_write("boot", "window set up; native handle acquired");
    log_write("boot", "integrity: %s",
              plat_process_is_elevated() ? "ELEVATED (admin)" : "normal user");

    /* Honour whatever the settings file says, per key. */
    for (int i = 0; i < PLAT_HK_COUNT; i++) {
        if (g.hotkey_on[i]) apply_hotkey(i, true);
    }
    if (!g.hotkey_on[0]) {
        popup_mode("F5", "BUSY", "F5 is unavailable or released.");
        g.state = ORB_NOCAPTURE;
    }
    /* PrintScreen is the one key another component of Windows itself is
     * likely to hold: Win11 binds it to Snipping Tool out of the box. Say
     * so, rather than leaving a checkbox that silently will not tick. */
    if (!g.hotkey_on[6]) {
        log_write("hotkey", "PrintScreen unavailable -- Windows 11 binds it to "
                            "Snipping Tool by default (Settings > Accessibility "
                            "> Keyboard > 'Use the Print screen key...'). F4 works "
                            "regardless.");
    }

    glfwSetMouseButtonCallback(g.window, on_mouse_button);
    glfwSetScrollCallback(g.window, on_scroll);
    glfwSetKeyCallback(g.window, on_key);
    glfwSetDropCallback(g.window, on_drop);
    plat_window_allow_drops(g.window);
    init_gl();

    /* Hold the intended position for the first second. Placement is correct
     * when the setup sequence ends -- traced -- but showing, restyling and
     * shaping a window are all things the OS can answer by moving it, and a
     * one-second re-assert costs nothing and removes the whole class of
     * "came back in the wrong corner". */
    int settle_x = g.orb_x, settle_y = g.orb_y;
    uint64_t settle_until = plat_now_ms() + 1000;

    popup("READY", "ORB_Recorder ready. F4/PrtSc = shot, F5/F6 = GIF, F7/F8 = video, F9 = camera.");
    ping_start_for(PING_MS_LONG);      /* linger -- this is how you find it */
    settings_mark_dirty();             /* persist startup geometry promptly */
    uint64_t lastTopMost = plat_now_ms();

    while (!glfwWindowShouldClose(g.window)) {
        /* Hotkeys are latched by the platform when the key is pressed, not
         * read out of the queue here, so it no longer matters which pump
         * happens to be running -- GLFW's, or the modal loop behind our own
         * popup menu. Both used to eat WM_HOTKEY and lose the keystroke. */
        int hk;
        while ((hk = plat_poll_hotkey()) != 0) {
            if (hk == PLAT_HK_F5)     handle_f5();
            if (hk == PLAT_HK_F6)     start_recording_region();
            if (hk == PLAT_HK_F7)     handle_f7();
            if (hk == PLAT_HK_F8)     handle_f8();
            if (hk == PLAT_HK_F9)     start_camera_record(-1);   /* any free one */
            if (hk == PLAT_HK_F4)     handle_shot_region();
            if (hk == PLAT_HK_PRTSC)  handle_shot_region();
            if (hk == PLAT_HK_ESCAPE) handle_escape();
        }
        glfwPollEvents();

        /* Brought forward, restored, or the tray icon clicked -> locate. */
        if (plat_poll_activation()) ping_start_for(PING_MS_LONG);
        int tray = plat_poll_tray();
        if (tray == PLAT_TRAY_LOCATE && g_orb_hidden) {
            /* Bring it back rather than pinging something invisible. */
            g_orb_hidden = false;
            plat_window_set_visible(g.window, true);
            orb_move_to(g.orb_x, g.orb_y);
            log_write("orb", "restored from the tray");
            tray = PLAT_TRAY_NONE;
            ping_start_for(PING_MS_LONG);
        }
        if (tray == PLAT_TRAY_LOCATE) {
            ping_start_for(PING_MS_LONG);
        } else if (tray == PLAT_TRAY_MENU) {
            refresh_monitors();
            PlatMenuState st = menu_state();
            handle_menu_command(plat_show_menu(g.window, &st,
                                               g.monitors, g.nmonitors));
        }

        if (settle_until && plat_now_ms() < settle_until) {
            if (!g.isDragging) {
                int wx, wy, want_x, want_y;
                glfwGetWindowPos(g.window, &wx, &wy);
                /* Ask the same function that placed it. This used to open-code
                 * `settle_x - g.orb_size`, which is exactly the formula that
                 * orb_sync_from_window's comment warns about: the window is a
                 * fixed size, so the inset depends on the orb size rather than
                 * being the orb size. The window was always precisely where it
                 * belonged and this declared it drifted, every frame, for the
                 * whole settle period, on both platforms. Harmless apart from
                 * burying the boot log -- which is where it was finally
                 * noticed, on a display with no window manager at all. */
                orb_window_pos(settle_x, settle_y, &want_x, &want_y);
                if (wx != want_x || wy != want_y) {
                    log_write("boot", "window at (%d,%d), wanted (%d,%d)"
                              " -- re-asserting orb (%d,%d)",
                              wx, wy, want_x, want_y, settle_x, settle_y);
                    g.orb_x = settle_x; g.orb_y = settle_y;
                    orb_move_to(settle_x, settle_y);
                }
            }
        } else if (settle_until) {
            settle_until = 0;
            orb_sync_from_window();     /* trust the window from here on */
        }

        if (g.pending_edit[0]) {
            char open_me[sizeof g.pending_edit];
            snprintf(open_me, sizeof open_me, "%s", g.pending_edit);
            g.pending_edit[0] = 0;
            if (ed_open_path(open_me)) g.ed_browsing = false;
        }
        tick_drag();
        tick_armed_picker();
        tick_recording();
        ping_tick();
        toast_region_tick();
        tick_monitor_rehome();
        ed_tick();
        settings_tick();

        /* The shell restores the taskbar button asynchronously after any
         * SetForegroundWindow -- which the right-click menu has to do -- so
         * re-applying immediately after the menu was too early and the button
         * came back anyway. Enforcing it on a timer beats guessing when the
         * shell has finished; the call returns at once when the style is
         * already right, so this is free in the normal case. */
        {
            static uint64_t next_style_ms = 0;
            uint64_t now_ms = plat_now_ms();
            if (now_ms >= next_style_ms) {
                next_style_ms = now_ms + 1000;
                /* Not while hidden: the style change needs a hide/show
                 * cycle, and the show half would put the orb back on screen. */
                if (!g_orb_hidden &&
                    plat_taskbar_button_enforce(g.window, g.tray_only)) {
                    log_write("tray", "taskbar button had come back -- removed "
                                      "it again (shell re-added it after a menu)");
                }
            }
        }

        /* ---- draw the orb (its own context) ---- */
        draw_orb_frame();
        if (g_no3d) {
            plat_sleep_ms(33);          /* no vsync to pace us in 2D */
            continue;
        }

        /* ---- help window ---- */
        if (g_help_win) {
            if (glfwWindowShouldClose(g_help_win)) {
                help_close();
            } else {
                glfwMakeContextCurrent(g_help_win);
                int hw, hh; glfwGetFramebufferSize(g_help_win, &hw, &hh);
                glViewport(0, 0, hw, hh);
                help_draw(hw, hh);
                glfwSwapBuffers(g_help_win);
            }
        }

        /* ---- draw the editor, if open (separate window + context) ---- */
        if (g.ed_open && g.ed_window) {
            if (glfwWindowShouldClose(g.ed_window)) {
                ed_close();                     /* title-bar X closes editor only */
            } else {
                glfwMakeContextCurrent(g.ed_window);
                int ew, eh; glfwGetFramebufferSize(g.ed_window, &ew, &eh);
                glViewport(0, 0, ew, eh);
                ed_draw(ew, eh);
                glfwSwapBuffers(g.ed_window);
            }
        }

        uint64_t now = plat_now_ms();
        if (now - lastTopMost > 5000) {
            plat_window_topmost_refresh(g.window);
            lastTopMost = now;
        }
    }

    for (int i = 0; i < PLAT_HK_COUNT; i++) plat_unregister_hotkey(HK_IDS[i]);
    if (g.state == ORB_RECORDING) stop_recording();
    if (g.gw_open) { GifEnd(&g.gw); g.gw_open = false; }
    if (g.ed_open) ed_close();
    help_close();
    if (g.tray_only) plat_tray_set(g.window, false, NULL);
    settings_save();                   /* always persist geometry on exit */
    glfwDestroyWindow(g.window);
    glfwTerminate();
    log_close();
    return 0;
}
