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
 * Right-click menu:  an override-redirect Xlib window with hit-tested rows,
 *                    drawn with XDrawString. No GTK, no zenity, no blocking.
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
#include <stdarg.h>
#include <errno.h>
#include <pthread.h>
#include <dlfcn.h>
#include <dirent.h>
#include <strings.h>
#include <ctype.h>

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
/* No tray here yet: a real one means XEmbed or StatusNotifierItem over DBus,
 * which is more than it is worth today. Saying so is the important part --
 * the core refuses to hide the orb when there is nowhere to bring it back
 * from, instead of hiding it and stranding whoever asked. */
bool plat_tray_available(void) { return false; }

void plat_tray_set(struct GLFWwindow* w, bool on, const char* tooltip) {
    (void)w; (void)tooltip; (void)on;
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

/* ---- right-click menu -------------------------------------------------
 *
 * A real popup, drawn here with Xlib.
 *
 * This used to shell out to `zenity --list` through popen(), which was wrong
 * three times over. It is a dialog box rather than a menu, so it looks
 * nothing like right-clicking anything else on the desktop. It blocks the
 * caller until the user answers, so the whole program stopped -- a region
 * selector left open behind it stopped repainting and stopped taking input,
 * which reads as "not responding", and no hotkey worked either. And it had
 * drifted: no screenshot items, no camera, no delayed shot, none of the
 * toggles added since.
 *
 * Xlib rather than OpenGL because a menu is rectangles and text, X has drawn
 * both since 1985, and it keeps the menu independent of whatever the GL
 * context is doing. Override-redirect, because a menu is not an application
 * window and no window manager should decorate, place or tab to it.
 *
 * The loop calls the modal tick, so a recording in progress keeps taking
 * frames while the menu is open -- the same guarantee the Windows side got in
 * 2.1, for the same reason.
 */

#define MENU_ROW_H   22
#define MENU_PAD_X   14
#define MENU_MAX     64

typedef struct {
    char label[72];
    int  id;          /* PLAT_MENU_* , or 0 for a separator */
    int  sep;
} MenuRow;

static PlatModalTickFn g_x11_modal_tick = NULL;
void plat_set_modal_tick(PlatModalTickFn fn) { g_x11_modal_tick = fn; }

static int menu_add(MenuRow* rows, int n, int id, const char* fmt, ...) {
    if (n >= MENU_MAX) return n;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rows[n].label, sizeof rows[n].label, fmt, ap);
    va_end(ap);
    rows[n].id = id;
    rows[n].sep = 0;
    return n + 1;
}

static int menu_sep(MenuRow* rows, int n) {
    if (n >= MENU_MAX) return n;
    rows[n].label[0] = 0;
    rows[n].id = 0;
    rows[n].sep = 1;
    return n + 1;
}

static const char* onoff(bool b) { return b ? "[on] " : "[  ] "; }

int plat_show_menu(struct GLFWwindow* w, const PlatMenuState* st,
                   const PlatMonitor* monitors, int nmonitors) {
    (void)w;
    Display* d = dpy();
    if (!d) return 0;

    /* ---- rows: the same menu the Windows build offers ---- */
    MenuRow rows[MENU_MAX];
    int n = 0;
    n = menu_add(rows, n, PLAT_MENU_SHOT_REGION,     "Screenshot: region            F4");
    n = menu_add(rows, n, PLAT_MENU_SHOT_WINDOW,     "Screenshot: window");
    n = menu_add(rows, n, PLAT_MENU_SHOT_DELAY_5,    "Screenshot: delayed 5s (captures menus)");
    n = menu_sep(rows, n);
    n = menu_add(rows, n, PLAT_MENU_RECORD_GIF,      "GIF: record window            F5");
    n = menu_add(rows, n, PLAT_MENU_RECORD_REGION,   "GIF: record region            F6");
    n = menu_add(rows, n, PLAT_MENU_RECORD_VIDEO,    "MP4: record window            F7");
    n = menu_add(rows, n, PLAT_MENU_RECORD_VIDEO_RGN,"MP4: record region            F8");
    for (int i = 0; i < nmonitors && i < 4; i++)
        n = menu_add(rows, n, PLAT_MENU_MONITOR_BASE + i, "GIF: record %s", monitors[i].name);
    for (int i = 0; i < nmonitors && i < 4; i++)
        n = menu_add(rows, n, PLAT_MENU_VMONITOR_BASE + i, "MP4: record %s", monitors[i].name);
    n = menu_sep(rows, n);
    {
        static const char* SRC[4] = { "off", "system", "microphone", "system + mic" };
        n = menu_add(rows, n, PLAT_MENU_AUDIO_BASE + ((st->audio_src + 1) & 3),
                     "Sound source: %s  (click to change)", SRC[st->audio_src & 3]);
        n = menu_add(rows, n, st->dbl_video ? PLAT_MENU_DBL_GIF : PLAT_MENU_DBL_MP4,
                     "Double-click arms: %s  (click to change)",
                     st->dbl_video ? "MP4" : "GIF");
    }
    n = menu_sep(rows, n);
    {
        static const char* HKN[PLAT_HK_COUNT] = { "F5","F6","F7","F8","F9","F4","PrtSc" };
        for (int i = 0; i < PLAT_HK_COUNT; i++)
            n = menu_add(rows, n, PLAT_MENU_HOTKEY_BASE + i,
                         "%sHotkey %s", onoff(st->hotkey_on[i]), HKN[i]);
    }
    n = menu_sep(rows, n);
    n = menu_add(rows, n, PLAT_MENU_TOGGLE_AUTOOPEN, "%sOpen folder after saving", onoff(st->auto_open));
    n = menu_add(rows, n, PLAT_MENU_TOGGLE_CLIP,     "%sCopy file to clipboard", onoff(st->auto_clip));
    {
        static const char* SE[3] = { "nothing", "the built-in editor", "the system editor" };
        n = menu_add(rows, n, PLAT_MENU_SHOT_ED_BASE + ((st->shot_editor + 1) % 3),
                     "After a screenshot: open %s  (click to change)",
                     SE[st->shot_editor % 3]);
    }
    n = menu_sep(rows, n);
    n = menu_add(rows, n, PLAT_MENU_OPEN_EDITOR,   "Open image or GIF...");
    n = menu_add(rows, n, PLAT_MENU_HELP,          "Help                          F1");
    n = menu_add(rows, n, PLAT_MENU_EDIT_SETTINGS, "Edit settings file...");
    n = menu_sep(rows, n);
    n = menu_add(rows, n, PLAT_MENU_QUIT,          "Quit");

    /* ---- font ---- */
    XFontStruct* fnt = XLoadQueryFont(d, "-*-dejavu sans mono-medium-r-*--13-*");
    if (!fnt) fnt = XLoadQueryFont(d, "9x15");
    if (!fnt) fnt = XLoadQueryFont(d, "fixed");
    if (!fnt) return 0;                      /* no core fonts: give up quietly */

    int wpx = 0;
    for (int i = 0; i < n; i++) {
        int lw = XTextWidth(fnt, rows[i].label, (int)strlen(rows[i].label));
        if (lw > wpx) wpx = lw;
    }
    wpx += MENU_PAD_X * 2;
    int hpx = 6;
    for (int i = 0; i < n; i++) hpx += rows[i].sep ? (MENU_ROW_H / 2) : MENU_ROW_H;
    hpx += 6;

    /* ---- place it at the pointer, kept on screen ---- */
    int px = 0, py = 0;
    plat_get_cursor(&px, &py);
    int scr = DefaultScreen(d);
    int sw = DisplayWidth(d, scr), sh = DisplayHeight(d, scr);
    if (px + wpx > sw) px = sw - wpx;
    if (py + hpx > sh) py = sh - hpx;
    if (px < 0) px = 0;
    if (py < 0) py = 0;

    XSetWindowAttributes wa;
    wa.override_redirect = True;
    wa.background_pixel  = 0x1E1E22;
    wa.border_pixel      = 0x505058;
    wa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                    PointerMotionMask | KeyPressMask | LeaveWindowMask;
    Window mw = XCreateWindow(d, DefaultRootWindow(d), px, py, (unsigned)wpx,
                              (unsigned)hpx, 1, CopyFromParent, InputOutput,
                              CopyFromParent,
                              CWOverrideRedirect | CWBackPixel | CWBorderPixel |
                              CWEventMask, &wa);
    XMapRaised(d, mw);

    GC gc = XCreateGC(d, mw, 0, NULL);
    XSetFont(d, gc, fnt->fid);

    /* Grab the pointer so a click anywhere dismisses, the way a menu should.
     * Not fatal if the grab is refused -- the menu still works, it just will
     * not see clicks that land on other windows. */
    XGrabPointer(d, mw, True,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    XGrabKeyboard(d, mw, True, GrabModeAsync, GrabModeAsync, CurrentTime);

    int hover = -1, chosen = 0, running = 1;

    while (running) {
        /* Keep the application alive: a recording in progress must not stop
         * because a menu is open. */
        if (g_x11_modal_tick) g_x11_modal_tick();

        while (XPending(d)) {
            XEvent ev;
            XNextEvent(d, &ev);
            if (ev.xany.window != mw) continue;
            if (ev.type == Expose) { hover = hover; }
            else if (ev.type == MotionNotify) {
                int y = 6, h2 = -1;
                for (int i = 0; i < n; i++) {
                    int rh = rows[i].sep ? (MENU_ROW_H / 2) : MENU_ROW_H;
                    if (ev.xmotion.y >= y && ev.xmotion.y < y + rh && !rows[i].sep)
                        h2 = i;
                    y += rh;
                }
                hover = h2;
            } else if (ev.type == ButtonPress) {
                if (ev.xbutton.x < 0 || ev.xbutton.y < 0 ||
                    ev.xbutton.x >= wpx || ev.xbutton.y >= hpx) {
                    running = 0;               /* clicked away */
                } else if (hover >= 0) {
                    chosen = rows[hover].id;
                    running = 0;
                }
            } else if (ev.type == KeyPress) {
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                if (ks == XK_Escape) running = 0;
                else if (ks == XK_Down || ks == XK_Up) {
                    int dir = (ks == XK_Down) ? 1 : -1;
                    int i = hover;
                    for (int step = 0; step < n; step++) {
                        i += dir;
                        if (i < 0) i = n - 1;
                        if (i >= n) i = 0;
                        if (!rows[i].sep) break;
                    }
                    hover = i;
                } else if (ks == XK_Return || ks == XK_KP_Enter) {
                    if (hover >= 0) chosen = rows[hover].id;
                    running = 0;
                }
            }
        }

        /* ---- paint ---- */
        XSetForeground(d, gc, 0x1E1E22);
        XFillRectangle(d, mw, gc, 0, 0, (unsigned)wpx, (unsigned)hpx);
        int y = 6;
        for (int i = 0; i < n; i++) {
            int rh = rows[i].sep ? (MENU_ROW_H / 2) : MENU_ROW_H;
            if (rows[i].sep) {
                XSetForeground(d, gc, 0x3A3A42);
                XDrawLine(d, mw, gc, 8, y + rh / 2, wpx - 8, y + rh / 2);
            } else {
                if (i == hover) {
                    XSetForeground(d, gc, 0xF08A1E);
                    XFillRectangle(d, mw, gc, 2, y, (unsigned)(wpx - 4), (unsigned)rh);
                    XSetForeground(d, gc, 0x1A1A1E);
                } else {
                    XSetForeground(d, gc, 0xE0E0E8);
                }
                XDrawString(d, mw, gc, MENU_PAD_X, y + MENU_ROW_H - 7,
                            rows[i].label, (int)strlen(rows[i].label));
            }
            y += rh;
        }
        XFlush(d);
        plat_sleep_ms(16);
    }

    XUngrabKeyboard(d, CurrentTime);
    XUngrabPointer(d, CurrentTime);
    XFreeGC(d, gc);
    XDestroyWindow(d, mw);
    XFreeFont(d, fnt);
    XFlush(d);
    return chosen;
}


/* ---- Wayland screenshots, via xdg-desktop-portal ----------------------
 *
 * On a Wayland session the X root window is not the desktop, so XShmGetImage
 * returns black. The compositor will not let any client read the screen
 * directly -- that is the entire point of the design -- and the sanctioned
 * route is the portal: a D-Bus service that takes the picture on our behalf,
 * after asking the user once whether that is allowed.
 *
 * org.freedesktop.portal.Screenshot hands back a whole screen as a PNG on
 * disk. There is no region variant, which suits this program: the capture
 * lands in the built-in editor and the crop tool is already there.
 *
 * libdbus is dlopen'd rather than linked, exactly like gdk-pixbuf above, so
 * the binary still runs on a machine without it -- one fewer feature, not a
 * failure to start. The headers are not needed either; the few types used
 * here are opaque pointers and the iterator is a fixed-size scratch buffer
 * that libdbus writes into, deliberately oversized.
 *
 * The call is asynchronous by design: Screenshot() returns a Request object
 * path, and the answer arrives later as a Response signal on it. The match
 * rule goes on BEFORE the call, or a fast compositor can answer before we are
 * listening. The whole thing runs on a background thread, because the user
 * may be looking at a permission dialog and the orb must not freeze while
 * they decide -- which is the mistake the zenity menu made.
 */

typedef struct DBusConnectionOpaque DBusConn;
typedef struct DBusMessageOpaque    DBusMsg;
typedef struct { void* pad[16]; } DBusIter;    /* real one is ~80 bytes */
typedef struct { void* pad[8];  } DBusErr;     /* real one is ~32 bytes */

#define DB_TYPE_INVALID     0
#define DB_TYPE_BOOLEAN     'b'
#define DB_TYPE_STRING      's'
#define DB_TYPE_UINT32      'u'
#define DB_TYPE_ARRAY       'a'
#define DB_TYPE_VARIANT     'v'
#define DB_TYPE_DICT_ENTRY  'e'
#define DB_TYPE_OBJECT_PATH 'o'
#define DB_BUS_SESSION      0

static struct {
    void* lib;
    int   tried;
    void        (*error_init)(DBusErr*);
    int         (*error_is_set)(const DBusErr*);
    void        (*error_free)(DBusErr*);
    DBusConn*   (*bus_get_private)(int, DBusErr*);
    void        (*connection_close)(DBusConn*);
    void        (*connection_unref)(DBusConn*);
    void        (*connection_set_exit_on_disconnect)(DBusConn*, int);
    const char* (*bus_get_unique_name)(DBusConn*);
    void        (*bus_add_match)(DBusConn*, const char*, DBusErr*);
    DBusMsg*    (*message_new_method_call)(const char*, const char*,
                                           const char*, const char*);
    void        (*message_unref)(DBusMsg*);
    void        (*message_iter_init_append)(DBusMsg*, DBusIter*);
    int         (*message_iter_append_basic)(DBusIter*, int, const void*);
    int         (*message_iter_open_container)(DBusIter*, int, const char*, DBusIter*);
    int         (*message_iter_close_container)(DBusIter*, DBusIter*);
    DBusMsg*    (*connection_send_with_reply_and_block)(DBusConn*, DBusMsg*, int, DBusErr*);
    int         (*connection_read_write_dispatch)(DBusConn*, int);
    DBusMsg*    (*connection_pop_message)(DBusConn*);
    int         (*message_is_signal)(DBusMsg*, const char*, const char*);
    const char* (*message_get_path)(DBusMsg*);
    int         (*message_iter_init)(DBusMsg*, DBusIter*);
    int         (*message_iter_next)(DBusIter*);
    int         (*message_iter_get_arg_type)(DBusIter*);
    void        (*message_iter_get_basic)(DBusIter*, void*);
    void        (*message_iter_recurse)(DBusIter*, DBusIter*);
} db;

static bool portal_load(void) {
    if (db.tried) return db.lib != NULL;
    db.tried = 1;
    db.lib = dlopen("libdbus-1.so.3", RTLD_LAZY);
    if (!db.lib) return false;
    #define DBSYM(field, name) \
        *(void**)(&db.field) = dlsym(db.lib, name); \
        if (!db.field) { dlclose(db.lib); db.lib = NULL; return false; }
    DBSYM(error_init,        "dbus_error_init")
    DBSYM(error_is_set,      "dbus_error_is_set")
    DBSYM(error_free,        "dbus_error_free")
    DBSYM(bus_get_private,   "dbus_bus_get_private")
    DBSYM(connection_close,  "dbus_connection_close")
    DBSYM(connection_unref,  "dbus_connection_unref")
    DBSYM(connection_set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect")
    DBSYM(bus_get_unique_name, "dbus_bus_get_unique_name")
    DBSYM(bus_add_match,     "dbus_bus_add_match")
    DBSYM(message_new_method_call, "dbus_message_new_method_call")
    DBSYM(message_unref,     "dbus_message_unref")
    DBSYM(message_iter_init_append, "dbus_message_iter_init_append")
    DBSYM(message_iter_append_basic, "dbus_message_iter_append_basic")
    DBSYM(message_iter_open_container, "dbus_message_iter_open_container")
    DBSYM(message_iter_close_container, "dbus_message_iter_close_container")
    DBSYM(connection_send_with_reply_and_block, "dbus_connection_send_with_reply_and_block")
    DBSYM(connection_read_write_dispatch, "dbus_connection_read_write_dispatch")
    DBSYM(connection_pop_message, "dbus_connection_pop_message")
    DBSYM(message_is_signal, "dbus_message_is_signal")
    DBSYM(message_get_path,  "dbus_message_get_path")
    DBSYM(message_iter_init, "dbus_message_iter_init")
    DBSYM(message_iter_next, "dbus_message_iter_next")
    DBSYM(message_iter_get_arg_type, "dbus_message_iter_get_arg_type")
    DBSYM(message_iter_get_basic, "dbus_message_iter_get_basic")
    DBSYM(message_iter_recurse, "dbus_message_iter_recurse")
    #undef DBSYM
    return true;
}

/* One {sv} pair into an already-open a{sv} array. */
static void portal_opt(DBusIter* arr, const char* key, int type,
                       const char* sig, const void* val) {
    DBusIter ent, var;
    db.message_iter_open_container(arr, DB_TYPE_DICT_ENTRY, NULL, &ent);
    db.message_iter_append_basic(&ent, DB_TYPE_STRING, &key);
    db.message_iter_open_container(&ent, DB_TYPE_VARIANT, sig, &var);
    db.message_iter_append_basic(&var, type, val);
    db.message_iter_close_container(&ent, &var);
    db.message_iter_close_container(arr, &ent);
}

/* Dig "uri" out of the Response signal's a{sv} results. */
static bool portal_find_uri(DBusIter* it, char* out, size_t sz) {
    if (db.message_iter_get_arg_type(it) != DB_TYPE_ARRAY) return false;
    DBusIter arr;
    db.message_iter_recurse(it, &arr);
    while (db.message_iter_get_arg_type(&arr) == DB_TYPE_DICT_ENTRY) {
        DBusIter ent;
        db.message_iter_recurse(&arr, &ent);
        const char* key = NULL;
        db.message_iter_get_basic(&ent, &key);
        db.message_iter_next(&ent);
        if (key && strcmp(key, "uri") == 0 &&
            db.message_iter_get_arg_type(&ent) == DB_TYPE_VARIANT) {
            DBusIter var;
            db.message_iter_recurse(&ent, &var);
            if (db.message_iter_get_arg_type(&var) == DB_TYPE_STRING) {
                const char* uri = NULL;
                db.message_iter_get_basic(&var, &uri);
                if (uri) { snprintf(out, sz, "%s", uri); return true; }
            }
        }
        db.message_iter_next(&arr);
    }
    return false;
}

/* percent-decode a file:// URI into a plain path */
static void portal_uri_to_path(const char* uri, char* out, size_t sz) {
    const char* p = uri;
    if (!strncmp(p, "file://", 7)) p += 7;
    size_t o = 0;
    while (*p && o + 1 < sz) {
        if (p[0] == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = { p[1], p[2], 0 };
            out[o++] = (char)strtol(hex, NULL, 16);
            p += 3;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
}

bool plat_portal_screenshot(char* out_path, size_t out_sz) {
    if (!portal_load()) return false;

    DBusErr err;
    db.error_init(&err);
    DBusConn* c = db.bus_get_private(DB_BUS_SESSION, &err);
    if (!c) { db.error_free(&err); return false; }
    db.connection_set_exit_on_disconnect(c, 0);

    bool ok = false;
    char token[64], expect[256], match[512];
    char handle_out[256] = {0};      /* the Request path, for Close() below */
    /* The token only has to be unique within this connection. */
    snprintf(token, sizeof token, "orb%u", (unsigned)(plat_now_ms() & 0xFFFFFFu));

    const char* uniq = db.bus_get_unique_name(c);
    if (uniq) {
        /* Sender name -> path fragment: drop the ':' and turn '.' into '_'. */
        char sender[128];
        snprintf(sender, sizeof sender, "%s", uniq[0] == ':' ? uniq + 1 : uniq);
        for (char* q = sender; *q; q++) if (*q == '.') *q = '_';
        snprintf(expect, sizeof expect,
                 "/org/freedesktop/portal/desktop/request/%s/%s", sender, token);

        /* Listen BEFORE asking: a quick compositor can answer before the
         * match rule would otherwise be in place.
         *
         * Deliberately NOT filtered on the path. The portal is only asked to
         * use our handle_token, and is entitled to hand back a different
         * Request path -- so the path is checked when a signal arrives,
         * against both the one it returned and the one we predicted, rather
         * than being baked into a rule that would silently match nothing. */
        snprintf(match, sizeof match,
                 "type='signal',interface='org.freedesktop.portal.Request',"
                 "member='Response'");
        db.bus_add_match(c, match, &err);

        DBusMsg* m = db.message_new_method_call(
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Screenshot",
            "Screenshot");
        if (m) {
            DBusIter args, opts;
            db.message_iter_init_append(m, &args);
            const char* parent = "";
            db.message_iter_append_basic(&args, DB_TYPE_STRING, &parent);
            db.message_iter_open_container(&args, DB_TYPE_ARRAY, "{sv}", &opts);
            const char* tok = token;
            portal_opt(&opts, "handle_token", DB_TYPE_STRING, "s", &tok);
            /* interactive=true: the compositor shows its own picker.
             *
             * The first version asked for interactive=false -- the whole
             * screen, no UI, crop afterwards in our editor. GNOME refuses
             * that, and is right to: a silent full-screen grab by a program
             * the desktop cannot identify is the exact thing the portal
             * exists to prevent. Ten attempts, ten refusals, and the journal
             * saying so each time.
             *
             * Asking for the picker is not a workaround, it is the model. On
             * Wayland the compositor owns the screen, so it owns the act of
             * choosing what to photograph. It also hands back a REGION rather
             * than a whole screen, which is what F4 wanted in the first
             * place -- so on Wayland the compositor's selector stands in for
             * ours, and the result lands in our editor exactly as before. */
            int yes = 1;
            portal_opt(&opts, "interactive", DB_TYPE_BOOLEAN, "b", &yes);
            db.message_iter_close_container(&args, &opts);

            char* handle = handle_out;
            DBusMsg* reply = db.connection_send_with_reply_and_block(c, m, 10000, &err);
            db.message_unref(m);
            if (reply) {
                /* The authoritative Request path, straight from the portal. */
                DBusIter r;
                if (db.message_iter_init(reply, &r) &&
                    db.message_iter_get_arg_type(&r) == DB_TYPE_OBJECT_PATH) {
                    const char* h = NULL;
                    db.message_iter_get_basic(&r, &h);
                    if (h) snprintf(handle_out, sizeof handle_out, "%s", h);
                }
                db.message_unref(reply);
            }

            if (!db.error_is_set(&err)) {
                /* Wait for the Response, effectively for as long as the
                 * user takes.
                 *
                 * This was two minutes, which was a deadline invented here
                 * for a UI that has none: the portal's picker sits waiting
                 * for a human, and a human reading something else is not an
                 * error. Worse, giving up orphans that picker -- closing the
                 * request does not take it off the screen -- and interacting
                 * with an orphaned picker segfaulted xdg-desktop-portal-gnome
                 * seven minutes after we walked away.
                 *
                 * So do not walk away. The picker's own Escape produces a
                 * proper cancelled Response and ends this cleanly; the ten
                 * minutes below is only a backstop against a portal that has
                 * died without answering. */
                uint64_t deadline = plat_now_ms() + 600000;
                while (plat_now_ms() < deadline && !ok) {
                    if (!db.connection_read_write_dispatch(c, 200)) break;
                    DBusMsg* sig;
                    while ((sig = db.connection_pop_message(c)) != NULL) {
                        if (db.message_is_signal(sig, "org.freedesktop.portal.Request",
                                                 "Response")) {
                            const char* path = db.message_get_path(sig);
                            if (path && (strcmp(path, expect) == 0 ||
                                         (handle[0] && strcmp(path, handle) == 0))) {
                                DBusIter it;
                                if (db.message_iter_init(sig, &it) &&
                                    db.message_iter_get_arg_type(&it) == DB_TYPE_UINT32) {
                                    unsigned code = 0;
                                    db.message_iter_get_basic(&it, &code);
                                    db.message_iter_next(&it);
                                    char uri[1024];
                                    /* code: 0 granted, 1 cancelled, 2 failed */
                                    if (code == 0 && portal_find_uri(&it, uri, sizeof uri)) {
                                        portal_uri_to_path(uri, out_path, out_sz);
                                        ok = true;
                                    } else {
                                        fprintf(stderr, "[orb] portal screenshot "
                                                "returned %u\n", code);
                                        deadline = 0;   /* refused: stop waiting */
                                    }
                                }
                            }
                        }
                        db.message_unref(sig);
                    }
                }
            }
        }
    }
    /* Abandoning a request without closing it leaves the compositor holding a
     * picker for a caller that has gone. xdg-desktop-portal-gnome was seen to
     * segfault on exactly that, so say goodbye properly: Request.Close() is in
     * the spec for this, and giving up quietly is not the same as giving up
     * politely. */
    if (!ok && handle_out[0] && db.message_new_method_call) {
        DBusMsg* cl = db.message_new_method_call(
            "org.freedesktop.portal.Desktop", handle_out,
            "org.freedesktop.portal.Request", "Close");
        if (cl) {
            DBusErr e2;
            db.error_init(&e2);
            DBusMsg* r2 = db.connection_send_with_reply_and_block(c, cl, 2000, &e2);
            if (r2) db.message_unref(r2);
            if (db.error_is_set(&e2)) db.error_free(&e2);
            db.message_unref(cl);
        }
    }

    if (db.error_is_set(&err)) db.error_free(&err);
    db.connection_close(c);
    db.connection_unref(c);
    return ok;
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

/* ---- clipboard --------------------------------------------------------
 *
 * X11 has no clipboard. It has a protocol in which the program that copied
 * KEEPS the data and serves it to whoever pastes, on demand, over the wire.
 * Nothing is stored in between. A copy is a promise, not an event.
 *
 * What was here shelled out to xclip or wl-copy and, finding neither,
 * printed a line to stderr that nobody sees; plat_clipboard_copy_image was an
 * empty function. So on Linux this program's headline -- take a shot, alt-tab,
 * paste -- quietly did nothing, and a stock Ubuntu has none of those tools
 * installed to begin with.
 *
 * Owning the selection ourselves removes the dependency and makes the promise
 * real. The cost is that it must be KEPT: plat_clipboard_serve() answers
 * SelectionRequest events every frame and the bytes live as long as the orb
 * does. That is ordinary for X, and on a desktop with a clipboard manager --
 * GNOME has one -- the content is taken over when we exit, so it outlives us.
 */

static Window   g_clip_win = 0;
static uint8_t* g_clip_png = NULL;      /* the image, as PNG bytes */
static size_t   g_clip_png_len = 0;
static char     g_clip_path[700];       /* the file, for a file-manager paste */

static Atom A_CLIPBOARD, A_TARGETS, A_PNG, A_URILIST, A_UTF8, A_STRING, A_TEXT;

static bool path_looks_png(const char* p) {
    size_t n = strlen(p);
    return n > 4 && strcasecmp(p + n - 4, ".png") == 0;
}

static void clip_init(void) {
    if (g_clip_win) return;
    Display* d = dpy();
    if (!d) return;
    A_CLIPBOARD = XInternAtom(d, "CLIPBOARD", False);
    A_TARGETS   = XInternAtom(d, "TARGETS", False);
    A_PNG       = XInternAtom(d, "image/png", False);
    A_URILIST   = XInternAtom(d, "text/uri-list", False);
    A_UTF8      = XInternAtom(d, "UTF8_STRING", False);
    A_STRING    = XInternAtom(d, "STRING", False);
    A_TEXT      = XInternAtom(d, "TEXT", False);
    /* An unmapped 1x1 window that exists only to be a selection owner. */
    g_clip_win = XCreateSimpleWindow(d, DefaultRootWindow(d), -10, -10, 1, 1,
                                     0, 0, 0);
    XSelectInput(d, g_clip_win, PropertyChangeMask);
}

static void clip_own(void) {
    clip_init();
    if (!g_clip_win) return;
    XSetSelectionOwner(dpy(), A_CLIPBOARD, g_clip_win, CurrentTime);
    XFlush(dpy());
}

/* Read the file into memory. Holding the path alone would not be enough: the
 * clipboard must be able to produce the bytes at any later moment, and by
 * then the file may have been moved, edited or deleted. */
static void clip_load_png(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0 && n < 64 * 1024 * 1024) {
        uint8_t* buf = (uint8_t*)malloc((size_t)n);
        if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
            free(g_clip_png);
            g_clip_png = buf;
            g_clip_png_len = (size_t)n;
        } else {
            free(buf);
        }
    }
    fclose(f);
}

void plat_clipboard_copy_file(const char* file_path) {
    snprintf(g_clip_path, sizeof g_clip_path, "%s", file_path);
    if (path_looks_png(file_path)) clip_load_png(file_path);
    clip_own();
}

/* png_write_rgba lives in the core translation unit. Declared rather than
 * included so this file does not end up carrying a second copy of the
 * encoder -- and so what is pasted is byte-identical to what is saved. */
int png_write_rgba(const char* path, const uint8_t* rgba, int w, int h);

void plat_clipboard_copy_image(const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return;
    const char* run = getenv("XDG_RUNTIME_DIR");
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s/orb_clip.png", run && run[0] ? run : "/tmp");
    if (png_write_rgba(tmp, rgba, w, h)) {
        clip_load_png(tmp);
        remove(tmp);
        clip_own();
    }
}

/* Answer one SelectionRequest. */
static void clip_answer(XSelectionRequestEvent* rq) {
    Display* d = dpy();
    XSelectionEvent res;
    memset(&res, 0, sizeof res);
    res.type      = SelectionNotify;
    res.display   = d;
    res.requestor = rq->requestor;
    res.selection = rq->selection;
    res.target    = rq->target;
    res.time      = rq->time;
    res.property  = None;

    Atom prop = rq->property ? rq->property : rq->target;

    if (rq->target == A_TARGETS) {
        Atom offer[6];
        int n = 0;
        offer[n++] = A_TARGETS;
        if (g_clip_png) offer[n++] = A_PNG;
        if (g_clip_path[0]) {
            offer[n++] = A_URILIST;
            offer[n++] = A_UTF8;
            offer[n++] = A_STRING;
            offer[n++] = A_TEXT;
        }
        XChangeProperty(d, rq->requestor, prop, XA_ATOM, 32, PropModeReplace,
                        (unsigned char*)offer, n);
        res.property = prop;
    } else if (rq->target == A_PNG && g_clip_png) {
        XChangeProperty(d, rq->requestor, prop, A_PNG, 8, PropModeReplace,
                        g_clip_png, (int)g_clip_png_len);
        res.property = prop;
    } else if (rq->target == A_URILIST && g_clip_path[0]) {
        char uri[800];
        int n = snprintf(uri, sizeof uri, "file://%s\r\n", g_clip_path);
        XChangeProperty(d, rq->requestor, prop, A_URILIST, 8, PropModeReplace,
                        (unsigned char*)uri, n);
        res.property = prop;
    } else if ((rq->target == A_UTF8 || rq->target == A_STRING ||
                rq->target == A_TEXT) && g_clip_path[0]) {
        XChangeProperty(d, rq->requestor, prop, rq->target, 8, PropModeReplace,
                        (unsigned char*)g_clip_path, (int)strlen(g_clip_path));
        res.property = prop;
    }
    XSendEvent(d, rq->requestor, False, 0, (XEvent*)&res);
    XFlush(d);
}

/* Take only selection traffic addressed to our owner window. Everything else
 * in the queue belongs to GLFW, and helping ourselves to it is exactly how
 * the hotkey poll used to swallow every keystroke in the program. */
static Bool clip_event(Display* d, XEvent* ev, XPointer arg) {
    (void)d; (void)arg;
    if (ev->type == SelectionRequest)
        return ev->xselectionrequest.owner == g_clip_win ? True : False;
    if (ev->type == SelectionClear)
        return ev->xselectionclear.window == g_clip_win ? True : False;
    return False;
}

void plat_clipboard_serve(void) {
    if (!g_clip_win) return;
    XEvent ev;
    while (XCheckIfEvent(dpy(), &ev, clip_event, NULL)) {
        if (ev.type == SelectionRequest) {
            clip_answer(&ev.xselectionrequest);
        } else if (ev.type == SelectionClear) {
            /* Somebody else copied something. Let ours go: continuing to
             * answer would be claiming a clipboard we no longer own. */
            free(g_clip_png);
            g_clip_png = NULL;
            g_clip_png_len = 0;
            g_clip_path[0] = 0;
        }
    }
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
    /* CLOEXEC, or every child inherits the lock. This program spawns
     * xdg-open, a file manager and an image editor, and without it a stray
     * child outliving the orb would keep the name bound -- leaving the next
     * launch to report "already running" with nothing running. */
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
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
        return "Wayland session: screenshots go through the desktop portal "
               "(it will show its own picker). GIF and MP4 recording read the "
               "X root window, which is empty here -- use an Xorg session to "
               "record.";

    /* Secondary hint, for compositors that do not advertise the extension. */
    const char* t = getenv("XDG_SESSION_TYPE");
    const char* w = getenv("WAYLAND_DISPLAY");
    if ((t && strcasecmp(t, "wayland") == 0) || (w && w[0]))
        return "Looks like a Wayland session: screenshots go through the "
               "desktop portal. Recording needs an Xorg session.";
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
