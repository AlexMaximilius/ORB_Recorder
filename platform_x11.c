/*
 * platform_x11.c -- Linux / X11 implementation of platform.h.
 *
 * Alex Maz -- ORB_Recorder (2026) -- Linux port.
 *
 * Screen capture:    XShmGetImage from the root window's DefaultRootWindow
 *                    (fast, shared-memory image transfer). Rect for
 *                    per-window capture is computed from the target Window's
 *                    root-relative geometry.
 * Global hotkeys:    XGrabKey on the root window, polled with XCheckIfEvent
 *                    so only grab-window deliveries are taken and ordinary
 *                    keystrokes stay in the queue for GLFW.
 * Monitor enum:      XRandR (Xrandr_CrtcInfo). Falls back to single monitor
 *                    at (0,0, root_w, root_h) if XRandR isn't available.
 * Right-click menu:  spawns `yad --notification`-style is heavy; we render a
 *                    simple owned popup (transient window with hit-tested rows).
 *                    Not fancy, but works with zero non-X deps.
 * Folder paths:      $XDG_PICTURES_DIR (parsed from user-dirs.dirs) with
 *                    ~/Pictures fallback. Log at $XDG_STATE_HOME or
 *                    ~/.local/state/ORB_Recorder/log.txt.
 * Background jobs:   pthread_create + pthread_detach.
 * Reveal file:       xdg-open on the containing directory.
 *
 * Build (Debian/Ubuntu):
 *   sudo apt install libglfw3-dev libx11-dev libxext-dev libxrandr-dev \
 *                    libgl1-mesa-dev libglu1-mesa-dev
 *   ./build.sh
 */

/* Feature test macros -- must come before ANY system include. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE          /* popen/pclose */

#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>

#include <sys/un.h>
#include <sys/socket.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <dlfcn.h>
#include <dirent.h>
#include <strings.h>

#include "platform.h"

/* Cached display + shortcut to the GLFW-owned display. */
static Display* dpy(void) {
    static Display* d = NULL;
    if (!d) d = glfwGetX11Display();
    return d;
}

/* ---- time ------------------------------------------------------------- */
uint64_t plat_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}
void plat_sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- paths ------------------------------------------------------------ */
static const char* home(void) {
    const char* h = getenv("HOME");
    return h ? h : "/tmp";
}

static void ensure_dir(const char* p) {
    mkdir(p, 0755);
}
static void mkdir_p(const char* path) {
    /* Simple recursive mkdir. */
    char buf[512]; strncpy(buf, path, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (char* c = buf + 1; *c; c++) {
        if (*c == '/') { *c = 0; ensure_dir(buf); *c = '/'; }
    }
    ensure_dir(buf);
}

void plat_get_log_path(char* out, size_t sz) {
    const char* xdg = getenv("XDG_STATE_HOME");
    if (xdg && xdg[0]) {
        snprintf(out, sz, "%s/ORB_Recorder", xdg);
    } else {
        snprintf(out, sz, "%s/.local/state/ORB_Recorder", home());
    }
    mkdir_p(out);
    strncat(out, "/log.txt", sz - strlen(out) - 1);
}

/* Parse ~/.config/user-dirs.dirs for XDG_PICTURES_DIR (a shell-syntax file
 * with lines like XDG_PICTURES_DIR="$HOME/Pictures"). Best-effort. */
static bool find_pictures_dir(char* out, size_t sz) {
    char path[512];
    snprintf(path, sizeof path, "%s/.config/user-dirs.dirs", home());
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof line, f)) {
        const char* key = "XDG_PICTURES_DIR=";
        char* p = strstr(line, key);
        if (!p || p != line) continue;
        p += strlen(key);
        /* Strip leading quote */
        if (*p == '"') p++;
        /* Substitute $HOME */
        char expanded[512];
        if (strncmp(p, "$HOME", 5) == 0) {
            snprintf(expanded, sizeof expanded, "%s%s", home(), p + 5);
        } else {
            strncpy(expanded, p, sizeof expanded - 1);
            expanded[sizeof expanded - 1] = 0;
        }
        /* Strip trailing quote + newline */
        for (char* c = expanded; *c; c++) {
            if (*c == '"' || *c == '\n' || *c == '\r') { *c = 0; break; }
        }
        snprintf(out, sz, "%s", expanded);
        found = true;
        break;
    }
    fclose(f);
    return found;
}

void plat_get_output_dir(char* out, size_t sz) {
    char pics[512];
    if (!find_pictures_dir(pics, sizeof pics)) {
        snprintf(pics, sizeof pics, "%s/Pictures", home());
    }
    snprintf(out, sz, "%s/ORB_Recorder", pics);
    mkdir_p(out);
}

void plat_get_config_path(char* out, size_t sz) {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) snprintf(out, sz, "%s/ORB_Recorder", xdg);
    else               snprintf(out, sz, "%s/.config/ORB_Recorder", home());
    mkdir_p(out);
    strncat(out, "/settings.ini", sz - strlen(out) - 1);

    /* Same migration as Win32: a rename must not reset the user's setup. */
    if (access(out, F_OK) != 0) {
        char legacy[600];
        if (xdg && xdg[0]) snprintf(legacy, sizeof legacy, "%s/GIF_Recorder/settings.ini", xdg);
        else               snprintf(legacy, sizeof legacy, "%s/.config/GIF_Recorder/settings.ini", home());
        FILE* src = fopen(legacy, "rb");
        if (src) {
            FILE* dst = fopen(out, "wb");
            if (dst) {
                char buf[4096]; size_t n;
                while ((n = fread(buf, 1, sizeof buf, src)) > 0) fwrite(buf, 1, n, dst);
                fclose(dst);
            }
            fclose(src);
        }
    }
}

/* X11: the window manager handles taskbar/dock clicks and we get a normal
 * focus-in. GLFW surfaces that via its own focus callback, so there is no
 * extra plumbing to do here -- report "no event" and let the core rely on
 * its startup ping. */
bool plat_poll_activation(void) { return false; }

/* X11 has no UIPI equivalent: any client that can open the display can read
 * the root window, so root-owned windows capture like any other. Nothing to
 * detect and nothing to escalate. */
bool plat_process_is_elevated(void)        { return geteuid() == 0; }
bool plat_window_is_elevated(void* handle) { (void)handle; return false; }
bool plat_restart_elevated(void)           { return false; }
void plat_window_allow_drops(struct GLFWwindow* w) { (void)w; }

/* ---- system tray ------------------------------------------------------
 * A real tray icon on X11 means XEmbed/StatusNotifierItem and a DBus
 * dependency, which is more than this is worth right now. Report "off" so
 * the core keeps the taskbar button. */
void plat_tray_set(struct GLFWwindow* w, bool on, const char* tooltip) {
    (void)w; (void)tooltip;
    if (on) fprintf(stderr, "[gif_orb] tray-only is not implemented on X11 yet.\n");
}
int plat_poll_tray(void) { return PLAT_TRAY_NONE; }

/* ---- native window ---------------------------------------------------- */
static Atom atom(const char* name) {
    return XInternAtom(dpy(), name, False);
}

/* Ask the WM for always-on-top + skip-taskbar-off + sticky. */
static void set_wm_state(Window w, const char* prop, bool on) {
    Atom netWmState = atom("_NET_WM_STATE");
    Atom p = atom(prop);
    XEvent ev = {0};
    ev.xclient.type = ClientMessage;
    ev.xclient.message_type = netWmState;
    ev.xclient.window = w;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = on ? 1 : 0;   /* _NET_WM_STATE_ADD/REMOVE */
    ev.xclient.data.l[1] = p;
    ev.xclient.data.l[2] = 0;
    ev.xclient.data.l[3] = 1;            /* source: normal application */
    XSendEvent(dpy(), DefaultRootWindow(dpy()), False,
               SubstructureNotifyMask | SubstructureRedirectMask, &ev);
}

void plat_window_setup(struct GLFWwindow* w) {
    Window x11w = glfwGetX11Window(w);

    /* Take the orb out of the window manager's hands.
     *
     * The orb is drawn in the middle of a window several times its size, so
     * that rings and toasts have room outside the body -- which means putting
     * the orb near a corner requires the WINDOW at negative coordinates. A
     * real window manager refuses: mutter clamped a request for (-276,-276)
     * to (66,32) and the orb landed 340 pixels from where it was asked to go,
     * every launch. Xvfb has no window manager, so this looked perfect there.
     *
     * override-redirect says "this is furniture, not an application window",
     * which is what a desktop widget is; docks and on-screen displays have
     * done it for decades. XMoveWindow is then honoured exactly. The window
     * has to be unmapped for the change to take, so it is a hide/show cycle
     * -- the same dance the Win32 side does for its taskbar-button styles.
     *
     * Only the orb. The editor and the region selector are ordinary windows
     * and want the WM: titlebar, focus, alt-tab, the lot. */
    XUnmapWindow(dpy(), x11w);
    XSync(dpy(), False);
    XSetWindowAttributes attr;
    attr.override_redirect = True;
    XChangeWindowAttributes(dpy(), x11w, CWOverrideRedirect, &attr);
    XMapRaised(dpy(), x11w);
    XSync(dpy(), False);

    /* Still ask for above + sticky: harmless when unmanaged, and correct if a
     * compositor honours the hints anyway. */
    set_wm_state(x11w, "_NET_WM_STATE_ABOVE", true);
    set_wm_state(x11w, "_NET_WM_STATE_STICKY", true);
    XFlush(dpy());
}

void plat_window_set_rect(struct GLFWwindow* w, int x, int y, int width, int height) {
    /* XMoveResizeWindow is the single-request form; X applies it as one
     * ConfigureWindow, so there is no intermediate size-without-move. */
    XMoveResizeWindow(dpy(), glfwGetX11Window(w), x, y,
                      (unsigned)width, (unsigned)height);
    XFlush(dpy());
}

void plat_window_move(struct GLFWwindow* w, int x, int y) {
    glfwSetWindowPos(w, x, y);   /* portable */
}

void plat_window_topmost_refresh(struct GLFWwindow* w) {
    Window x11w = glfwGetX11Window(w);
    /* Re-assert ABOVE. Some WMs strip it on focus changes. */
    set_wm_state(x11w, "_NET_WM_STATE_ABOVE", true);
    XRaiseWindow(dpy(), x11w);
    XFlush(dpy());
}

/* Build a simple orange-orb icon in memory and hand it to GLFW.
 * Keeps zero file dependency; matches the visual. */
static void render_orb_icon(uint8_t* rgba, int size) {
    double cx = (size - 1) / 2.0, cy = cx;
    double rmax = size / 2.0 - 0.5;
    for (int y = 0; y < size; y++)
    for (int x = 0; x < size; x++) {
        double dx = x - cx, dy = y - cy;
        double d = (dx*dx + dy*dy);
        double r = rmax * rmax;
        int i = (y * size + x) * 4;
        if (d > r) { rgba[i]=0; rgba[i+1]=0; rgba[i+2]=0; rgba[i+3]=0; continue; }
        double t = 1.0 - (d / r);
        double br = 0.65 + 0.35 * t;
        rgba[i+0] = (uint8_t)(255 * br);
        rgba[i+1] = (uint8_t)(140 * br);
        rgba[i+2] = (uint8_t)( 25 * br);
        rgba[i+3] = 255;
    }
}

/* XShape is the X11 equivalent of SetWindowRgn. ShapeBounding controls what
 * is painted; ShapeInput controls what receives events. */
void plat_window_set_circular(struct GLFWwindow* w, int diameter) {
    Window x11w = glfwGetX11Window(w);
    int major = 0, minor = 0;
    if (!XShapeQueryVersion(dpy(), &major, &minor)) return;

    if (diameter <= 0) {
        XShapeCombineMask(dpy(), x11w, ShapeBounding, 0, 0, None, ShapeSet);
        XShapeCombineMask(dpy(), x11w, ShapeInput,    0, 0, None, ShapeSet);
        XFlush(dpy());
        return;
    }
    Pixmap mask = XCreatePixmap(dpy(), x11w, diameter, diameter, 1);
    GC gc = XCreateGC(dpy(), mask, 0, NULL);
    XSetForeground(dpy(), gc, 0);
    XFillRectangle(dpy(), mask, gc, 0, 0, diameter, diameter);
    XSetForeground(dpy(), gc, 1);
    XFillArc(dpy(), mask, gc, 0, 0, diameter, diameter, 0, 360 * 64);
    XFreeGC(dpy(), gc);
    XShapeCombineMask(dpy(), x11w, ShapeBounding, 0, 0, mask, ShapeSet);
    XShapeCombineMask(dpy(), x11w, ShapeInput,    0, 0, mask, ShapeSet);
    XFreePixmap(dpy(), mask);
    XFlush(dpy());
}

void plat_window_set_shape(struct GLFWwindow* w,
                           int bx, int by, int diameter,
                           int rx, int ry, int rw, int rh) {
    Window x11w = glfwGetX11Window(w);
    int major = 0, minor = 0;
    if (!XShapeQueryVersion(dpy(), &major, &minor)) return;
    if (diameter <= 0) {
        XShapeCombineMask(dpy(), x11w, ShapeBounding, 0, 0, None, ShapeSet);
        XShapeCombineMask(dpy(), x11w, ShapeInput,    0, 0, None, ShapeSet);
        XFlush(dpy());
        return;
    }
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy(), x11w, &wa)) return;
    Pixmap mask = XCreatePixmap(dpy(), x11w, wa.width, wa.height, 1);
    GC gc = XCreateGC(dpy(), mask, 0, NULL);
    XSetForeground(dpy(), gc, 0);
    XFillRectangle(dpy(), mask, gc, 0, 0, wa.width, wa.height);
    XSetForeground(dpy(), gc, 1);
    XFillArc(dpy(), mask, gc, bx, by, diameter, diameter, 0, 360 * 64);
    if (rw > 0 && rh > 0) XFillRectangle(dpy(), mask, gc, rx, ry, rw, rh);
    XFreeGC(dpy(), gc);
    XShapeCombineMask(dpy(), x11w, ShapeBounding, 0, 0, mask, ShapeSet);
    XShapeCombineMask(dpy(), x11w, ShapeInput,    0, 0, mask, ShapeSet);
    XFreePixmap(dpy(), mask);
    XFlush(dpy());
}

void plat_window_set_clickthrough(struct GLFWwindow* w, bool on) {
    Window x11w = glfwGetX11Window(w);
    int major = 0, minor = 0;
    if (!XShapeQueryVersion(dpy(), &major, &minor)) return;
    if (on) {
        /* Empty input region = every event falls through to what is below. */
        XRectangle none = { 0, 0, 0, 0 };
        XShapeCombineRectangles(dpy(), x11w, ShapeInput, 0, 0,
                                &none, 1, ShapeSet, Unsorted);
    } else {
        XShapeCombineMask(dpy(), x11w, ShapeInput, 0, 0, None, ShapeSet);
    }
    XFlush(dpy());
}

void plat_window_load_icon(struct GLFWwindow* w) {
    static uint8_t buf[64 * 64 * 4];
    render_orb_icon(buf, 64);
    GLFWimage img = { 64, 64, buf };
    glfwSetWindowIcon(w, 1, &img);
}

/* ---- pointing & focus ------------------------------------------------- */
void plat_get_cursor(int* x, int* y) {
    Window root_ret, child_ret;
    int rx, ry, wx, wy;
    unsigned int mask;
    XQueryPointer(dpy(), DefaultRootWindow(dpy()),
                  &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask);
    *x = rx; *y = ry;
}

bool plat_left_button_down(void) {
    Window r, c; int rx, ry, wx, wy; unsigned int mask;
    XQueryPointer(dpy(), DefaultRootWindow(dpy()),
                  &r, &c, &rx, &ry, &wx, &wy, &mask);
    return (mask & Button1Mask) != 0;
}

/* Return the top-level Window under the cursor. Walks up from
 * XQueryPointer's child using _NET_WM_STATE presence as a heuristic;
 * simpler: return the child of root at that point. */
void* plat_window_at_cursor(void) {
    Window root = DefaultRootWindow(dpy());
    Window child = None, r; int rx, ry, wx, wy; unsigned int mask;
    XQueryPointer(dpy(), root, &r, &child, &rx, &ry, &wx, &wy, &mask);
    return (void*)(uintptr_t)child;
}

/* _NET_ACTIVE_WINDOW property on root -> the currently focused window. */
void* plat_get_foreground_window(void) {
    Atom act = atom("_NET_ACTIVE_WINDOW");
    Atom actualType; int actualFormat; unsigned long nItems, bytesAfter;
    unsigned char* data = NULL;
    Window w = None;
    if (XGetWindowProperty(dpy(), DefaultRootWindow(dpy()), act, 0, 1, False,
                           XA_WINDOW, &actualType, &actualFormat,
                           &nItems, &bytesAfter, &data) == Success && data) {
        if (nItems > 0) w = *(Window*)data;
        XFree(data);
    }
    return (void*)(uintptr_t)w;
}

void* plat_get_orb_native_handle(struct GLFWwindow* w) {
    return (void*)(uintptr_t)glfwGetX11Window(w);
}

bool plat_handles_equal(void* a, void* b) { return a == b; }

/* ---- capture ---------------------------------------------------------- */
static XImage* g_shm_img = NULL;
static XShmSegmentInfo g_shm_info;
static int g_shm_w = 0, g_shm_h = 0;

static bool ensure_shm(int w, int h) {
    if (g_shm_img && g_shm_w == w && g_shm_h == h) return true;
    if (g_shm_img) {
        XShmDetach(dpy(), &g_shm_info);
        XDestroyImage(g_shm_img);
        shmdt(g_shm_info.shmaddr);
        shmctl(g_shm_info.shmid, IPC_RMID, NULL);
        g_shm_img = NULL;
    }
    int screen = DefaultScreen(dpy());
    Visual* vis = DefaultVisual(dpy(), screen);
    int depth  = DefaultDepth(dpy(), screen);
    g_shm_img = XShmCreateImage(dpy(), vis, depth, ZPixmap, NULL,
                                &g_shm_info, w, h);
    if (!g_shm_img) return false;
    g_shm_info.shmid = shmget(IPC_PRIVATE,
        g_shm_img->bytes_per_line * g_shm_img->height, IPC_CREAT | 0600);
    if (g_shm_info.shmid < 0) { XDestroyImage(g_shm_img); g_shm_img = NULL; return false; }
    g_shm_info.shmaddr = (char*)shmat(g_shm_info.shmid, NULL, 0);
    g_shm_img->data = g_shm_info.shmaddr;
    g_shm_info.readOnly = False;
    XShmAttach(dpy(), &g_shm_info);
    XSync(dpy(), False);
    g_shm_w = w; g_shm_h = h;
    return true;
}

/* Nearest-neighbor resample of an XImage (BGRX/BGRA 32-bit typical on Linux)
 * into an RGBA malloc'd buffer of size capW*capH. */
static uint8_t* resample_to_rgba(XImage* img, int capW, int capH) {
    uint8_t* out = (uint8_t*)malloc(capW * capH * 4);
    if (!out) return NULL;
    int sw = img->width, sh = img->height;
    /* We assume 32bpp (bits_per_pixel==32); typical X11 root visual. */
    int Bpp = img->bits_per_pixel / 8;
    int red_shift = 16, green_shift = 8, blue_shift = 0;   /* BGRX by default */
    /* If the XImage advertises different masks, honor them. */
    unsigned long rm = img->red_mask, gm = img->green_mask, bm = img->blue_mask;
    if (rm && gm && bm) {
        /* find shift for each mask */
        int rs = 0, gs = 0, bs = 0;
        unsigned long m;
        for (m = rm; m && !(m & 1); m >>= 1) rs++;
        for (m = gm; m && !(m & 1); m >>= 1) gs++;
        for (m = bm; m && !(m & 1); m >>= 1) bs++;
        red_shift = rs; green_shift = gs; blue_shift = bs;
    }
    for (int y = 0; y < capH; y++) {
        int sy = y * sh / capH;
        for (int x = 0; x < capW; x++) {
            int sx = x * sw / capW;
            uint8_t* pix = (uint8_t*)img->data + sy * img->bytes_per_line + sx * Bpp;
            uint32_t v =  (uint32_t)pix[0]        | ((uint32_t)pix[1] <<  8) |
                         ((uint32_t)pix[2] << 16) | ((uint32_t)pix[3] << 24);
            int i = (y * capW + x) * 4;
            out[i+0] = (uint8_t)((v >> red_shift)   & 0xFF);
            out[i+1] = (uint8_t)((v >> green_shift) & 0xFF);
            out[i+2] = (uint8_t)((v >> blue_shift)  & 0xFF);
            out[i+3] = 255;
        }
    }
    return out;
}

uint8_t* plat_capture_rect(int x, int y, int w, int h, int capW, int capH) {
    if (w <= 0 || h <= 0) return NULL;
    if (!ensure_shm(w, h)) return NULL;
    if (!XShmGetImage(dpy(), DefaultRootWindow(dpy()), g_shm_img, x, y, AllPlanes)) {
        return NULL;
    }
    return resample_to_rgba(g_shm_img, capW, capH);
}

uint8_t* plat_capture_window(void* native_handle, int capW, int capH,
                             char* title_out, size_t title_size,
                             int* src_w, int* src_h) {
    Window win = (Window)(uintptr_t)native_handle;
    if (win == None) return NULL;
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy(), win, &wa)) return NULL;
    int rx = 0, ry = 0;
    Window dummy;
    XTranslateCoordinates(dpy(), win, DefaultRootWindow(dpy()),
                          0, 0, &rx, &ry, &dummy);
    if (src_w) *src_w = wa.width;
    if (src_h) *src_h = wa.height;
    if (title_out && title_size > 0) {
        title_out[0] = 0;
        char* n = NULL;
        if (XFetchName(dpy(), win, &n) && n) {
            strncpy(title_out, n, title_size - 1);
            title_out[title_size - 1] = 0;
            XFree(n);
        }
    }
    if (wa.width <= 0 || wa.height <= 0) return NULL;
    return plat_capture_rect(rx, ry, wa.width, wa.height, capW, capH);
}

int plat_enum_monitors(PlatMonitor* out, int max) {
    int n = 0;
    XRRScreenResources* sr = XRRGetScreenResources(dpy(), DefaultRootWindow(dpy()));
    if (sr) {
        for (int i = 0; i < sr->ncrtc && n < max; i++) {
            XRRCrtcInfo* ci = XRRGetCrtcInfo(dpy(), sr, sr->crtcs[i]);
            if (!ci) continue;
            if (ci->width > 0 && ci->height > 0 && ci->mode != None) {
                PlatMonitor* m = &out[n++];
                m->x = ci->x; m->y = ci->y;
                m->w = (int)ci->width; m->h = (int)ci->height;
                snprintf(m->name, sizeof m->name, "Monitor %d  (%dx%d)",
                         n, m->w, m->h);
                /* Stable identity: the XRandR output name, e.g. HDMI-1. */
                m->id[0] = 0;
                if (ci->noutput > 0) {
                    XRROutputInfo* oi = XRRGetOutputInfo(dpy(), sr, ci->outputs[0]);
                    if (oi) {
                        snprintf(m->id, sizeof m->id, "%s", oi->name);
                        XRRFreeOutputInfo(oi);
                    }
                }
                if (!m->id[0]) snprintf(m->id, sizeof m->id, "idx%d", n - 1);
            }
            XRRFreeCrtcInfo(ci);
        }
        XRRFreeScreenResources(sr);
    }
    if (n == 0 && max > 0) {
        /* Fallback: single monitor at root size. */
        int screen = DefaultScreen(dpy());
        out[0].x = 0; out[0].y = 0;
        out[0].w = DisplayWidth(dpy(), screen);
        out[0].h = DisplayHeight(dpy(), screen);
        snprintf(out[0].name, sizeof out[0].name, "Monitor 1  (%dx%d)",
                 out[0].w, out[0].h);
        snprintf(out[0].id, sizeof out[0].id, "idx0");
        n = 1;
    }
    return n;
}

/* ---- hotkeys (root-window XGrabKey) ---------------------------------- */
static unsigned int g_f5_kc = 0, g_esc_kc = 0, g_f6_kc = 0, g_f7_kc = 0,
                    g_f8_kc = 0, g_f9_kc = 0, g_f4_kc = 0, g_prt_kc = 0;

bool plat_register_hotkey(int id) {
    Window root = DefaultRootWindow(dpy());
    if (id == PLAT_HK_F5) {
        g_f5_kc = XKeysymToKeycode(dpy(), XK_F5);
        XGrabKey(dpy(), g_f5_kc, AnyModifier, root, True,
                 GrabModeAsync, GrabModeAsync);
        XSelectInput(dpy(), root, KeyPressMask);
        XFlush(dpy());
        return true;
    }
    if (id == PLAT_HK_PRTSC) {
        g_prt_kc = XKeysymToKeycode(dpy(), XK_Print);
        XGrabKey(dpy(), g_prt_kc, AnyModifier, root, True,
                 GrabModeAsync, GrabModeAsync);
        XSelectInput(dpy(), root, KeyPressMask);
        XFlush(dpy());
        return true;
    }
    if (id == PLAT_HK_F4) {
        g_f4_kc = XKeysymToKeycode(dpy(), XK_F4);
        XGrabKey(dpy(), g_f4_kc, AnyModifier, root, True,
                 GrabModeAsync, GrabModeAsync);
        XSelectInput(dpy(), root, KeyPressMask);
        XFlush(dpy());
        return true;
    }
    if (id == PLAT_HK_F9) {
        g_f9_kc = XKeysymToKeycode(dpy(), XK_F9);
        XGrabKey(dpy(), g_f9_kc, AnyModifier, root, True,
                 GrabModeAsync, GrabModeAsync);
        XSelectInput(dpy(), root, KeyPressMask);
        XFlush(dpy());
        return true;
    }
    if (id == PLAT_HK_F8) {
        g_f8_kc = XKeysymToKeycode(dpy(), XK_F8);
        XGrabKey(dpy(), g_f8_kc, AnyModifier, root, True,
                 GrabModeAsync, GrabModeAsync);
        XSelectInput(dpy(), root, KeyPressMask);
        XFlush(dpy());
        return true;
    }
    if (id == PLAT_HK_F8) {
        g_f8_kc = XKeysymToKeycode(dpy(), XK_F8);
        XGrabKey(dpy(), g_f8_kc, AnyModifier, root, True,
                 GrabModeAsync, GrabModeAsync);
        XSelectInput(dpy(), root, KeyPressMask);
        XFlush(dpy());
        return true;
    }
    if (id == PLAT_HK_F7) {
        g_f7_kc = XKeysymToKeycode(dpy(), XK_F7);
        XGrabKey(dpy(), g_f7_kc, AnyModifier, root, True,
                 GrabModeAsync, GrabModeAsync);
        XSelectInput(dpy(), root, KeyPressMask);
        XFlush(dpy());
        return true;
    }
    if (id == PLAT_HK_F6) {
        g_f6_kc = XKeysymToKeycode(dpy(), XK_F6);
        XGrabKey(dpy(), g_f6_kc, AnyModifier, root, True,
                 GrabModeAsync, GrabModeAsync);
        XSelectInput(dpy(), root, KeyPressMask);
        XFlush(dpy());
        return true;
    }
    if (id == PLAT_HK_ESCAPE) {
        g_esc_kc = XKeysymToKeycode(dpy(), XK_Escape);
        XGrabKey(dpy(), g_esc_kc, AnyModifier, root, True,
                 GrabModeAsync, GrabModeAsync);
        XSelectInput(dpy(), root, KeyPressMask);
        XFlush(dpy());
        return true;
    }
    return false;
}

void plat_unregister_hotkey(int id) {
    Window root = DefaultRootWindow(dpy());
    if (id == PLAT_HK_F5 && g_f5_kc) {
        XUngrabKey(dpy(), g_f5_kc, AnyModifier, root);
        g_f5_kc = 0;
    }
    if (id == PLAT_HK_ESCAPE && g_esc_kc) {
        XUngrabKey(dpy(), g_esc_kc, AnyModifier, root);
        g_esc_kc = 0;
    }
    if (id == PLAT_HK_F6 && g_f6_kc) {
        XUngrabKey(dpy(), g_f6_kc, AnyModifier, root);
        g_f6_kc = 0;
    }
    if (id == PLAT_HK_F7 && g_f7_kc) {
        XUngrabKey(dpy(), g_f7_kc, AnyModifier, root);
        g_f7_kc = 0;
    }
    if (id == PLAT_HK_F8 && g_f8_kc) {
        XUngrabKey(dpy(), g_f8_kc, AnyModifier, root);
        g_f8_kc = 0;
    }
    if (id == PLAT_HK_F9 && g_f9_kc) {
        XUngrabKey(dpy(), g_f9_kc, AnyModifier, root);
        g_f9_kc = 0;
    }
    if (id == PLAT_HK_F4 && g_f4_kc) {
        XUngrabKey(dpy(), g_f4_kc, AnyModifier, root);
        g_f4_kc = 0;
    }
    if (id == PLAT_HK_PRTSC && g_prt_kc) {
        XUngrabKey(dpy(), g_prt_kc, AnyModifier, root);
        g_prt_kc = 0;
    }
    if (id == PLAT_HK_F8 && g_f8_kc) {
        XUngrabKey(dpy(), g_f8_kc, AnyModifier, root);
        g_f8_kc = 0;
    }
    XFlush(dpy());
}

static int hotkey_for_keycode(unsigned int kc) {
    if (!kc) return 0;
    if (g_f5_kc  && kc == g_f5_kc)  return PLAT_HK_F5;
    if (g_esc_kc && kc == g_esc_kc) return PLAT_HK_ESCAPE;
    if (g_f6_kc  && kc == g_f6_kc)  return PLAT_HK_F6;
    if (g_f7_kc  && kc == g_f7_kc)  return PLAT_HK_F7;
    if (g_f8_kc  && kc == g_f8_kc)  return PLAT_HK_F8;
    if (g_f9_kc  && kc == g_f9_kc)  return PLAT_HK_F9;
    if (g_f4_kc  && kc == g_f4_kc)  return PLAT_HK_F4;
    if (g_prt_kc && kc == g_prt_kc) return PLAT_HK_PRTSC;
    return 0;
}

/* Take ONLY the grabbed keys, and leave everything else where it was.
 *
 * This used to be XCheckMaskEvent(KeyPressMask), which pulls every KeyPress
 * off the connection -- including the ones on their way to our own windows,
 * because GLFW shares this display -- and dropped anything that was not a
 * registered hotkey. So no key could ever be typed: the editor's tool
 * shortcuts, Ctrl+S and the whole text tool were silently inert while the
 * mouse worked perfectly.
 *
 * That is exactly the Windows bug fixed in 2.1, with the roles reversed.
 * There a foreign message pump ate our WM_HOTKEY; here we were the thief.
 *
 * XGrabKey delivers to its grab window -- the root -- so the event's window
 * field is what separates a hotkey from an ordinary keystroke. XCheckIfEvent
 * removes only what the predicate accepts and disturbs nothing else. */
static Bool grabbed_key_only(Display* d, XEvent* ev, XPointer arg) {
    (void)arg;
    if (ev->type != KeyPress) return False;
    if (ev->xkey.window != DefaultRootWindow(d)) return False;
    return hotkey_for_keycode(ev->xkey.keycode) ? True : False;
}

int plat_poll_hotkey(void) {
    XEvent ev;
    if (XCheckIfEvent(dpy(), &ev, grabbed_key_only, NULL))
        return hotkey_for_keycode(ev.xkey.keycode);
    return 0;
}

/* ---- right-click menu -------------------------------------------------
 * V1 Linux menu: since we don't want a GTK dep, we shell out to `zenity`
 * or `yad` if present. This is not fancy but works everywhere those are
 * installed. If neither exists we log a hint and return 0 (no action).
 * Monitor recording is fully-scriptable via the hotkey path anyway.        */

static char* which(const char* prog) {
    static char buf[256];
    char cmd[128];
    snprintf(cmd, sizeof cmd, "command -v %s 2>/dev/null", prog);
    FILE* f = popen(cmd, "r");
    if (!f) return NULL;
    if (!fgets(buf, sizeof buf, f)) { pclose(f); return NULL; }
    pclose(f);
    /* strip trailing newline */
    for (char* c = buf; *c; c++) if (*c == '\n') { *c = 0; break; }
    return buf[0] ? buf : NULL;
}

int plat_show_menu(struct GLFWwindow* w, const PlatMenuState* st,
                   const PlatMonitor* monitors, int nmonitors) {
    (void)w;
    char* zenity = which("zenity");
    if (!zenity) {
        fprintf(stderr, "[gif_orb] right-click menu unavailable: install `zenity`.\n");
        return 0;
    }
    char cmd[4096];
    int n = snprintf(cmd, sizeof cmd,
        "zenity --list --title=\"ORB_Recorder\" --column=Action "
        "\"GIF: record window\" \"GIF: record region\" "
        "\"MP4: record window\" \"MP4: record region\" "
        "\"F5 hotkey %s\" \"F6 hotkey %s\" \"F7 hotkey %s\" \"F8 hotkey %s\" "
        "\"Sound source %s\" "
        "\"Open folder after saving %s\" \"Copy file to clipboard %s\" "
        "\"Double-click arms %s\" ",
        st->hotkey_on[0] ? "[ON]" : "[OFF]",
        st->hotkey_on[1] ? "[ON]" : "[OFF]",
        st->hotkey_on[2] ? "[ON]" : "[OFF]",
        st->hotkey_on[3] ? "[ON]" : "[OFF]",
        st->audio_src == 0 ? "[OFF]" : st->audio_src == 1 ? "[SYSTEM]"
                           : st->audio_src == 2 ? "[MIC]" : "[BOTH]",
        st->auto_open ? "[ON]" : "[OFF]",
        st->auto_clip ? "[ON]" : "[OFF]",
        st->dbl_video ? "MP4" : "GIF");
    for (int i = 0; i < nmonitors && n < (int)sizeof cmd - 128; i++)
        n += snprintf(cmd + n, sizeof cmd - n, "\"Record %s\" ", monitors[i].name);
    snprintf(cmd + n, sizeof cmd - n, "\"Help\" \"Edit settings file\" \"Quit\" 2>/dev/null");

    FILE* f = popen(cmd, "r");
    if (!f) return 0;
    char pick[256] = {0};
    if (!fgets(pick, sizeof pick, f)) { pclose(f); return 0; }
    pclose(f);
    for (char* c = pick; *c; c++) if (*c == '\n') { *c = 0; break; }

    if (!strncmp(pick, "F5 hotkey", 9)) return PLAT_MENU_HOTKEY_BASE + 0;
    if (!strncmp(pick, "F6 hotkey", 9)) return PLAT_MENU_HOTKEY_BASE + 1;
    if (!strncmp(pick, "F7 hotkey", 9)) return PLAT_MENU_HOTKEY_BASE + 2;
    if (!strncmp(pick, "F8 hotkey", 9)) return PLAT_MENU_HOTKEY_BASE + 3;
    if (strstr(pick, "GIF: record window")) return PLAT_MENU_RECORD_GIF;
    if (strstr(pick, "GIF: record region")) return PLAT_MENU_RECORD_REGION;
    if (strstr(pick, "MP4: record window")) return PLAT_MENU_RECORD_VIDEO;
    if (strstr(pick, "MP4: record region")) return PLAT_MENU_RECORD_VIDEO_RGN;
    if (strstr(pick, "Sound source"))       return PLAT_MENU_AUDIO_BASE
                                                 + ((st->audio_src + 1) & 3);
    if (strstr(pick, "Open folder"))        return PLAT_MENU_TOGGLE_AUTOOPEN;
    if (strstr(pick, "Copy file"))          return PLAT_MENU_TOGGLE_CLIP;
    if (strstr(pick, "Double-click arms"))  return st->dbl_video
                                                 ? PLAT_MENU_DBL_GIF
                                                 : PLAT_MENU_DBL_MP4;
    if (strstr(pick, "Help"))               return PLAT_MENU_HELP;
    if (strstr(pick, "Edit settings"))      return PLAT_MENU_EDIT_SETTINGS;
    if (!strcmp(pick, "Quit"))              return PLAT_MENU_QUIT;
    for (int i = 0; i < nmonitors; i++) {
        char want[96];
        snprintf(want, sizeof want, "Record %s", monitors[i].name);
        if (!strcmp(pick, want)) return PLAT_MENU_MONITOR_BASE + i;
    }
    return 0;
}

/* ---- image helpers ----------------------------------------------------
 * This is the counterpart to IShellItemImageFactory on Windows: the "let
 * the OS decode it" fallback, used for anything stb_image and simplewebp
 * cannot read themselves.
 *
 * gdk-pixbuf is the right service for it -- it has a loader registry, the
 * same shape as Windows Imaging Component, and on a desktop install that
 * covers SVG, PNM, XPM, ICNS, TIFF and more. dlopen'd rather than linked,
 * so it stays optional: no gdk-pixbuf means fewer formats, not a broken
 * binary.
 *
 * Unlike the Windows path this returns FULL resolution, not a thumbnail --
 * gdk-pixbuf decodes properly rather than handing back a cached tile. */
uint8_t* plat_shell_thumbnail(const char* path, int want_px, int* out_w, int* out_h) {
    (void)want_px;
    if (!path || !out_w || !out_h) return NULL;

    void* lib = dlopen("libgdk_pixbuf-2.0.so.0", RTLD_LAZY);
    if (!lib) lib = dlopen("libgdk_pixbuf-2.0.so", RTLD_LAZY);
    if (!lib) return NULL;

    void*    (*from_file)(const char*, void**) = dlsym(lib, "gdk_pixbuf_new_from_file");
    int      (*get_w)(void*)                   = dlsym(lib, "gdk_pixbuf_get_width");
    int      (*get_h)(void*)                   = dlsym(lib, "gdk_pixbuf_get_height");
    int      (*get_stride)(void*)              = dlsym(lib, "gdk_pixbuf_get_rowstride");
    int      (*get_nchan)(void*)               = dlsym(lib, "gdk_pixbuf_get_n_channels");
    uint8_t* (*get_px)(void*)                  = dlsym(lib, "gdk_pixbuf_get_pixels");

    uint8_t* out = NULL;
    if (from_file && get_w && get_h && get_stride && get_nchan && get_px) {
        void* err = NULL;
        void* pb = from_file(path, &err);
        if (pb) {
            int w = get_w(pb), h = get_h(pb);
            int stride = get_stride(pb), nch = get_nchan(pb);
            uint8_t* src = get_px(pb);
            if (w > 0 && h > 0 && src && (nch == 3 || nch == 4)) {
                out = (uint8_t*)malloc((size_t)w * h * 4);
                if (out) {
                    for (int y = 0; y < h; y++) {
                        const uint8_t* srow = src + (size_t)y * stride;
                        uint8_t* drow = out + (size_t)y * w * 4;
                        for (int x = 0; x < w; x++) {
                            drow[x*4 + 0] = srow[x*nch + 0];
                            drow[x*4 + 1] = srow[x*nch + 1];
                            drow[x*4 + 2] = srow[x*nch + 2];
                            drow[x*4 + 3] = (nch == 4) ? srow[x*nch + 3] : 255;
                        }
                    }
                    *out_w = w; *out_h = h;
                }
            }
            /* Release via glib, which gdk-pixbuf already depends on. */
            void* glib = dlopen("libgobject-2.0.so.0", RTLD_LAZY);
            if (glib) {
                void (*unref)(void*) = dlsym(glib, "g_object_unref");
                if (unref) unref(pb);
                dlclose(glib);
            }
        }
    }
    dlclose(lib);
    return out;
}

static bool is_image_ext_(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    static const char* exts[] = {
        ".jpg",".jpeg",".png",".gif",".bmp",".tga",".webp",".tif",".tiff",
        ".heic",".avif",".jfif",".psd",".ico", NULL
    };
    for (int i = 0; exts[i]; i++) if (strcasecmp(dot, exts[i]) == 0) return true;
    return false;
}
static int cmp_str_ci_(const void* a, const void* b) {
    return strcasecmp(*(const char**)a, *(const char**)b);
}

int plat_list_sibling_images(const char* path, char** out, int max, int* out_self) {
    if (out_self) *out_self = 0;
    if (!path || !*path || max <= 0) return 0;

    char dir[512];
    strncpy(dir, path, sizeof dir - 1); dir[sizeof dir - 1] = 0;
    char* slash = strrchr(dir, '/');
    const char* self_name = path;
    if (slash) { *slash = 0; self_name = path + (slash - dir) + 1; }
    else       { strcpy(dir, "."); }

    DIR* d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!is_image_ext_(de->d_name)) continue;
        if (n >= max) break;
        size_t need = strlen(dir) + 1 + strlen(de->d_name) + 1;
        char* full = (char*)malloc(need);
        if (!full) break;
        snprintf(full, need, "%s/%s", dir, de->d_name);
        out[n++] = full;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof(char*), cmp_str_ci_);
    if (out_self) {
        for (int i = 0; i < n; i++) {
            const char* base = strrchr(out[i], '/');
            base = base ? base + 1 : out[i];
            if (strcasecmp(base, self_name) == 0) { *out_self = i; break; }
        }
    }
    return n;
}

/* Linux DOES have an equivalent of Windows Imaging Component: gdk-pixbuf
 * keeps a loader registry, and gdk_pixbuf_get_formats() reports every
 * decoder installed along with its extensions -- JPEG, PNG, TIFF, and
 * whatever else the distribution shipped (HEIF, AVIF and JXL loaders all
 * exist as separate packages).
 *
 * It is loaded with dlopen rather than linked, on purpose. libgdk_pixbuf is
 * present on essentially every desktop install but is NOT a build
 * dependency this way -- if it is missing we simply fall back to the
 * formats we decode ourselves, and the binary still runs. That keeps the
 * "one self-contained file" property while still getting the OS's codecs
 * for free where they exist. */

typedef struct _GSList GSList;
struct _GSList { void* data; GSList* next; };

int plat_list_image_extensions(char* out, size_t sz) {
    if (!out || sz == 0) return 0;
    out[0] = 0;
    int count = 0;

    void* lib = dlopen("libgdk_pixbuf-2.0.so.0", RTLD_LAZY);
    if (!lib) lib = dlopen("libgdk_pixbuf-2.0.so", RTLD_LAZY);
    if (lib) {
        GSList*  (*get_formats)(void)                = dlsym(lib, "gdk_pixbuf_get_formats");
        char**   (*get_ext)(void*)                   = dlsym(lib, "gdk_pixbuf_format_get_extensions");
        void     (*strfreev)(char**)                 = NULL;

        /* g_strfreev lives in glib, which gdk-pixbuf already pulled in. */
        void* glib = dlopen("libglib-2.0.so.0", RTLD_LAZY);
        if (glib) strfreev = dlsym(glib, "g_strfreev");

        if (get_formats && get_ext) {
            GSList* fmts = get_formats();
            for (GSList* it = fmts; it; it = it->next) {
                char** exts = get_ext(it->data);
                if (!exts) continue;
                for (int i = 0; exts[i]; i++) {
                    char dotted[40];
                    snprintf(dotted, sizeof dotted, ".%s,", exts[i]);
                    for (char* c = dotted; *c; c++)
                        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
                    if (!strstr(out, dotted)) {
                        size_t cur = strlen(out);
                        if (cur + strlen(dotted) + 1 < sz) {
                            strcat(out, dotted);
                            count++;
                        }
                    }
                }
                if (strfreev) strfreev(exts);
            }
        }
        if (glib) dlclose(glib);
        dlclose(lib);
    }

    /* Union our own decoders on top -- stb and simplewebp cover things the
     * system loaders may not, and vice versa. */
    static const char* MINE[] = { ".jpg",".jpeg",".png",".gif",".bmp",
                                  ".tga",".psd",".webp", NULL };
    for (int i = 0; MINE[i]; i++) {
        char dotted[16];
        snprintf(dotted, sizeof dotted, "%s,", MINE[i]);
        if (!strstr(out, dotted)) {
            size_t cur = strlen(out);
            if (cur + strlen(dotted) + 1 < sz) { strcat(out, dotted); count++; }
        }
    }
    return count;
}

/* ---- open-file dialog + default handler ------------------------------- */

bool plat_open_file_dialog(char* out, size_t out_sz) {
    char* zenity = which("zenity");
    if (!zenity) {
        fprintf(stderr, "[gif_orb] open dialog needs `zenity`.\n");
        return false;
    }
    char dir[512]; plat_get_output_dir(dir, sizeof dir);
    char cmd[1200];
    snprintf(cmd, sizeof cmd,
        "zenity --file-selection --title='Open in ORB_Recorder' "
        "--filename='%s/' "
        "--file-filter='Images and GIFs | *.gif *.jpg *.jpeg *.png *.bmp *.tga *.psd *.webp *.tif *.tiff' "
        "--file-filter='All files | *' 2>/dev/null", dir);
    FILE* f = popen(cmd, "r");
    if (!f) return false;
    if (!fgets(out, (int)out_sz, f)) { pclose(f); return false; }
    pclose(f);
    for (char* c = out; *c; c++) if (*c == '\n' || *c == '\r') { *c = 0; break; }
    return out[0] != 0;
}

void plat_open_with_default_app(const char* path) {
    pid_t pid = fork();
    if (pid == 0) {
        execlp("xdg-open", "xdg-open", path, (char*)NULL);
        _exit(1);
    }
}

/* Needs a persistent selection owner to serve image data; xclip can do it. */
void plat_clipboard_copy_image(const uint8_t* rgba, int w, int h) {
    (void)rgba; (void)w; (void)h;
}

/* ---- 2D fallback painting ---------------------------------------------
 * Xlib arcs into a pixmap, then one copy to the window. Same reasoning as
 * the Win32 side: the window's shape mask makes it round, so this only has
 * to fill, and it needs no GLX context. */
void plat_draw_orb_2d(struct GLFWwindow* w, int win_size, int orb_size,
                      unsigned char cr, unsigned char cg, unsigned char cb) {
    Display* d = dpy();
    if (!d) return;
    Window win = glfwGetX11Window(w);
    int scr = DefaultScreen(d);

    Pixmap pm = XCreatePixmap(d, win, (unsigned)win_size, (unsigned)win_size,
                              (unsigned)DefaultDepth(d, scr));
    GC gc = XCreateGC(d, pm, 0, NULL);

    XSetForeground(d, gc, 0x181820);
    XFillRectangle(d, pm, gc, 0, 0, (unsigned)win_size, (unsigned)win_size);

    int cx = win_size / 2, cy = win_size / 2, r = orb_size / 2;

    /* Core first, then the ring on top. */
    XSetForeground(d, gc, ((unsigned long)cr << 16) |
                          ((unsigned long)cg << 8)  | (unsigned long)cb);
    int r2 = (r * 62) / 100;
    XFillArc(d, pm, gc, cx - r2, cy - r2,
             (unsigned)(r2 * 2), (unsigned)(r2 * 2), 0, 360 * 64);

    XSetForeground(d, gc, 0xC8C8D2);
    XSetLineAttributes(d, gc, 2, LineSolid, CapButt, JoinMiter);
    XDrawArc(d, pm, gc, cx - r, cy - r,
             (unsigned)(r * 2), (unsigned)(r * 2), 0, 360 * 64);

    XCopyArea(d, pm, win, gc, 0, 0, (unsigned)win_size, (unsigned)win_size, 0, 0);
    XFreeGC(d, gc);
    XFreePixmap(d, pm);
    XFlush(d);
}

/* ---- single instance --------------------------------------------------
 * An abstract-namespace unix socket: bound to the kernel rather than the
 * filesystem, so it vanishes with the process and leaves nothing to go
 * stale. Same contract as the Windows named mutex. */
bool plat_single_instance(void) {
    static int held = -1;
    if (held >= 0) return true;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return true;                /* cannot tell -- do not block */
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    a.sun_path[0] = 0;                      /* leading NUL = abstract namespace */
    const char* name = "324x_ORB_Recorder_single";
    memcpy(a.sun_path + 1, name, strlen(name));
    socklen_t len = (socklen_t)(sizeof(a.sun_family) + 1 + strlen(name));
    if (bind(fd, (struct sockaddr*)&a, len) < 0) {
        close(fd);
        return false;                       /* someone else holds it */
    }
    held = fd;
    return true;
}

void plat_window_set_visible(struct GLFWwindow* w, bool visible) {
    if (visible) {
        glfwShowWindow(w);
        set_wm_state(glfwGetX11Window(w), "_NET_WM_STATE_ABOVE", true);
    } else {
        glfwHideWindow(w);
    }
}

bool plat_taskbar_button_enforce(struct GLFWwindow* w, bool hidden) {
    /* X has no equivalent of APPWINDOW overriding SKIP_TASKBAR, and asking
     * the WM for its current state costs a round trip, so just re-assert. */
    set_wm_state(glfwGetX11Window(w), "_NET_WM_STATE_SKIP_TASKBAR", hidden);
    return false;
}

/* X11 has no single menu window class -- toolkits each do their own, and
 * override-redirect covers tooltips and drag images too. Report none and
 * let the caller fall back to the monitor. */
bool plat_find_open_menu(int* x, int* y, int* w, int* h) {
    (void)x; (void)y; (void)w; (void)h; return false;
}

/* ---- run at login -----------------------------------------------------
 * freedesktop autostart: a .desktop file in $XDG_CONFIG_HOME/autostart.
 * Every mainstream desktop honours it, and the user can remove it from their
 * own session settings -- the same contract as the Windows registry entry.
 * Read the real state back rather than a remembered intention, so the
 * checkbox cannot disagree with the system. */

static void autostart_path(char* out, size_t sz) {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    char dir[512];
    if (xdg && xdg[0]) snprintf(dir, sizeof dir, "%s/autostart", xdg);
    else               snprintf(dir, sizeof dir, "%s/.config/autostart", home());
    mkdir_p(dir);
    snprintf(out, sz, "%s/orb_recorder.desktop", dir);
}

bool plat_get_run_at_startup(void) {
    char p[600];
    autostart_path(p, sizeof p);
    FILE* f = fopen(p, "r");
    if (!f) return false;
    fclose(f);
    return true;
}

bool plat_set_run_at_startup(bool on) {
    char p[600];
    autostart_path(p, sizeof p);
    if (!on) { remove(p); return true; }

    char exe[512] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return false;
    exe[n] = 0;

    FILE* f = fopen(p, "w");
    if (!f) return false;
    fprintf(f,
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=ORB_Recorder\n"
        "Comment=Always-on-top capture orb\n"
        "Exec=%s\n"
        "Terminal=false\n"
        "X-GNOME-Autostart-enabled=true\n", exe);
    fclose(f);
    return true;
}

/* ---- clipboard file-drop --------------------------------------------- *
 * X11 clipboards are per-client selection owners; a real file drop needs
 * a persistent daemon (xclip / wl-copy). Shell out when one exists. */
void plat_clipboard_copy_file(const char* file_path) {
    char* xclip = which("xclip");
    if (xclip) {
        char cmd[1024];
        snprintf(cmd, sizeof cmd,
                 "printf 'file://%%s\\n' '%s' | xclip -selection clipboard -t text/uri-list 2>/dev/null",
                 file_path);
        int rc = system(cmd);
        (void)rc;
        return;
    }
    char* wl = which("wl-copy");
    if (wl) {
        char cmd[1024];
        snprintf(cmd, sizeof cmd,
                 "printf 'file://%%s\\n' '%s' | wl-copy --type text/uri-list 2>/dev/null",
                 file_path);
        int rc = system(cmd);
        (void)rc;
        return;
    }
    fprintf(stderr, "[gif_orb] clipboard copy unavailable: install xclip or wl-copy.\n");
}

/* No font engine is linked here on purpose: pulling in Xft/fontconfig for
 * one string would be the first real dependency in the whole program. NULL
 * tells the caller to use the built-in glyphs, which is honest and visible
 * rather than silently dropping the text. */
uint8_t* plat_render_text(const char* s, int px_height, int* out_w, int* out_h) {
    (void)s; (void)px_height; (void)out_w; (void)out_h;
    return NULL;
}

/* A Wayland session hands X clients an XWayland server whose root window is
 * not the desktop: it contains only other X clients, and on GNOME not even
 * those. XShmGetImage succeeds and returns black. `import` and `xwd` fail
 * outright on the same display, which is the tell.
 *
 * Ask the X SERVER, not the environment. XWayland advertises an extension
 * called XWAYLAND, and that is true however the process was started -- from a
 * desktop launcher, from cron, or over an ssh session that inherits none of
 * the session variables. The environment check was written first and reported
 * a perfectly healthy X11 session in exactly the case it existed to catch.
 *
 * Not detected by capturing and inspecting the pixels: a legitimately black
 * screenshot exists, and guessing from them would eventually accuse the wrong
 * person. Capturing a Wayland desktop properly means the xdg-desktop-portal
 * ScreenCast API over PipeWire -- a real dependency and a real project, and
 * not something to fake. */
const char* plat_capture_unavailable(void) {
    int op = 0, ev = 0, err = 0;
    if (XQueryExtension(dpy(), "XWAYLAND", &op, &ev, &err))
        return "This is a Wayland session (XWayland). X11 capture comes back "
               "black here -- the desktop is not in the X root window. Log out "
               "and pick an Xorg session for capture to work.";

    /* Secondary hint, for compositors that do not advertise the extension. */
    const char* t = getenv("XDG_SESSION_TYPE");
    const char* w = getenv("WAYLAND_DISPLAY");
    if ((t && strcasecmp(t, "wayland") == 0) || (w && w[0]))
        return "This looks like a Wayland session. X11 capture comes back "
               "black there -- log out and pick an Xorg session instead.";
    return NULL;
}

bool plat_open_in_editor(const char* path) {
    /* There is no "edit" verb in the freedesktop world -- xdg-open hands a
     * PNG to the default handler, which is a viewer. So ask for an editor by
     * name, in the order a desktop is likely to have one, and only fall back
     * to xdg-open so that SOMETHING opens rather than nothing happening. */
    static const char* ED[] = { "pinta", "kolourpaint", "drawing", "krita",
                                "gimp", "mtpaint", NULL };
    const char* chosen = NULL;
    for (int i = 0; ED[i]; i++) {
        char* p = which(ED[i]);
        if (p) { chosen = ED[i]; free(p); break; }
    }
    if (!chosen) {
        char* x = which("xdg-open");
        if (!x) return false;
        free(x);
        chosen = "xdg-open";
    }
    pid_t pid = fork();
    if (pid == 0) {
        execlp(chosen, chosen, path, (char*)NULL);
        _exit(1);
    }
    return pid > 0;                 /* parent: don't wait */
}

/* ---- reveal file ------------------------------------------------------ */
void plat_open_folder_select(const char* file_path) {
    /* xdg-open opens the parent dir; there's no cross-DE "select file" verb.
     * Some file managers accept a URL like file:///path; xdg-open of the
     * dir is the portable minimum.                                        */
    char dir[512]; strncpy(dir, file_path, sizeof dir - 1); dir[sizeof dir - 1] = 0;
    char* slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    pid_t pid = fork();
    if (pid == 0) {
        execlp("xdg-open", "xdg-open", dir, (char*)NULL);
        _exit(1);
    }
    /* parent: don't wait */
}

/* ---- video recording --------------------------------------------------
 * Not implemented on Linux yet. The honest options are libx264 + libavformat
 * (a real dependency, unlike everything else vendored here) or piping raw
 * frames to ffmpeg (only works if the user has it). Neither belongs in a
 * self-contained binary without a decision first -- see IDEAS.md. */
bool plat_video_start(const char* path, int w, int h, int fps, int audio_src) {
    (void)path; (void)w; (void)h; (void)fps; (void)audio_src;
    fprintf(stderr, "[gif_orb] video recording is not implemented on Linux yet.\n");
    return false;
}
bool plat_video_write_frame(const uint8_t* rgba, uint64_t t_ms) {
    (void)rgba; (void)t_ms; return false;
}
bool plat_video_has_audio(void) { return false; }

/* Camera needs V4L2 here; it rides on the same missing encoder, so it is
 * reported as absent rather than half-implemented. */
/* V4L2 would answer with an O_RDWR open returning EBUSY, but camera capture
 * is not implemented on X11 at all, so claim nothing rather than wrongly. */
bool plat_camera_in_use(int index) { (void)index; return false; }
void plat_camera_last_error(char* out, size_t sz) {
    snprintf(out, sz, "camera capture is not implemented on Linux yet.");
}

int plat_camera_list(char names[][64], int max) { (void)names; (void)max; return 0; }
bool plat_camera_open(int index, int* out_w, int* out_h) {
    (void)index; (void)out_w; (void)out_h; return false;
}
uint8_t* plat_camera_read(int capW, int capH) { (void)capW; (void)capH; return NULL; }
void plat_camera_close(void) {}
void plat_video_stop(void) {}

/* ---- background jobs -------------------------------------------------- */
typedef struct { PlatJobFn fn; void* arg; } PxJob;
static void* pthread_trampoline(void* p) {
    PxJob* j = (PxJob*)p;
    j->fn(j->arg);
    free(j);
    return NULL;
}
/* The X11 menu is drawn and driven by our own code rather than by a
 * platform modal loop, so nothing blocks the caller and there is nothing
 * to keep alive. Accept the registration and ignore it. */
void plat_set_modal_tick(PlatModalTickFn fn) { (void)fn; }

void plat_run_background(PlatJobFn fn, void* arg) {
    PxJob* j = (PxJob*)malloc(sizeof *j);
    j->fn = fn; j->arg = arg;
    pthread_t th;
    if (pthread_create(&th, NULL, pthread_trampoline, j) != 0) {
        fn(arg); free(j);
        return;
    }
    pthread_detach(th);
}
