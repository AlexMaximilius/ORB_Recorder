/*
 * platform.h -- OS-abstraction interface for GIF_Recorder Orb.
 *
 * Two implementations, one per build target:
 *   platform_win32.c   Windows (GDI + WinAPI)
 *   platform_x11.c     Linux / X11 (XShmGetImage + XGrabKey + XDG paths)
 *
 * The core (gif_orb.c) is GLFW + OpenGL + libc only; it calls plat_* for
 * anything the standard doesn't cover: screen capture, global hotkeys,
 * right-click menus, native folder paths, file-manager reveal, threading.
 */

#ifndef GIFORB_PLATFORM_H
#define GIFORB_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct GLFWwindow;

/* ---- time ------------------------------------------------------------- */
uint64_t plat_now_ms(void);
void     plat_sleep_ms(int ms);

/* ---- paths (caller-provided buffers, no allocation) ------------------- */
void plat_get_log_path   (char* out, size_t sz);   /* full path incl. filename */
void plat_get_output_dir (char* out, size_t sz);   /* Pictures/GIF directory  */
void plat_get_config_path(char* out, size_t sz);   /* settings.ini full path  */

/* ---- native window: called AFTER glfwCreateWindow --------------------- */
void plat_window_setup           (struct GLFWwindow* w);
void plat_window_move            (struct GLFWwindow* w, int x, int y);
/* Move AND resize in one operation.
 *
 * Resizing and moving as two calls leaves a window that is briefly the new
 * SIZE at the old POSITION. Windows resizes about the top-left corner, and
 * the orb is centred in a window three times its width, so during that gap
 * the orb renders half the size change down and to the right -- a visible
 * diagonal flick on every scroll notch, measured at 9-18 px. One call, one
 * atomic change, no intermediate state to catch. */
void plat_window_set_rect        (struct GLFWwindow* w, int x, int y,
                                  int width, int height);
void plat_window_topmost_refresh (struct GLFWwindow* w);
void plat_window_load_icon       (struct GLFWwindow* w);
/* When running elevated, UIPI blocks drag-drop FROM normal-integrity apps
 * (e.g. a non-elevated Explorer). Call this on any window that accepts
 * drops to punch the needed messages through the filter. No-op elsewhere. */
void plat_window_allow_drops     (struct GLFWwindow* w);

/* OpenGL always renders into a rectangle, and a top-level window is always
 * a rectangle -- so a round orb still sits in a square that swallows clicks
 * in its transparent corners. These tell the OS otherwise.
 *
 * plat_window_set_circular(w, d) clips the window to a circle of diameter d
 * (0 restores the rectangle). Both hit-testing and painting follow the shape.
 * plat_window_set_clickthrough(w, on) makes the window ignore the mouse
 * entirely -- used while the ping window is temporarily enlarged, so the
 * empty space around the rings never steals a click. */
void plat_window_set_circular    (struct GLFWwindow* w, int diameter);
/* Region = a circle at (cx,cy) of the given diameter, optionally unioned
 * with a rectangle. The orb lives centred in a larger window so the ping
 * has room without ever resizing anything -- and the toast is drawn OUTSIDE
 * the orb circle, so it needs to be part of the region or the OS clips it
 * away. Pass rw <= 0 for circle only. */
/* NOTE: (bx, by) is the TOP-LEFT of the circle's bounding box, not its
 * centre -- it maps straight onto CreateEllipticRgn(bx, by, bx+d, by+d).
 * Named cx/cy originally, which read as "centre" and cost a regression on
 * 2026-08-14 when the window stopped being a fixed multiple of the orb. */
void plat_window_set_shape (struct GLFWwindow* w,
                            int bx, int by, int diameter,
                            int rx, int ry, int rw, int rh);
void plat_window_set_clickthrough(struct GLFWwindow* w, bool on);

/* ---- 2D fallback -------------------------------------------------------
 * When the display driver is gone -- Joe lost a 4090 to a code 43 on
 * 2026-08-14 and ORB_Recorder went with it -- there is no GL context to
 * draw into, but there is nothing wrong with the machine's ability to
 * RECORD: capture is GDI and Media Foundation, neither of which needs
 * OpenGL. Losing the picture of the orb should not lose the tool.
 *
 * So the orb can be painted with plain 2D primitives instead. The window
 * region already clips it to a circle, so this only has to fill shapes --
 * no alpha compositing, no shaders, no context.
 *
 * core_rgb is the state colour the 3D core would have had. */
void plat_draw_orb_2d(struct GLFWwindow* w, int win_size, int orb_size,
                      unsigned char cr, unsigned char cg, unsigned char cb);

/* ---- system tray -------------------------------------------------------
 * "Tray only" hides the taskbar button and puts an icon in the notification
 * area instead. Left-click the tray icon to locate the orb (it pings),
 * right-click for the same menu the orb has. */
void plat_tray_set(struct GLFWwindow* w, bool on, const char* tooltip);
#define PLAT_TRAY_NONE   0
#define PLAT_TRAY_LOCATE 1   /* left click  */
#define PLAT_TRAY_MENU   2   /* right click */
int  plat_poll_tray(void);

/* ---- pointing & focus ------------------------------------------------- */
void  plat_get_cursor(int* x, int* y);
bool  plat_left_button_down(void);
/* Returns an opaque native handle (HWND on Win32, Window on X11), or NULL. */
void* plat_window_at_cursor(void);
void* plat_get_foreground_window(void);
void* plat_get_orb_native_handle(struct GLFWwindow* w);
bool  plat_handles_equal(void* a, void* b);

/* ---- capture ---------------------------------------------------------- */
/* Both return a malloc'd RGBA buffer of exactly capW*capH*4 bytes,
 * or NULL on failure. Caller must free().                             */
uint8_t* plat_capture_window (void* native_handle,
                              int capW, int capH,
                              char* title_out, size_t title_size,
                              int* src_w, int* src_h);

/* `name` is a human label for menus. `id` is a STABLE identity for the
 * physical display (\\.\DISPLAY2 on Windows, an XRandR output name like
 * HDMI-1 on X11) -- used to remember which screen the user parked the
 * orb on, so the choice survives resolution changes and renumbering. */
typedef struct { int x, y, w, h; char name[64]; char id[64]; } PlatMonitor;
int      plat_enum_monitors (PlatMonitor* out, int max);
uint8_t* plat_capture_rect  (int x, int y, int w, int h,
                             int capW, int capH);

/* ---- global hotkeys --------------------------------------------------- */
#define PLAT_HK_F5     1
#define PLAT_HK_ESCAPE 2
#define PLAT_HK_F6     3    /* region capture */
#define PLAT_HK_F7     4    /* video capture  */
#define PLAT_HK_F8     5    /* video, region  */
#define PLAT_HK_F9     6    /* camera         */
#define PLAT_HK_F4     7    /* screenshot     */
#define PLAT_HK_PRTSC  8    /* screenshot, PrintScreen */
bool plat_register_hotkey  (int id);
void plat_unregister_hotkey(int id);
int  plat_poll_hotkey(void);   /* returns PLAT_HK_* or 0 */

/* ---- right-click menu (returns picked ID or 0) ------------------------ */
enum {
    PLAT_MENU_NONE            = 0,
    PLAT_MENU_TOGGLE_F5       = 100,
    PLAT_MENU_TOGGLE_AUTOOPEN = 101,
    PLAT_MENU_QUIT            = 102,
    PLAT_MENU_TOGGLE_CLIP     = 103,
    PLAT_MENU_RESTART_ADMIN   = 104,
    PLAT_MENU_RECORD_REGION   = 105,
    PLAT_MENU_TOGGLE_TRAY     = 106,
    PLAT_MENU_OPEN_EDITOR     = 107,
    PLAT_MENU_HELP            = 108,
    PLAT_MENU_EDIT_SETTINGS   = 109,
    PLAT_MENU_RECORD_VIDEO    = 110,
    PLAT_MENU_RECORD_VIDEO_RGN = 111,
    PLAT_MENU_AUDIO_BASE      = 120,   /* +0 none +1 system +2 mic +3 both */
    PLAT_MENU_HOTKEY_BASE     = 130,   /* +0..5 = F5,F6,F7,F8,F9,F4 toggle */
    PLAT_MENU_RECORD_GIF      = 140,   /* GIF, focused window (same as F5) */
    PLAT_MENU_DBL_GIF         = 141,   /* double-click arms for GIF        */
    PLAT_MENU_DBL_MP4         = 142,   /* double-click arms for MP4        */
    PLAT_MENU_SHOT_REGION     = 143,   /* PNG of a dragged rectangle       */
    PLAT_MENU_SHOT_WINDOW     = 144,   /* PNG of the focused window        */
    PLAT_MENU_SHOT_DELAY_5    = 148,   /* the monitor under the cursor, after 5s */
    PLAT_MENU_RUN_AT_STARTUP  = 145,   /* launch when the user logs in     */
    PLAT_MENU_HIDE_ORB        = 146,   /* hide the orb; tray is the way back */
    PLAT_MENU_SHOT_ED_BASE    = 150,   /* +0 nothing +1 built-in +2 system */
    PLAT_MENU_MONITOR_BASE    = 200,   /* GIF:   + monitor index */
    PLAT_MENU_VMONITOR_BASE   = 300,   /* VIDEO: + monitor index */
    PLAT_MENU_CAMERA_BASE     = 400    /* CAMERA: + device index */
};
/* Everything the menu needs to draw itself. Passed as a struct because the
 * argument list had grown past the point of being readable, and because
 * hotkey capture is now per-key rather than F5-only. */
#define PLAT_HK_COUNT 7        /* F5, F6, F7, F8, F9, F4, PrintScreen */
typedef struct {
    bool hotkey_on[PLAT_HK_COUNT];   /* 0=F5 1=F6 2=F7 3=F8 4=F9 5=F4 6=PrtSc */
    bool auto_open;
    bool auto_clip;
    int  shot_editor;    /* 0 = nothing, 1 = the built-in one, 2 = the OS one */
    bool tray_only;
    int  audio_src;
    bool dbl_video;      /* double-click arms MP4 rather than GIF */
    bool run_at_startup; /* reflects the OS, not a stored preference */
} PlatMenuState;

int plat_show_menu(struct GLFWwindow* w, const PlatMenuState* st,
                   const PlatMonitor* monitors, int nmonitors);

/* ---- image loading helpers -------------------------------------------
 * The portable path is stb_image (JPG/PNG/BMP/TGA/PSD). These two exist to
 * borrow what the OS already knows:
 *
 * plat_shell_thumbnail() asks the shell for a file's thumbnail. On Windows
 * that is IShellItemImageFactory, which (a) inherits every codec with a
 * registered handler -- WebP, HEIC, RAW, PSD, even video posters -- and
 * (b) returns Explorer's ALREADY-CACHED bitmap, so pulling 40 of them for
 * a filmstrip is near instant where decoding 40 JPEGs is not.
 * Returns a malloc'd RGBA buffer, or NULL if unsupported. */
uint8_t* plat_shell_thumbnail(const char* path, int want_px, int* out_w, int* out_h);

/* List image files sitting alongside `path` in the same directory, sorted.
 * Fills `out` with up to `max` heap-allocated paths (caller frees each),
 * and reports the index of `path` itself via *out_self. Returns the count. */
int plat_list_sibling_images(const char* path, char** out, int max, int* out_self);

/* Every image extension this machine can actually decode, as a lowercase
 * comma-separated list (".bmp,.gif,.jpg,..."). Built from the OS codec
 * registry rather than hardcoded, so HEIC, AVIF and camera RAW come along
 * for free wherever the user has those decoders. Returns the count. */
int plat_list_image_extensions(char* out, size_t sz);

/* ---- reveal a file in the OS file explorer ---------------------------- */
void plat_open_folder_select(const char* file_path);

/* Hand a saved image to the system's image EDITOR, which is not the same
 * thing as its viewer.
 *
 * Double-clicking a .png on Windows gets you Photos: no pen, no crop, no
 * arrow, nothing you actually want thirty seconds after taking a
 * screenshot. The shell has kept a separate "edit" verb for precisely this
 * distinction since Windows 95, and on a stock box it resolves to Paint --
 * or to whatever the user has since chosen for editing images, which is the
 * better answer and comes free.
 *
 * Returns false if nothing could be launched. */
bool plat_open_in_editor(const char* path);

/* Why screen capture cannot work here, or NULL when it can.
 *
 * There is exactly one environment where everything else in this program
 * runs correctly and capture silently returns black: an X11 client under a
 * Wayland compositor. The window appears, hotkeys fire, the region selector
 * works, the PNG is written and the editor opens on it -- and the image is
 * empty, because the X root window no longer holds the desktop. Saying
 * nothing is the worst option; a black screenshot that claims to have saved
 * is a bug report from a confused user at best.
 *
 * Returns a short sentence for the log and a popup, or NULL if capture is
 * expected to work. */
const char* plat_capture_unavailable(void);

/* Rasterise a string with the SYSTEM font, into an 8-bit coverage mask.
 *
 * The built-in 5x7 glyphs are uppercase-only and read like a pocket
 * calculator. That is the right look for a toolbar and the wrong one for a
 * label somebody is about to put on a screenshot and send to a colleague,
 * which wants mixed case, real spacing and hinting -- all of which the OS
 * already has and none of which is worth reimplementing.
 *
 * Returns a malloc'd mask of *out_w * *out_h bytes (0 = nothing, 255 =
 * solid) for the caller to free, or NULL where there is no font engine, in
 * which case the caller falls back to the built-in glyphs. */
uint8_t* plat_render_text(const char* s, int px_height, int* out_w, int* out_h);

/* ---- copy a file path onto the clipboard as a file drop (CF_HDROP) ----
 * After this, Ctrl-V in Slack / Discord / GitHub / Explorer attaches the file. */
void plat_clipboard_copy_file(const char* file_path);

/* Put the IMAGE itself on the clipboard (CF_DIB), so it can be pasted
 * straight into a chat box or document rather than as a file attachment.
 * Both are useful and they are different clipboard formats. */
void plat_clipboard_copy_image(const uint8_t* rgba, int w, int h);

/* Launch at login. Deliberately reads back from the OS rather than from
 * settings.ini: the user can remove the entry outside this program, and a
 * checkbox that reports a stored intention rather than the actual state is
 * a checkbox that lies. Returns true on success. */
/* Only one copy should run. A second instance cannot register the global
 * hotkeys -- the first already owns them -- so it comes up with every
 * capture key dead and a menu full of unchecked boxes, which looks like a
 * broken install rather than a duplicate. Returns false if another
 * instance already holds the lock. */
bool plat_single_instance(void);

/* Show or hide the orb window itself. Hiding is not minimising: there is no
 * taskbar button to restore from, so the tray icon is the way back. */
void plat_window_set_visible(struct GLFWwindow* w, bool visible);

/* Re-assert whether the window has a taskbar button.
 *
 * Needed as a repeated call, not a one-off: showing a popup menu requires
 * SetForegroundWindow, the shell re-evaluates the window asynchronously
 * after that, and it hands the taskbar button back some time AFTER the
 * menu call returns -- so re-applying immediately is too early. Cheap to
 * call often; it returns at once when the style is already correct. */
/* Returns true if it actually had to correct the style, so the caller can
 * log it -- evidence beats guessing about when the shell interferes. */
bool plat_taskbar_button_enforce(struct GLFWwindow* w, bool hidden);

bool plat_get_run_at_startup(void);
bool plat_set_run_at_startup(bool on);

/* ---- open-file dialog -------------------------------------------------
 * Fills `out` with a chosen path and returns true, or returns false if the
 * user cancelled. Used only where a dialog is actually wanted -- never in
 * the recording path, which stays keypress-only. */
bool plat_open_file_dialog(char* out, size_t out_sz);

/* Hand a file to the OS default handler (opens settings.ini in whatever
 * edits .ini files). Unlike plat_open_folder_select, this opens the FILE. */
void plat_open_with_default_app(const char* path);

/* ---- privilege / integrity level --------------------------------------
 * Windows UIPI blocks a medium-integrity process from PrintWindow-ing a
 * high-integrity (elevated) window. Reading the composited desktop is NOT
 * blocked, so we can still capture such a window by screen rectangle --
 * we just need to know when to take that path, and offer the user the
 * option to relaunch elevated for proper per-window capture. */
bool plat_process_is_elevated(void);            /* are WE running as admin? */
bool plat_window_is_elevated(void* native_handle); /* is the TARGET admin? */
/* Relaunch this executable via UAC. Returns true if the elevated instance
 * was launched (caller should then exit); false if the user declined. */
bool plat_restart_elevated(void);

/* ---- activation ping --------------------------------------------------
 * Returns true once per "the user clicked our taskbar button / brought the
 * app forward" event. Used to fire a locate-me ring around the orb.
 * Must be called after plat_window_setup(). */
bool plat_poll_activation(void);

/* ---- video recording (H.264 + AAC in MP4) -----------------------------
 * Windows uses Media Foundation for the encoder and WASAPI loopback for
 * system audio -- both OS components, so no download and no extra binary.
 * Audio is best-effort: if loopback cannot start, recording continues
 * silently rather than failing. plat_video_has_audio() reports which
 * happened so the UI can say so honestly.
 * Returns false from plat_video_start() when video recording is not
 * available on this platform at all. */
/* Which sound to record. Loopback = whatever is coming out of the speakers;
 * mic = the default capture device; both are summed. */
#define PLAT_AUDIO_NONE   0
#define PLAT_AUDIO_SYSTEM 1
#define PLAT_AUDIO_MIC    2
#define PLAT_AUDIO_BOTH   3
bool plat_video_start     (const char* path, int w, int h, int fps, int audio_src);
bool plat_video_write_frame(const uint8_t* rgba, uint64_t t_ms);
bool plat_video_has_audio (void);
void plat_video_stop      (void);

/* ---- camera ------------------------------------------------------------
 * Recorded DIRECTLY to its own file -- not composited over a screen
 * capture. That keeps it to a source reader feeding the encoder above,
 * rather than a second live pipeline with per-frame blending and
 * position/size UI. Returns 0 cameras where unsupported. */
#define PLAT_MAX_CAMERAS 8
int      plat_camera_list (char names[][64], int max);

/* Is this camera held EXCLUSIVELY by something else?
 *
 * Almost always false on Windows 10+, and that is not a bug. Media
 * Foundation shares cameras: two processes can stream the same device at
 * once. Verified directly -- one process held a camera open and reading
 * frames while another opened it successfully and the probe still reported
 * free. So there is no "busy" state to display, which is why the camera menu
 * shows no in-use marker.
 *
 * Kept because a driver CAN refuse to share, and when that happens the
 * distinction between "someone else has it" and "it would not start" is
 * worth reporting accurately. Costs an activate/release, so never call it
 * per frame. */
bool     plat_camera_in_use(int index);

/* Find an open popup menu on screen, if there is one.
 *
 * Delayed capture exists to photograph menus, and photographing a whole
 * 3440x1440 monitor to show a menu 300 px wide is not what anyone wanted.
 * Windows gives popup menus their own window class (#32768), so the menu
 * can be found and captured exactly. Returns false when no menu is open,
 * and the caller falls back to the monitor. */
bool     plat_find_open_menu(int* x, int* y, int* w, int* h);

/* Why the last plat_camera_open() failed, in words the user can act on.
 *
 * Media Foundation's HRESULTs are specific and the difference matters --
 * 'someone else has it' and 'the USB controller is out of bandwidth' need
 * completely different responses from the person reading the message, and
 * reporting both as CAM BUSY sent Joe looking for an application that did
 * not exist. Writes an empty string if the last open succeeded. */
void     plat_camera_last_error(char* out, size_t sz);
bool     plat_camera_open (int index, int* out_w, int* out_h);
uint8_t* plat_camera_read (int capW, int capH);   /* malloc'd RGBA, or NULL */
void     plat_camera_close(void);

/* ---- background jobs -------------------------------------------------- */
typedef void (*PlatJobFn)(void* arg);
void plat_run_background(PlatJobFn fn, void* arg);

/* Keep the main loop's work going while the platform is inside a MODAL
 * loop of its own.
 *
 * Showing a popup menu means TrackPopupMenu, which does not return until
 * the menu closes -- and it is called from inside a GLFW callback, so the
 * whole main loop stops with it. Recording stops too, which is why the orb
 * could not record its own right-click menu: the frames simply were not
 * being taken.
 *
 * Register a function here and the platform will call it periodically
 * while it is blocked. On Windows that is a WM_TIMER, which the menu's own
 * modal loop dispatches. Pass NULL to clear. */
typedef void (*PlatModalTickFn)(void);
void plat_set_modal_tick(PlatModalTickFn fn);

#endif /* GIFORB_PLATFORM_H */
