/*
 * platform_win32.c -- Windows implementation of platform.h.
 * Extracted from the original gif_orb.c (v1..v2).
 * Alex Maz -- ORB_Recorder (2026).
 */
/* Win7+ APIs: TokenElevation, TokenIntegrityLevel, ChangeWindowMessageFilterEx */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
/* C-callable COM: gives IFoo_Method(ptr, ...) instead of ptr->lpVtbl->Method.
 * Needed for the WIC enumeration below. */
#define COBJMACROS

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>       /* CoInitializeEx / CoUninitialize */
#include <shobjidl.h>      /* IShellItemImageFactory */
#include <shlwapi.h>
#include <commdlg.h>
#include <wincodec.h>
#include <wchar.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "platform.h"

#define IDI_ORB         101
#define HOTKEY_ID_F5    0xB055
#define HOTKEY_ID_ESC   0xB056
#define HOTKEY_ID_F6    0xB057
#define HOTKEY_ID_F7    0xB058
#define HOTKEY_ID_F8    0xB059
#define HOTKEY_ID_F9    0xB05A
#define HOTKEY_ID_F4    0xB05B
#define HOTKEY_ID_PRTSC 0xB05C

/* ---- time ------------------------------------------------------------- */
uint64_t plat_now_ms(void) { return (uint64_t)GetTickCount64(); }
void     plat_sleep_ms(int ms) { Sleep((DWORD)ms); }

/* ---- paths ------------------------------------------------------------ */
void plat_get_log_path(char* out, size_t sz) {
    char base[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) {
        snprintf(out, sz, "%s\\ORB_Recorder", base);
        CreateDirectoryA(out, NULL);
        strncat(out, "\\log.txt", sz - strlen(out) - 1);
    } else {
        snprintf(out, sz, ".\\orb_recorder_log.txt");
    }
}

void plat_get_output_dir(char* out, size_t sz) {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_MYPICTURES, NULL, 0, path))) {
        snprintf(out, sz, "%s\\ORB_Recorder", path);
    } else {
        snprintf(out, sz, ".\\ORB_Recorder");
    }
    CreateDirectoryA(out, NULL);
}

void plat_get_config_path(char* out, size_t sz) {
    char base[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) {
        snprintf(out, sz, "%s\\ORB_Recorder", base);
        CreateDirectoryA(out, NULL);
        strncat(out, "\\settings.ini", sz - strlen(out) - 1);

        /* Carry settings over from the old name. Renaming the program should
         * not silently reset where the user parked the orb, how big they made
         * it, or which hotkeys they released -- those are hard-won choices and
         * losing them feels like a bug even when the rename was deliberate.
         * Copied, not moved: if they run an older build it still works. */
        if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES) {
            char legacy[MAX_PATH];
            snprintf(legacy, sizeof legacy, "%s\\GIF_Recorder\\settings.ini", base);
            if (GetFileAttributesA(legacy) != INVALID_FILE_ATTRIBUTES)
                CopyFileA(legacy, out, TRUE);
        }
    } else {
        snprintf(out, sz, ".\\orb_recorder_settings.ini");
    }
}

/* ---- privilege / integrity level -------------------------------------- */

bool plat_process_is_elevated(void) {
    BOOL elevated = FALSE;
    HANDLE tok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION el;
        DWORD sz = sizeof el;
        if (GetTokenInformation(tok, TokenElevation, &el, sizeof el, &sz))
            elevated = el.TokenIsElevated;
        CloseHandle(tok);
    }
    return elevated != FALSE;
}

/* Integrity RID of a token: MEDIUM = 0x2000, HIGH = 0x3000, SYSTEM = 0x4000. */
static DWORD token_integrity_rid(HANDLE tok) {
    DWORD size = 0, rid = 0;
    GetTokenInformation(tok, TokenIntegrityLevel, NULL, 0, &size);
    if (!size) return 0;
    TOKEN_MANDATORY_LABEL* lbl = (TOKEN_MANDATORY_LABEL*)malloc(size);
    if (!lbl) return 0;
    if (GetTokenInformation(tok, TokenIntegrityLevel, lbl, size, &size)) {
        UCHAR* cnt = GetSidSubAuthorityCount(lbl->Label.Sid);
        if (cnt && *cnt > 0)
            rid = *GetSidSubAuthority(lbl->Label.Sid, (DWORD)(*cnt - 1));
    }
    free(lbl);
    return rid;
}

/* True when the target window's process runs at a HIGHER integrity level
 * than us -- i.e. UIPI will block PrintWindow against it. */
bool plat_window_is_elevated(void* native_handle) {
    HWND h = (HWND)native_handle;
    if (!h || !IsWindow(h)) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return false;
    if (pid == GetCurrentProcessId()) return false;

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        /* Cannot even query it with LIMITED rights -- treat as higher. */
        return true;
    }
    HANDLE their_tok = NULL;
    bool higher = false;
    if (!OpenProcessToken(proc, TOKEN_QUERY, &their_tok)) {
        /* Access denied opening their token is the classic signature of a
         * higher-integrity process seen from a medium-integrity one. */
        higher = (GetLastError() == ERROR_ACCESS_DENIED);
    } else {
        DWORD theirs = token_integrity_rid(their_tok);
        CloseHandle(their_tok);
        HANDLE mine_tok = NULL;
        DWORD mine = 0;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mine_tok)) {
            mine = token_integrity_rid(mine_tok);
            CloseHandle(mine_tok);
        }
        higher = (theirs > mine);
    }
    CloseHandle(proc);
    return higher;
}

bool plat_restart_elevated(void) {
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH)) return false;
    SHELLEXECUTEINFOA sei;
    memset(&sei, 0, sizeof sei);
    sei.cbSize = sizeof sei;
    sei.fMask  = SEE_MASK_NOASYNC;
    sei.lpVerb = "runas";            /* prompts UAC */
    sei.lpFile = exe;
    sei.nShow  = SW_SHOWNORMAL;
    if (ShellExecuteExA(&sei)) return true;
    return false;                    /* user declined the UAC prompt */
}

/* ---- activation detection (taskbar click) -----------------------------
 * We subclass GLFW's WndProc so we can observe activation messages that
 * GLFW does not surface. WS_EX_NOACTIVATE stops the window taking focus,
 * but the shell still sends restore/tasklist/activateapp traffic when the
 * taskbar button is clicked -- that's what we watch for. */
static WNDPROC       g_orig_wndproc = NULL;
static HWND          g_orb_hwnd     = NULL;

/* WM_HOTKEY has to be latched rather than read straight out of the queue.
 *
 * RegisterHotKey(NULL, ...) posts a THREAD message -- hwnd 0 -- and any
 * message pump that is not ours pulls it out and drops it on the floor, because
 * DispatchMessage has no window to deliver it to. GLFW's pump does that, and so
 * does the modal loop behind a popup menu: press F5 while the orb's own menu is
 * open and the keystroke simply disappears (measured).
 *
 * Registering against the orb's window instead turns it into a window message,
 * which every pump delivers to the window proc below -- so it survives whoever
 * happens to own the thread, and the main loop picks it up when it comes back. */
static volatile LONG g_hotkey_latch = 0;

static int hotkey_id_to_plat(WPARAM id) {
    if (id == HOTKEY_ID_F5)    return PLAT_HK_F5;
    if (id == HOTKEY_ID_ESC)   return PLAT_HK_ESCAPE;
    if (id == HOTKEY_ID_F6)    return PLAT_HK_F6;
    if (id == HOTKEY_ID_F7)    return PLAT_HK_F7;
    if (id == HOTKEY_ID_F8)    return PLAT_HK_F8;
    if (id == HOTKEY_ID_F9)    return PLAT_HK_F9;
    if (id == HOTKEY_ID_F4)    return PLAT_HK_F4;
    if (id == HOTKEY_ID_PRTSC) return PLAT_HK_PRTSC;
    return 0;
}
static volatile LONG g_activation   = 0;

/* Tray */
#define WM_ORB_TRAY (WM_APP + 17)
#define ORB_TRAY_ID 1
static volatile LONG g_tray_event = 0;
static bool          g_tray_on    = false;

/* Clipboard history: the OS tells us when the slot changed, so we never
 * have to poll it. Polling would mean opening the clipboard on a timer,
 * and opening it fights with whatever else is trying to write to it. */
static volatile LONG g_clip_change = 0;

/* Called from WM_TIMER while a modal loop has the thread. See
 * plat_set_modal_tick() in platform.h for why this exists. */
static PlatModalTickFn g_modal_tick = NULL;
#define ORB_MODAL_TIMER_ID 0xB060

void plat_set_modal_tick(PlatModalTickFn fn) { g_modal_tick = fn; }

static LRESULT CALLBACK orb_subclass_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        /* The whole point: a menu's modal loop dispatches this, so whatever
         * the main loop would have been doing keeps happening. */
        if (wp == ORB_MODAL_TIMER_ID && g_modal_tick) {
            g_modal_tick();
            return 0;
        }
        break;
    case WM_HOTKEY: {
        int id = hotkey_id_to_plat(wp);
        if (id) { InterlockedExchange(&g_hotkey_latch, id); return 0; }
        break;
    }
    case WM_ACTIVATEAPP:
        if (wp) InterlockedExchange(&g_activation, 1);
        break;
    case WM_SYSCOMMAND: {
        UINT sc = (UINT)(wp & 0xFFF0);
        if (sc == SC_RESTORE || sc == SC_TASKLIST)
            InterlockedExchange(&g_activation, 1);
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) InterlockedExchange(&g_activation, 1);
        break;
    case WM_SHOWWINDOW:
        /* Restored from minimised / re-shown -- worth a locate flash. */
        if (wp) InterlockedExchange(&g_activation, 1);
        break;
    case WM_CLIPBOARDUPDATE:
        InterlockedExchange(&g_clip_change, 1);
        break;
    case WM_ORB_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK)
            InterlockedExchange(&g_tray_event, PLAT_TRAY_LOCATE);
        else if (LOWORD(lp) == WM_RBUTTONUP)
            InterlockedExchange(&g_tray_event, PLAT_TRAY_MENU);
        break;
    default: break;
    }
    return CallWindowProc(g_orig_wndproc, h, msg, wp, lp);
}

bool plat_poll_activation(void) {
    return InterlockedExchange(&g_activation, 0) != 0;
}

/* ---- system tray ------------------------------------------------------ */

int plat_poll_tray(void) {
    return (int)InterlockedExchange(&g_tray_event, 0);
}

/* Taskbar button on or off.
 *
 * WS_EX_TOOLWINDOW is what removes it, and Windows only notices the style
 * change across a hide/show cycle -- setting it on a visible window does
 * nothing, which is the classic way this is got wrong.
 *
 * Factored out because it has to be re-applied, not just applied: showing a
 * popup menu requires SetForegroundWindow, and forcing a WS_EX_NOACTIVATE
 * window to the foreground makes the shell re-evaluate it and give the button
 * back. Joe found that -- right-click the orb in tray-only mode and the
 * taskbar entry returned, and stayed until tray-only was toggled off and on,
 * because that toggle was the only thing that ever re-applied the style. */
static bool apply_taskbar_button(HWND h, bool hidden) {
    LONG ex = GetWindowLong(h, GWL_EXSTYLE);

    /* TWO bits decide this, not one.
     *
     * WS_EX_TOOLWINDOW asks for no taskbar button, but WS_EX_APPWINDOW
     * OVERRIDES it and forces one -- and the shell sets APPWINDOW itself when
     * a window is activated, which is precisely what showing a popup menu
     * does. So the earlier version, which looked only at TOOLWINDOW, saw the
     * flag still set, concluded the state was already correct, and returned
     * while a taskbar button sat there in plain sight. That is why this
     * survived two attempts at fixing it.
     *
     * The button is present if APPWINDOW is set, or if TOOLWINDOW is not. */
    bool button_present = (ex & WS_EX_APPWINDOW) || !(ex & WS_EX_TOOLWINDOW);
    if (button_present == !hidden) return false;   /* already right */

    /* Windows only re-reads these across a hide/show cycle. */
    ShowWindow(h, SW_HIDE);
    if (hidden) {
        ex |=  WS_EX_TOOLWINDOW;
        ex &= ~WS_EX_APPWINDOW;             /* the one that was overriding */
    } else {
        ex &= ~WS_EX_TOOLWINDOW;
        /* And ASK for the button, rather than merely stopping asking for it
         * to be hidden. Clearing TOOLWINDOW is not enough on its own: this
         * window is WS_EX_NOACTIVATE, and the shell will not always give an
         * unactivatable window a taskbar entry by default. Turning tray-only
         * off left no button at all until APPWINDOW was set explicitly. */
        ex |=  WS_EX_APPWINDOW;
    }
    SetWindowLong(h, GWL_EXSTYLE, ex);
    ShowWindow(h, SW_SHOWNOACTIVATE);
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return true;                            /* we had to correct it */
}

/* Shell_NotifyIcon has been there since Windows 95. */
bool plat_tray_available(void) { return true; }

void plat_tray_set(struct GLFWwindow* w, bool on, const char* tooltip) {
    HWND h = glfwGetWin32Window(w);
    NOTIFYICONDATAA nid;
    memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd   = h;
    nid.uID    = ORB_TRAY_ID;

    if (!on) {
        if (g_tray_on) { Shell_NotifyIconA(NIM_DELETE, &nid); g_tray_on = false; }
        apply_taskbar_button(h, false);     /* give the button back */
        return;
    }

    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_ORB_TRAY;
    nid.hIcon = (HICON)LoadImageA(GetModuleHandleA(NULL),
                                  MAKEINTRESOURCEA(IDI_ORB),
                                  IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON),
                                  LR_DEFAULTCOLOR);
    if (!nid.hIcon) nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    snprintf(nid.szTip, sizeof nid.szTip, "%s", tooltip ? tooltip : "ORB_Recorder");

    if (!g_tray_on) {
        if (Shell_NotifyIconA(NIM_ADD, &nid)) g_tray_on = true;
    } else {
        Shell_NotifyIconA(NIM_MODIFY, &nid);
    }

    /* The orb itself stays visible and on top; only the taskbar entry goes. */
    apply_taskbar_button(h, true);
}

/* ---- native window ---------------------------------------------------- */
void plat_window_setup(struct GLFWwindow* w) {
    HWND h = glfwGetWin32Window(w);
    /* Layered + topmost + noactivate; NO toolwindow -> real taskbar entry. */
    SetWindowLong(h, GWL_EXSTYLE,
        GetWindowLong(h, GWL_EXSTYLE)
        | WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE);
    SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
    SetWindowPos(h, HWND_TOPMOST, 50, 50, 0, 0,
                 SWP_NOSIZE | SWP_SHOWWINDOW);

    /* Install activation subclass once. */
    if (!g_orig_wndproc) {
        g_orb_hwnd = h;
        g_orig_wndproc = (WNDPROC)SetWindowLongPtr(h, GWLP_WNDPROC,
                                                   (LONG_PTR)orb_subclass_proc);
    }
}

void plat_window_set_rect(struct GLFWwindow* w, int x, int y, int width, int height) {
    HWND h = glfwGetWin32Window(w);
    /* Position and size together: SWP_NOMOVE/SWP_NOSIZE both omitted, so
     * the window manager applies one change and there is no frame in which
     * the window is the new size at the old place. */
    SetWindowPos(h, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void plat_window_move(struct GLFWwindow* w, int x, int y) {
    SetWindowPos(glfwGetWin32Window(w), HWND_TOPMOST, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
}

void plat_window_topmost_refresh(struct GLFWwindow* w) {
    SetWindowPos(glfwGetWin32Window(w), HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

/* Punch WM_DROPFILES / WM_COPYDATA / WM_COPYGLOBALDATA through the UIPI
 * message filter so drag-drop from a normal-integrity Explorer still works
 * when we are running elevated. Resolved dynamically: the API is Win7+ and
 * some toolchain headers omit it. */
void plat_window_allow_drops(struct GLFWwindow* w) {
    if (!plat_process_is_elevated()) return;   /* nothing filtered when medium */
    HWND h = glfwGetWin32Window(w);
    typedef BOOL (WINAPI *PFN_CWMFEx)(HWND, UINT, DWORD, void*);
    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (!u32) return;
    PFN_CWMFEx fn = (PFN_CWMFEx)(void*)GetProcAddress(u32, "ChangeWindowMessageFilterEx");
    if (!fn) return;
    const DWORD MSGFLT_ALLOW_ = 1;
    fn(h, WM_DROPFILES,        MSGFLT_ALLOW_, NULL);
    fn(h, WM_COPYDATA,         MSGFLT_ALLOW_, NULL);
    fn(h, 0x0049 /* WM_COPYGLOBALDATA */, MSGFLT_ALLOW_, NULL);
}

/* SetWindowRgn is the Win32 answer to "my object is round but my window is
 * square": the region defines what the window IS, for painting and for hit
 * testing alike, so clicks in the corners go to whatever is underneath. */
void plat_window_set_circular(struct GLFWwindow* w, int diameter) {
    HWND h = glfwGetWin32Window(w);
    if (diameter <= 0) {
        SetWindowRgn(h, NULL, TRUE);      /* back to a plain rectangle */
        return;
    }
    HRGN rgn = CreateEllipticRgn(0, 0, diameter + 1, diameter + 1);
    if (!rgn) return;
    /* The window takes ownership of the region -- do not delete it here. */
    SetWindowRgn(h, rgn, TRUE);
}

void plat_window_set_shape(struct GLFWwindow* w,
                           int bx, int by, int diameter,
                           int rx, int ry, int rw, int rh,
                           int mx, int my, int md) {
    HWND h = glfwGetWin32Window(w);
    if (diameter <= 0) { SetWindowRgn(h, NULL, TRUE); return; }
    HRGN rgn = CreateEllipticRgn(bx, by, bx + diameter + 1, by + diameter + 1);
    if (!rgn) return;
    if (rw > 0 && rh > 0) {
        HRGN bar = CreateRectRgn(rx, ry, rx + rw, ry + rh);
        if (bar) {
            CombineRgn(rgn, rgn, bar, RGN_OR);
            DeleteObject(bar);
        }
    }
    if (md > 0) {
        HRGN moon = CreateEllipticRgn(mx, my, mx + md + 1, my + md + 1);
        if (moon) {
            CombineRgn(rgn, rgn, moon, RGN_OR);
            DeleteObject(moon);
        }
    }
    SetWindowRgn(h, rgn, TRUE);   /* window owns the region now */
}

void plat_window_set_clickthrough(struct GLFWwindow* w, bool on) {
    HWND h = glfwGetWin32Window(w);
    LONG ex = GetWindowLong(h, GWL_EXSTYLE);
    if (on) ex |=  WS_EX_TRANSPARENT;
    else    ex &= ~WS_EX_TRANSPARENT;
    SetWindowLong(h, GWL_EXSTYLE, ex);
}

void plat_window_load_icon(struct GLFWwindow* w) {
    HWND h = glfwGetWin32Window(w);
    HICON hOrb = (HICON)LoadImageA(GetModuleHandleA(NULL),
                                   MAKEINTRESOURCEA(IDI_ORB),
                                   IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    if (hOrb) {
        SendMessageA(h, WM_SETICON, ICON_BIG,   (LPARAM)hOrb);
        SendMessageA(h, WM_SETICON, ICON_SMALL, (LPARAM)hOrb);
    }
}

/* ---- pointing & focus ------------------------------------------------- */
void plat_get_cursor(int* x, int* y) {
    POINT p; GetCursorPos(&p);
    *x = p.x; *y = p.y;
}
bool plat_left_button_down(void) {
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
}
void* plat_window_at_cursor(void) {
    POINT p; GetCursorPos(&p);
    HWND h = WindowFromPoint(p);
    if (!h) return NULL;
    HWND top = GetAncestor(h, GA_ROOT);
    return top ? (void*)top : (void*)h;
}
void* plat_get_foreground_window(void) {
    return (void*)GetForegroundWindow();
}
void* plat_get_orb_native_handle(struct GLFWwindow* w) {
    return (void*)glfwGetWin32Window(w);
}
bool plat_handles_equal(void* a, void* b) { return a == b; }

/* ---- capture ---------------------------------------------------------- */
/* Off by default until the core says otherwise; see plat_capture_draw_cursor. */
static bool g_cap_cursor = false;
void plat_capture_draw_cursor(bool on) { g_cap_cursor = on; }

/* Composite the live pointer into a capture already sitting in `memDC`.
 *
 * (originX, originY) is where the captured area starts in SCREEN coordinates,
 * which is the only frame the cursor position is reported in. srcW/srcH vs
 * capW/capH gives the scale, because captures are usually downsized.
 *
 * The DIB must still be selected into memDC when this runs -- drawing after
 * SelectObject puts the pointer on a bitmap nobody is going to read. */
static void draw_cursor_into(HDC memDC, int originX, int originY,
                             int srcW, int srcH, int capW, int capH) {
    if (!g_cap_cursor || srcW <= 0 || srcH <= 0) return;

    CURSORINFO ci;
    memset(&ci, 0, sizeof ci);
    ci.cbSize = sizeof ci;
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor)
        return;

    ICONINFO ii;
    memset(&ii, 0, sizeof ii);
    if (!GetIconInfo(ci.hCursor, &ii)) return;

    /* The hotspot is the pixel that actually points at things; the bitmap
     * hangs off it. Subtract it or the arrow lands down-right of the truth. */
    int sx = ci.ptScreenPos.x - (int)ii.xHotspot - originX;
    int sy = ci.ptScreenPos.y - (int)ii.yHotspot - originY;

    /* A cursor is square-ish and sized by the system, not by the icon
     * bitmap -- a mask-only cursor (the I-beam) has a double-height mask. */
    int cw = GetSystemMetrics(SM_CXCURSOR);
    int ch = GetSystemMetrics(SM_CYCURSOR);

    int dx = MulDiv(sx, capW, srcW);
    int dy = MulDiv(sy, capH, srcH);
    int dw = MulDiv(cw, capW, srcW);
    int dh = MulDiv(ch, capH, srcH);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    /* Wholly outside the frame -- skip rather than let GDI clip it. */
    if (dx + dw > 0 && dy + dh > 0 && dx < capW && dy < capH)
        DrawIconEx(memDC, dx, dy, ci.hCursor, dw, dh, 0, NULL, DI_NORMAL);

    if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
}

static uint8_t* capture_dc(HDC srcDC, int srcX, int srcY, int srcW, int srcH,
                           int capW, int capH, int originX, int originY) {
    HDC memDC = CreateCompatibleDC(srcDC);
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = capW;
    bmi.bmiHeader.biHeight = -capH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* pixels = NULL;
    HBITMAP dib = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &pixels, NULL, 0);
    if (!dib) { DeleteDC(memDC); return NULL; }
    HGDIOBJ oldBmp = SelectObject(memDC, dib);
    SetStretchBltMode(memDC, HALFTONE);
    StretchBlt(memDC, 0, 0, capW, capH, srcDC, srcX, srcY, srcW, srcH, SRCCOPY);
    /* While the DIB is still selected. */
    draw_cursor_into(memDC, originX, originY, srcW, srcH, capW, capH);
    SelectObject(memDC, oldBmp);

    int npix = capW * capH;
    uint8_t* out = (uint8_t*)malloc(npix * 4);
    if (!out) { DeleteObject(dib); DeleteDC(memDC); return NULL; }
    uint8_t* src = (uint8_t*)pixels;
    for (int i = 0; i < npix; i++) {
        out[i*4 + 0] = src[i*4 + 2];   /* R <- B */
        out[i*4 + 1] = src[i*4 + 1];   /* G */
        out[i*4 + 2] = src[i*4 + 0];   /* B <- R */
        out[i*4 + 3] = 255;
    }
    DeleteObject(dib);
    DeleteDC(memDC);
    return out;
}

/* PW_RENDERFULLCONTENT (Windows 8.1+): tells PrintWindow to composite the
 * window's DirectComposition / hardware-accelerated surfaces into the DC,
 * instead of returning stale cached bits. Without this flag, capturing
 * Chrome/Firefox/Electron/games via BitBlt returns frozen frames. */
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

/* Detect a completely uniform (all one color) capture -- means the source
 * refused to render, so we should retry via a different path. */
static bool image_is_uniform(const uint8_t* rgba, int npix) {
    if (npix < 2) return true;
    uint32_t first = *(const uint32_t*)rgba;
    for (int i = 1; i < npix; i++) {
        if (*(const uint32_t*)(rgba + i*4) != first) return false;
    }
    return true;
}

uint8_t* plat_capture_window(void* handle, int capW, int capH,
                             char* title_out, size_t title_size,
                             int* src_w, int* src_h) {
    HWND target = (HWND)handle;
    if (!IsWindow(target)) return NULL;
    RECT r; if (!GetClientRect(target, &r)) return NULL;
    int sW = r.right - r.left, sH = r.bottom - r.top;
    if (src_w) *src_w = sW;
    if (src_h) *src_h = sH;
    if (title_out && title_size > 0) {
        title_out[0] = 0;
        GetWindowTextA(target, title_out, (int)title_size - 1);
    }
    if (sW <= 0 || sH <= 0) return NULL;

    /* UIPI: if the target runs at higher integrity than us, PrintWindow is
     * blocked and returns black/stale bits. Reading the composited desktop
     * is NOT blocked, so capture the window's screen rectangle instead.
     * Cached per-HWND -- this is a per-frame hot path. */
    {
        static HWND s_cached_hwnd = NULL;
        static bool s_cached_elev = false;
        if (target != s_cached_hwnd) {
            s_cached_hwnd = target;
            s_cached_elev = plat_window_is_elevated(target);
        }
        if (s_cached_elev) {
            RECT wr;
            if (GetWindowRect(target, &wr)) {
                return plat_capture_rect(wr.left, wr.top,
                                         wr.right - wr.left,
                                         wr.bottom - wr.top, capW, capH);
            }
        }
    }

    /* Strategy: build a DIB at native size, ask the window to PrintWindow
     * itself into it (forcing GPU-composited windows to redraw), then
     * resample to capW x capH.                                          */
    HDC winDC = GetDC(target);
    if (!winDC) return NULL;
    HDC memDC = CreateCompatibleDC(winDC);
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = sW;
    bmi.bmiHeader.biHeight = -sH;   /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* nativePixels = NULL;
    HBITMAP nativeDib = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS,
                                         &nativePixels, NULL, 0);
    if (!nativeDib) {
        DeleteDC(memDC); ReleaseDC(target, winDC); return NULL;
    }
    HGDIOBJ oldNative = SelectObject(memDC, nativeDib);

    BOOL pwOk = PrintWindow(target, memDC, PW_RENDERFULLCONTENT);
    if (!pwOk) {
        /* Older Windows / windows that don't support PrintWindow:
         * fall back to BitBlt from the window's own DC.                */
        BitBlt(memDC, 0, 0, sW, sH, winDC, 0, 0, SRCCOPY);
    }

    /* Now resample nativePixels (BGRA at sW x sH) into out (RGBA at capW x capH).
     * We do a simple StretchBlt via a second memory DC.                */
    HDC scaleDC = CreateCompatibleDC(winDC);
    BITMAPINFO sbmi;
    memset(&sbmi, 0, sizeof sbmi);
    sbmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    sbmi.bmiHeader.biWidth = capW;
    sbmi.bmiHeader.biHeight = -capH;
    sbmi.bmiHeader.biPlanes = 1;
    sbmi.bmiHeader.biBitCount = 32;
    sbmi.bmiHeader.biCompression = BI_RGB;
    void* scalePixels = NULL;
    HBITMAP scaleDib = CreateDIBSection(scaleDC, &sbmi, DIB_RGB_COLORS,
                                        &scalePixels, NULL, 0);
    if (!scaleDib) {
        SelectObject(memDC, oldNative); DeleteObject(nativeDib);
        DeleteDC(memDC); DeleteDC(scaleDC); ReleaseDC(target, winDC);
        return NULL;
    }
    HGDIOBJ oldScale = SelectObject(scaleDC, scaleDib);
    SetStretchBltMode(scaleDC, HALFTONE);
    StretchBlt(scaleDC, 0, 0, capW, capH, memDC, 0, 0, sW, sH, SRCCOPY);
    /* The pointer goes on AFTER the downscale, so it stays crisp instead of
     * being resampled with the rest of the frame. The window's top-left is
     * the origin the cursor's screen position is measured against -- asked
     * for here rather than reused from further up, where it lives inside a
     * block that only the elevated-window path enters. */
    {
        RECT cwr;
        if (GetWindowRect(target, &cwr))
            draw_cursor_into(scaleDC, cwr.left, cwr.top, sW, sH, capW, capH);
    }
    SelectObject(scaleDC, oldScale);

    int npix = capW * capH;
    uint8_t* out = (uint8_t*)malloc(npix * 4);
    if (!out) {
        DeleteObject(scaleDib); SelectObject(memDC, oldNative);
        DeleteObject(nativeDib); DeleteDC(memDC); DeleteDC(scaleDC);
        ReleaseDC(target, winDC); return NULL;
    }
    uint8_t* src = (uint8_t*)scalePixels;
    for (int i = 0; i < npix; i++) {
        out[i*4 + 0] = src[i*4 + 2];   /* R <- B */
        out[i*4 + 1] = src[i*4 + 1];   /* G */
        out[i*4 + 2] = src[i*4 + 0];   /* B <- R */
        out[i*4 + 3] = 255;
    }
    DeleteObject(scaleDib);
    SelectObject(memDC, oldNative);
    DeleteObject(nativeDib);
    DeleteDC(memDC);
    DeleteDC(scaleDC);
    ReleaseDC(target, winDC);

    /* Last-ditch: if PrintWindow returned all-one-color (some fullscreen
     * DX apps ignore PW_RENDERFULLCONTENT), fall back to grabbing the
     * screen rectangle where the window lives via GetDC(NULL).          */
    if (image_is_uniform(out, npix)) {
        RECT wr;
        if (GetWindowRect(target, &wr)) {
            uint8_t* alt = plat_capture_rect(wr.left, wr.top,
                                             wr.right - wr.left,
                                             wr.bottom - wr.top,
                                             capW, capH);
            if (alt) { free(out); return alt; }
        }
    }
    return out;
}

uint8_t* plat_capture_rect(int x, int y, int w, int h, int capW, int capH) {
    HDC screenDC = GetDC(NULL);
    if (!screenDC) return NULL;
    /* A screen capture's origin IS its screen position. */
    uint8_t* px = capture_dc(screenDC, x, y, w, h, capW, capH, x, y);
    ReleaseDC(NULL, screenDC);
    return px;
}

static PlatMonitor* g_enum_buf; static int g_enum_max, g_enum_n;
static BOOL CALLBACK enum_cb(HMONITOR mon, HDC dc, LPRECT rect, LPARAM lp) {
    (void)dc; (void)lp;
    if (g_enum_n >= g_enum_max) return FALSE;
    PlatMonitor* m = &g_enum_buf[g_enum_n];
    m->x = rect->left; m->y = rect->top;
    m->w = rect->right - rect->left;
    m->h = rect->bottom - rect->top;
    snprintf(m->name, sizeof m->name, "Monitor %d  (%dx%d)",
             g_enum_n + 1, m->w, m->h);
    /* Stable identity: \\.\DISPLAY1 etc. Survives resolution changes. */
    m->id[0] = 0;
    MONITORINFOEXA mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoA(mon, (MONITORINFO*)&mi))
        snprintf(m->id, sizeof m->id, "%s", mi.szDevice);
    if (!m->id[0]) snprintf(m->id, sizeof m->id, "idx%d", g_enum_n);
    g_enum_n++;
    return TRUE;
}
int plat_enum_monitors(PlatMonitor* out, int max) {
    g_enum_buf = out; g_enum_max = max; g_enum_n = 0;
    EnumDisplayMonitors(NULL, NULL, enum_cb, 0);
    return g_enum_n;
}

/* ---- hotkeys ---------------------------------------------------------- */
bool plat_register_hotkey(int id) {
    if (id == PLAT_HK_F5)     return RegisterHotKey(g_orb_hwnd, HOTKEY_ID_F5,  MOD_NOREPEAT, VK_F5)     != 0;
    if (id == PLAT_HK_ESCAPE) return RegisterHotKey(g_orb_hwnd, HOTKEY_ID_ESC, 0,            VK_ESCAPE) != 0;
    if (id == PLAT_HK_F6)     return RegisterHotKey(g_orb_hwnd, HOTKEY_ID_F6,  MOD_NOREPEAT, VK_F6)     != 0;
    if (id == PLAT_HK_F7)     return RegisterHotKey(g_orb_hwnd, HOTKEY_ID_F7,  MOD_NOREPEAT, VK_F7)     != 0;
    if (id == PLAT_HK_F8)     return RegisterHotKey(g_orb_hwnd, HOTKEY_ID_F8,  MOD_NOREPEAT, VK_F8)     != 0;
    if (id == PLAT_HK_F9)     return RegisterHotKey(g_orb_hwnd, HOTKEY_ID_F9,  MOD_NOREPEAT, VK_F9)     != 0;
    if (id == PLAT_HK_F4)     return RegisterHotKey(g_orb_hwnd, HOTKEY_ID_F4,  MOD_NOREPEAT, VK_F4)     != 0;
    /* PrintScreen is bound to Snipping Tool on Windows 11 out of the box,
     * so this can legitimately fail. The caller reports that honestly
     * rather than leaving a checkbox that silently will not tick. */
    if (id == PLAT_HK_PRTSC)  return RegisterHotKey(g_orb_hwnd, HOTKEY_ID_PRTSC, MOD_NOREPEAT, VK_SNAPSHOT) != 0;
    if (id == PLAT_HK_F8)     return RegisterHotKey(g_orb_hwnd, HOTKEY_ID_F8,  MOD_NOREPEAT, VK_F8)     != 0;
    return false;
}
void plat_unregister_hotkey(int id) {
    if (id == PLAT_HK_F5)     UnregisterHotKey(g_orb_hwnd, HOTKEY_ID_F5);
    if (id == PLAT_HK_ESCAPE) UnregisterHotKey(g_orb_hwnd, HOTKEY_ID_ESC);
    if (id == PLAT_HK_F6)     UnregisterHotKey(g_orb_hwnd, HOTKEY_ID_F6);
    if (id == PLAT_HK_F7)     UnregisterHotKey(g_orb_hwnd, HOTKEY_ID_F7);
    if (id == PLAT_HK_F8)     UnregisterHotKey(g_orb_hwnd, HOTKEY_ID_F8);
    if (id == PLAT_HK_F9)     UnregisterHotKey(g_orb_hwnd, HOTKEY_ID_F9);
    if (id == PLAT_HK_F4)     UnregisterHotKey(g_orb_hwnd, HOTKEY_ID_F4);
    if (id == PLAT_HK_PRTSC)  UnregisterHotKey(g_orb_hwnd, HOTKEY_ID_PRTSC);
    if (id == PLAT_HK_F8)     UnregisterHotKey(g_orb_hwnd, HOTKEY_ID_F8);
}
int plat_poll_hotkey(void) {
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        /* A hotkey registered before the window existed is still a thread
         * message with nowhere to be dispatched, so latch it here too. */
        if (msg.message == WM_HOTKEY && !msg.hwnd) {
            int id = hotkey_id_to_plat(msg.wParam);
            if (id) InterlockedExchange(&g_hotkey_latch, id);
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)InterlockedExchange(&g_hotkey_latch, 0);
}

/* ---- right-click menu ------------------------------------------------- */
/* The menu is grouped by what you are making -- GIF, then MP4, then camera
 * -- with each action's key right-aligned via \t. Capture toggles live in
 * their own submenu rather than doubling up on the action items: a row that
 * both performs an action AND carries a checkbox cannot say which one a
 * click means. */
int plat_show_menu(struct GLFWwindow* w, const PlatMenuState* st,
                   const PlatMonitor* monitors, int nmonitors) {
    HWND h = glfwGetWin32Window(w);
    HMENU root = CreatePopupMenu();

    /* --- monitor submenus, one per output format --- */
    HMENU gifMon = CreatePopupMenu();
    HMENU vidMon = CreatePopupMenu();
    for (int i = 0; i < nmonitors; i++) {
        AppendMenuA(gifMon, MF_STRING, PLAT_MENU_MONITOR_BASE  + (UINT)i, monitors[i].name);
        AppendMenuA(vidMon, MF_STRING, PLAT_MENU_VMONITOR_BASE + (UINT)i, monitors[i].name);
    }

    /* --- screenshot: the quickest path of all --- */
    AppendMenuA(root, MF_STRING, PLAT_MENU_SHOT_REGION, "Screenshot: region\tF4 / PrtSc");
    AppendMenuA(root, MF_STRING, PLAT_MENU_SHOT_WINDOW, "Screenshot: window");
    /* One row, not a submenu. Delayed capture exists for one job --
     * photographing a menu, which cannot be done any other way because
     * Windows menus close on any keystroke. Five seconds is enough to
     * reopen a menu and long enough not to rush; offering 3 and 10 as
     * well cost three rows of a menu that is already long. */
    AppendMenuA(root, MF_STRING, PLAT_MENU_SHOT_DELAY_5,
                "Screenshot: delayed 5s (captures menus)");
    AppendMenuA(root, MF_SEPARATOR, 0, NULL);

    /* --- GIF --- */
    AppendMenuA(root, MF_STRING, PLAT_MENU_RECORD_GIF,    "GIF: record window\tF5");
    AppendMenuA(root, MF_STRING, PLAT_MENU_RECORD_REGION, "GIF: record region\tF6");
    AppendMenuA(root, MF_POPUP, (UINT_PTR)gifMon,         "GIF: record monitor");
    AppendMenuA(root, MF_SEPARATOR, 0, NULL);

    /* --- MP4 --- */
    AppendMenuA(root, MF_STRING, PLAT_MENU_RECORD_VIDEO,     "MP4: record window\tF7");
    AppendMenuA(root, MF_STRING, PLAT_MENU_RECORD_VIDEO_RGN, "MP4: record region\tF8");
    AppendMenuA(root, MF_POPUP, (UINT_PTR)vidMon,            "MP4: record monitor");
    AppendMenuA(root, MF_SEPARATOR, 0, NULL);

    /* --- camera + sound --- */
    {
        char cams[PLAT_MAX_CAMERAS][64];
        int ncam = plat_camera_list(cams, PLAT_MAX_CAMERAS);
        if (ncam > 0) {
            HMENU camMenu = CreatePopupMenu();
            for (int i = 0; i < ncam; i++) {
                /* Number identical devices so they can be told apart.
                 *
                 * There is deliberately NO "in use" marker here. Windows
                 * Media Foundation SHARES cameras between applications --
                 * verified by holding one open and streaming in one process
                 * while a second opened the same device successfully -- so
                 * "in use" is not a state that exists to be shown, and a
                 * marker that can never appear is worse than none. It also
                 * cost an activate/release per camera every time the menu
                 * opened. */
                char row[96];
                bool dup = false;
                for (int j = 0; j < ncam && !dup; j++)
                    if (j != i && strcmp(cams[j], cams[i]) == 0) dup = true;
                if (dup) snprintf(row, sizeof row, "%s  (%d)", cams[i], i + 1);
                else     snprintf(row, sizeof row, "%s", cams[i]);
                AppendMenuA(camMenu, MF_STRING,
                            PLAT_MENU_CAMERA_BASE + (UINT)i, row);
            }
            AppendMenuA(root, MF_POPUP, (UINT_PTR)camMenu, "Camera: record\tF9");
        } else {
            AppendMenuA(root, MF_STRING | MF_DISABLED | MF_GRAYED, 0,
                        "Camera: none found");
        }
    }
    {
        static const char* AUDN[4] = { "No sound", "System sound",
                                       "Microphone", "System + microphone" };
        HMENU audMenu = CreatePopupMenu();
        for (int i = 0; i < 4; i++)
            AppendMenuA(audMenu,
                        MF_STRING | (st->audio_src == i ? MF_CHECKED : MF_UNCHECKED),
                        PLAT_MENU_AUDIO_BASE + (UINT)i, AUDN[i]);
        AppendMenuA(root, MF_POPUP, (UINT_PTR)audMenu, "Sound source");
    }
    {
        /* Which format the double-click gesture arms for. Radio-style:
         * one gesture cannot mean two things at once. */
        HMENU dblMenu = CreatePopupMenu();
        AppendMenuA(dblMenu, MF_STRING | (st->dbl_video ? MF_UNCHECKED : MF_CHECKED),
                    PLAT_MENU_DBL_GIF, "Double-click arms GIF");
        AppendMenuA(dblMenu, MF_STRING | (st->dbl_video ? MF_CHECKED : MF_UNCHECKED),
                    PLAT_MENU_DBL_MP4, "Double-click arms MP4");
        AppendMenuA(root, MF_POPUP, (UINT_PTR)dblMenu, "Double-click gesture");
    }
    AppendMenuA(root, MF_SEPARATOR, 0, NULL);

    /* --- hotkey capture, one checkbox per key ---
     * Releasing a key hands it back to whatever else wants it, which is the
     * whole point: F5 in particular is Refresh almost everywhere. */
    {
        static const char* HKN[PLAT_HK_COUNT] = {
            "F5\tGIF window",
            "F6\tGIF region",
            "F7\tMP4 window",
            "F8\tMP4 region",
            "F9\tCamera",
            "F4\tScreenshot",
            "PrtSc\tScreenshot"
        };
        HMENU hkMenu = CreatePopupMenu();
        for (int i = 0; i < PLAT_HK_COUNT; i++)
            AppendMenuA(hkMenu,
                        MF_STRING | (st->hotkey_on[i] ? MF_CHECKED : MF_UNCHECKED),
                        PLAT_MENU_HOTKEY_BASE + (UINT)i, HKN[i]);
        AppendMenuA(root, MF_POPUP, (UINT_PTR)hkMenu, "Hotkeys captured");
    }

    /* --- output behaviour --- */
    AppendMenuA(root, MF_STRING | (st->auto_open ? MF_CHECKED : MF_UNCHECKED),
                PLAT_MENU_TOGGLE_AUTOOPEN, "Open folder after saving");
    AppendMenuA(root, MF_STRING | (st->auto_clip ? MF_CHECKED : MF_UNCHECKED),
                PLAT_MENU_TOGGLE_CLIP,     "Copy file to clipboard");
    {
        /* Radio rather than a checkbox: "edit it here", "edit it in Paint"
         * and "just save it" are three answers to one question, and a pair of
         * checkboxes that cannot both be ticked is a worse way to say that. */
        HMENU se = CreatePopupMenu();
        static const char* SEN[3] = { "Nothing -- just save it",
                                      "The built-in editor",
                                      "The system image editor (Paint)" };
        for (int i = 0; i < 3; i++)
            AppendMenuA(se, MF_STRING | (st->shot_editor == i ? MF_CHECKED : 0),
                        PLAT_MENU_SHOT_ED_BASE + (UINT)i, SEN[i]);
        AppendMenuA(root, MF_POPUP, (UINT_PTR)se, "After a screenshot, open >");
    }
    {
        /* Clipboard history. Off by default -- a program that starts
         * remembering everything you copy without being asked is exactly the
         * thing we are offering an alternative to. */
        HMENU ch = CreatePopupMenu();
        static const char* CHN[PLAT_CLIPHIST_CHOICES] = {
            "Off", "Keep last 5", "Keep last 10", "Keep last 20" };
        static const int CHV[PLAT_CLIPHIST_CHOICES] = { 0, 5, 10, 20 };
        for (int i = 0; i < PLAT_CLIPHIST_CHOICES; i++)
            AppendMenuA(ch, MF_STRING | (st->clip_keep == CHV[i] ? MF_CHECKED : 0),
                        PLAT_MENU_CLIPHIST_BASE + (UINT)i, CHN[i]);
        AppendMenuA(root, MF_POPUP, (UINT_PTR)ch,
                    "Clipboard history (middle-click) >");
    }
    AppendMenuA(root, MF_STRING | (st->draw_cursor ? MF_CHECKED : MF_UNCHECKED),
                PLAT_MENU_TOGGLE_CURSOR, "Record the mouse pointer");
    AppendMenuA(root, MF_STRING | (st->moon_on ? MF_CHECKED : MF_UNCHECKED),
                PLAT_MENU_TOGGLE_MOON, "Moon (C0ry)");
    if (st->ghost_on) {
        AppendMenuA(root, MF_STRING, PLAT_MENU_FILM_STOP,
                    "Stop filming the orb");
    } else {
        HMENU fm = CreatePopupMenu();
        AppendMenuA(fm, MF_STRING, PLAT_MENU_FILM_GIF, "as GIF");
        AppendMenuA(fm, MF_STRING, PLAT_MENU_FILM_MP4, "as MP4");
        AppendMenuA(root, MF_POPUP, (UINT_PTR)fm, "Film the orb itself >");
    }
    AppendMenuA(root, MF_STRING | (st->tray_only ? MF_CHECKED : MF_UNCHECKED),
                PLAT_MENU_TOGGLE_TRAY,     "Tray icon only");
    AppendMenuA(root, MF_STRING, PLAT_MENU_HIDE_ORB,
                "Hide orb (tray icon brings it back)");
    AppendMenuA(root, MF_STRING | (st->run_at_startup ? MF_CHECKED : 0),
                PLAT_MENU_RUN_AT_STARTUP, "Run at startup");
    AppendMenuA(root, MF_SEPARATOR, 0, NULL);

    /* --- files + help --- */
    AppendMenuA(root, MF_STRING, PLAT_MENU_OPEN_EDITOR,   "Open image or GIF...");
    AppendMenuA(root, MF_STRING, PLAT_MENU_HELP,          "Help\tF1");
    AppendMenuA(root, MF_STRING, PLAT_MENU_EDIT_SETTINGS, "Edit settings file...");
    if (plat_process_is_elevated()) {
        AppendMenuA(root, MF_STRING | MF_DISABLED | MF_GRAYED, 0,
                    "Running as administrator");
    } else {
        AppendMenuA(root, MF_STRING, PLAT_MENU_RESTART_ADMIN,
                    "Restart as administrator...");
    }
    AppendMenuA(root, MF_SEPARATOR, 0, NULL);
    AppendMenuA(root, MF_STRING, PLAT_MENU_QUIT, "Quit");

    POINT p; GetCursorPos(&p);

    /* Keep the main loop's work alive for as long as this menu is up. Without
     * this the thread is parked in TrackPopupMenu and any recording in
     * progress simply stops taking frames -- which is why the orb could not
     * record its own menu. 30 ms is one frame at the video rate. */
    SetTimer(h, ORB_MODAL_TIMER_ID, 30, NULL);

    /* TrackPopupMenu needs a foreground window or the menu will not dismiss
     * when the user clicks away -- but that activation is exactly what makes
     * the shell restore the taskbar button. Do it, then put the style back. */
    SetForegroundWindow(h);
    int cmd = TrackPopupMenu(root,
                             TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_TOPALIGN,
                             p.x, p.y, 0, h, NULL);
    KillTimer(h, ORB_MODAL_TIMER_ID);
    DestroyMenu(root);

    /* The documented incantation: without this the menu can leave the window
     * in a half-activated state and the next click is swallowed. */
    PostMessage(h, WM_NULL, 0, 0);

    /* Re-assert. apply_taskbar_button() returns immediately when the style is
     * already right, so this costs nothing in the common case and only does
     * the hide/show cycle when the shell actually took the button back. */
    apply_taskbar_button(h, st && st->tray_only);

    return cmd;
}

/* ---- pressure, as close as Windows gets -------------------------------
 *
 * There is no PSI here. Windows can say how FULL memory is and how BUSY the
 * CPU is, neither of which is what PSI measures -- a machine can be at 95%
 * memory with nothing waiting on it. So these are reported for what they are
 * and `true_stall` is false, rather than dressing utilisation up as pressure.
 *
 * IO is left unmeasured instead of guessed. -1 is an honest answer. */
void plat_pressure(PlatPressure* out) {
    if (!out) return;
    out->mem = -1; out->cpu = -1; out->io = -1;
    out->true_stall = false;

    out->disk_used = -1; out->disk_free_mb = 0; out->disk_total_mb = 0;

    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) out->mem = (int)ms.dwMemoryLoad * 10;

    /* The drive Windows itself is on -- the one whose filling up breaks
     * everything, rather than whichever one we happen to save GIFs to. */
    ULARGE_INTEGER avail, total, freebytes;
    char sysdir[MAX_PATH] = "C:\\";
    if (GetWindowsDirectoryA(sysdir, MAX_PATH) >= 3) sysdir[3] = 0;
    if (GetDiskFreeSpaceExA(sysdir, &avail, &total, &freebytes) &&
        total.QuadPart > 0) {
        out->disk_total_mb = (int)(total.QuadPart / (1024 * 1024));
        out->disk_free_mb  = (int)(avail.QuadPart / (1024 * 1024));
        unsigned long long used = total.QuadPart - avail.QuadPart;
        out->disk_used = (int)((used * 1000ULL) / total.QuadPart);
    }

    /* CPU busy over the interval between calls. The first call has no
     * previous sample to difference against, so it reports -1 rather than a
     * meaningless figure computed from boot. */
    static ULONGLONG p_idle = 0, p_kern = 0, p_user = 0;
    FILETIME fi, fk, fu;
    if (GetSystemTimes(&fi, &fk, &fu)) {
        ULONGLONG i = ((ULONGLONG)fi.dwHighDateTime << 32) | fi.dwLowDateTime;
        ULONGLONG k = ((ULONGLONG)fk.dwHighDateTime << 32) | fk.dwLowDateTime;
        ULONGLONG u = ((ULONGLONG)fu.dwHighDateTime << 32) | fu.dwLowDateTime;
        if (p_kern || p_user) {
            ULONGLONG di = i - p_idle, dk = k - p_kern, du = u - p_user;
            ULONGLONG total = dk + du;          /* kernel time includes idle */
            if (total > 0 && di <= total)
                out->cpu = (int)(((total - di) * 1000ULL) / total);
        }
        p_idle = i; p_kern = k; p_user = u;
    }
}

/* ---- start another copy of ourselves ---------------------------------- */
bool plat_spawn_self(const char* args) {
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH)) return false;
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "\"%s\" %s", exe, args ? args : "");

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);

    /* CREATE_NO_WINDOW: the ghost has no console and must not flash one --
     * a console window appearing would land in the very recording it is
     * being started to make. */
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);   /* it outlives us; we do not wait on it */
    return true;
}

void plat_temp_dir(char* out, size_t sz) {
    DWORD n = GetTempPathA((DWORD)sz, out);
    if (n == 0 || n >= sz) snprintf(out, sz, "C:\\Temp\\");
}

/* ---- image helpers: borrow the shell's decoders + thumbnail cache ------ */

/* IShellItemImageFactory {BCC18B79-BA16-442F-80C4-8A59C30C463B}.
 * Defined locally rather than pulled from libuuid so the link line stays
 * the same across MinGW installs. */
static const GUID IID_IShellItemImageFactory_ =
    { 0xbcc18b79, 0xba16, 0x442f,
      { 0x80, 0xc4, 0x8a, 0x59, 0xc3, 0x0c, 0x46, 0x3b } };

/* Ask the shell for a file's thumbnail. This is the same bitmap Explorer
 * shows, pulled from the same cache, which means:
 *   - every registered codec works (WebP/HEIC/RAW/PSD/video posters)
 *   - already-browsed folders return essentially instantly
 * Returns malloc'd RGBA, or NULL if the shell cannot produce one. */
uint8_t* plat_shell_thumbnail(const char* path, int want_px, int* out_w, int* out_h) {
    if (!path || !*path) return NULL;

    /* IShellItemImageFactory is COM; make sure this thread is initialized. */
    HRESULT hrco = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool we_init = SUCCEEDED(hrco);

    /* SHCreateItemFromParsingName only accepts backslash-separated paths --
     * a forward slash makes it fail outright, which is easy to mistake for
     * "no thumbnail available". Normalize first.
     * Paths arrive as UTF-8 (GLFW's drop callback); CP_ACP would mangle
     * anything non-ASCII, e.g. the em-dash in a Firefox screenshot name. */
    char norm[MAX_PATH];
    size_t plen = strlen(path);
    if (plen >= sizeof norm) plen = sizeof norm - 1;
    for (size_t i = 0; i < plen; i++) norm[i] = (path[i] == '/') ? '\\' : path[i];
    norm[plen] = 0;

    wchar_t wpath[MAX_PATH];
    if (MultiByteToWideChar(CP_UTF8, 0, norm, -1, wpath, MAX_PATH) == 0) {
        if (we_init) CoUninitialize();
        return NULL;
    }

    IShellItemImageFactory* factory = NULL;
    HRESULT hr = SHCreateItemFromParsingName(wpath, NULL,
                                             &IID_IShellItemImageFactory_,
                                             (void**)&factory);
    if (FAILED(hr) || !factory) {
        if (we_init) CoUninitialize();
        return NULL;
    }

    SIZE sz; sz.cx = want_px; sz.cy = want_px;
    HBITMAP hbm = NULL;
    /* BIGGERSIZEOK lets the shell hand back its cached tile rather than
     * re-rendering to our exact request -- much faster, and we scale. */
    hr = factory->lpVtbl->GetImage(factory, sz,
                                   SIIGBF_BIGGERSIZEOK | SIIGBF_THUMBNAILONLY,
                                   &hbm);
    if (FAILED(hr) || !hbm) {
        /* Retry allowing an icon, so unknown types still show something. */
        hr = factory->lpVtbl->GetImage(factory, sz, SIIGBF_BIGGERSIZEOK, &hbm);
    }
    factory->lpVtbl->Release(factory);

    if (FAILED(hr) || !hbm) {
        if (we_init) CoUninitialize();
        return NULL;
    }

    BITMAP bm;
    if (!GetObject(hbm, sizeof bm, &bm)) {
        DeleteObject(hbm);
        if (we_init) CoUninitialize();
        return NULL;
    }
    int w = bm.bmWidth, h = bm.bmHeight;

    /* Pull the pixels out as a top-down 32bpp DIB. */
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;          /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    uint8_t* raw = (uint8_t*)malloc((size_t)w * h * 4);
    if (!raw) {
        DeleteObject(hbm);
        if (we_init) CoUninitialize();
        return NULL;
    }
    HDC dc = GetDC(NULL);
    int got = GetDIBits(dc, hbm, 0, h, raw, &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    DeleteObject(hbm);
    if (we_init) CoUninitialize();

    if (got == 0) { free(raw); return NULL; }

    /* BGRA -> RGBA. Shell thumbnails may carry premultiplied alpha; composite
     * onto black so transparent PNGs do not come back as garbage. */
    for (int i = 0; i < w * h; i++) {
        uint8_t b = raw[i*4 + 0], g_ = raw[i*4 + 1], r = raw[i*4 + 2], a = raw[i*4 + 3];
        if (a == 0) {
            /* Fully transparent, or an alpha-less source the shell zero-filled. */
            raw[i*4 + 0] = r; raw[i*4 + 1] = g_; raw[i*4 + 2] = b; raw[i*4 + 3] = 255;
        } else {
            raw[i*4 + 0] = r; raw[i*4 + 1] = g_; raw[i*4 + 2] = b; raw[i*4 + 3] = 255;
        }
    }
    *out_w = w; *out_h = h;
    return raw;
}

static bool is_image_ext_(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    static const char* exts[] = {
        ".jpg",".jpeg",".png",".gif",".bmp",".tga",".webp",".tif",".tiff",
        ".heic",".avif",".jfif",".psd",".ico", NULL
    };
    for (int i = 0; exts[i]; i++) if (_stricmp(dot, exts[i]) == 0) return true;
    return false;
}

static int cmp_str_ci_(const void* a, const void* b) {
    return _stricmp(*(const char**)a, *(const char**)b);
}

int plat_list_sibling_images(const char* path, char** out, int max, int* out_self) {
    if (out_self) *out_self = 0;
    if (!path || !*path || max <= 0) return 0;

    /* UTF-8 in, UTF-8 out, wide in the middle -- FindFirstFileA would drop
     * or mangle any filename containing non-ASCII. */
    wchar_t wpath[MAX_PATH];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH) == 0) return 0;

    wchar_t wdir[MAX_PATH];
    wcsncpy(wdir, wpath, MAX_PATH - 1); wdir[MAX_PATH - 1] = 0;
    wchar_t* wslash = wcsrchr(wdir, L'\\');
    wchar_t* wfs    = wcsrchr(wdir, L'/');
    if (wfs > wslash) wslash = wfs;
    const wchar_t* wself = wpath;
    if (wslash) { *wslash = 0; wself = wpath + (wslash - wdir) + 1; }
    else        { wcscpy(wdir, L"."); }

    wchar_t wpattern[MAX_PATH];
    _snwprintf(wpattern, MAX_PATH, L"%s\\*", wdir);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char nameU8[MAX_PATH * 3];
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1,
                                nameU8, sizeof nameU8, NULL, NULL) == 0) continue;
        if (!is_image_ext_(nameU8)) continue;
        if (n >= max) break;

        wchar_t wfull[MAX_PATH];
        _snwprintf(wfull, MAX_PATH, L"%s\\%s", wdir, fd.cFileName);
        char fullU8[MAX_PATH * 3];
        if (WideCharToMultiByte(CP_UTF8, 0, wfull, -1,
                                fullU8, sizeof fullU8, NULL, NULL) == 0) continue;
        char* full = _strdup(fullU8);
        if (!full) break;
        out[n++] = full;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    qsort(out, (size_t)n, sizeof(char*), cmp_str_ci_);

    /* Locate the dropped file in the sorted list (compare in UTF-8). */
    if (out_self) {
        char selfU8[MAX_PATH * 3];
        if (WideCharToMultiByte(CP_UTF8, 0, wself, -1,
                                selfU8, sizeof selfU8, NULL, NULL) != 0) {
            for (int i = 0; i < n; i++) {
                const char* base = strrchr(out[i], '\\');
                base = base ? base + 1 : out[i];
                if (_stricmp(base, selfU8) == 0) { *out_self = i; break; }
            }
        }
    }
    return n;
}

/* ---- what this machine can decode ------------------------------------
 * Windows Imaging Component knows every registered decoder, which on a
 * stock Windows 11 box is ~14 codecs covering ~60 extensions -- including
 * HEIC/AVIF and the whole camera-RAW family. Asking WIC beats hardcoding a
 * list that goes stale the moment someone installs a codec pack. */

static const GUID CLSID_WICImagingFactory_ =
  {0xCACAF262,0x9370,0x4615,{0xA1,0x3B,0x9F,0x55,0x39,0xDA,0x4C,0x0A}};
static const GUID IID_IWICImagingFactory_ =
  {0xEC5EC8A9,0xC395,0x4314,{0x9C,0x77,0x54,0xD7,0xA9,0x35,0xFF,0x70}};
static const GUID IID_IWICBitmapCodecInfo_ =
  {0xE87A44C4,0xB76E,0x4C47,{0x8B,0x09,0x29,0x8E,0xB1,0x2A,0x27,0x14}};

int plat_list_image_extensions(char* out, size_t sz) {
    if (!out || sz == 0) return 0;
    out[0] = 0;

    HRESULT hrco = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool we_init = SUCCEEDED(hrco);

    IWICImagingFactory* fac = NULL;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory_, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory_, (void**)&fac)) || !fac) {
        if (we_init) CoUninitialize();
        return 0;
    }
    IEnumUnknown* en = NULL;
    if (FAILED(IWICImagingFactory_CreateComponentEnumerator(
            fac, WICDecoder, WICComponentEnumerateDefault, &en)) || !en) {
        IWICImagingFactory_Release(fac);
        if (we_init) CoUninitialize();
        return 0;
    }

    int count = 0;
    IUnknown* unk = NULL;
    ULONG got = 0;
    while (IEnumUnknown_Next(en, 1, &unk, &got) == S_OK && got) {
        IWICBitmapCodecInfo* ci = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface(unk, &IID_IWICBitmapCodecInfo_, (void**)&ci))) {
            WCHAR wext[1024] = {0};
            UINT len = 0;
            if (SUCCEEDED(IWICBitmapCodecInfo_GetFileExtensions(ci, 1024, wext, &len))) {
                char ext[2048];
                if (WideCharToMultiByte(CP_UTF8, 0, wext, -1, ext, sizeof ext, NULL, NULL)) {
                    /* Lowercase, then append each ".xxx" we do not already have. */
                    for (char* c = ext; *c; c++)
                        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
                    char* tok = strtok(ext, ",");
                    while (tok) {
                        while (*tok == ' ') tok++;
                        if (*tok == '.') {
                            char probe[40];
                            snprintf(probe, sizeof probe, "%s,", tok);
                            if (!strstr(out, probe)) {
                                size_t cur = strlen(out);
                                if (cur + strlen(probe) + 1 < sz) {
                                    strcat(out, probe);
                                    count++;
                                }
                            }
                        }
                        tok = strtok(NULL, ",");
                    }
                }
            }
            IWICBitmapCodecInfo_Release(ci);
        }
        IUnknown_Release(unk);
    }
    IEnumUnknown_Release(en);
    IWICImagingFactory_Release(fac);
    if (we_init) CoUninitialize();
    return count;
}

/* ---- open-file dialog + default handler ------------------------------- */

bool plat_open_file_dialog(char* out, size_t out_sz) {
    if (!out || out_sz < MAX_PATH) return false;
    char buf[MAX_PATH]; buf[0] = 0;

    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner   = NULL;
    ofn.lpstrFilter =
        "Images and GIFs\0*.gif;*.jpg;*.jpeg;*.png;*.bmp;*.tga;*.psd;*.webp;*.tif;*.tiff\0"
        "GIF\0*.gif\0"
        "All files\0*.*\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = sizeof buf;
    ofn.lpstrTitle  = "Open in ORB_Recorder";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR
                    | OFN_EXPLORER;

    /* Start in Pictures\GIF, which is where recordings land. */
    char initial[MAX_PATH];
    plat_get_output_dir(initial, sizeof initial);
    ofn.lpstrInitialDir = initial;

    if (!GetOpenFileNameA(&ofn)) return false;   /* cancelled */
    snprintf(out, out_sz, "%s", buf);
    return true;
}

void plat_open_with_default_app(const char* path) {
    char norm[MAX_PATH];
    size_t n = strlen(path);
    if (n >= sizeof norm) n = sizeof norm - 1;
    for (size_t i = 0; i < n; i++) norm[i] = (path[i] == '/') ? '\\' : path[i];
    norm[n] = 0;
    ShellExecuteA(NULL, "open", norm, NULL, NULL, SW_SHOWNORMAL);
}

/* ---- reveal file ------------------------------------------------------
 * Explorer /select requires backslashes -- forward slashes silently no-op.
 * Also ShellExecute needs COM initialized on the calling thread; when we
 * hop in here from a background thread the thread trampoline handles it.
 */
uint8_t* plat_render_text(const char* s, int px_height, int* out_w, int* out_h) {
    if (!s || !*s || px_height < 4) return NULL;
    if (px_height > 512) px_height = 512;

    /* Segoe UI is the Windows UI face and is present on everything since
     * Vista; the fallback chain in CreateFont handles the day it is not. */
    HFONT font = CreateFontA(-px_height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS,
                             "Segoe UI");
    if (!font) return NULL;

    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    if (!dc) { DeleteObject(font); return NULL; }
    HGDIOBJ oldf = SelectObject(dc, font);

    SIZE sz;
    if (!GetTextExtentPoint32A(dc, s, (int)strlen(s), &sz) || sz.cx <= 0) {
        SelectObject(dc, oldf); DeleteDC(dc); DeleteObject(font); return NULL;
    }
    int w = sz.cx + 4, h = sz.cy + 2;      /* room for the antialiased edge */

    /* White on black, then read the green channel as coverage. Rendering to a
     * mask this way rather than asking for a glyph bitmap keeps kerning,
     * ligatures and the font's own hinting, because GDI lays out the whole
     * string exactly as it would on screen. */
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;           /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP dib = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dib) {
        SelectObject(dc, oldf); DeleteDC(dc); DeleteObject(font); return NULL;
    }
    HGDIOBJ oldb = SelectObject(dc, dib);
    RECT all = { 0, 0, w, h };
    FillRect(dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutA(dc, 2, 1, s, (int)strlen(s));
    GdiFlush();

    uint8_t* mask = (uint8_t*)malloc((size_t)w * h);
    if (mask) {
        const uint8_t* src = (const uint8_t*)bits;
        for (int i = 0; i < w * h; i++) mask[i] = src[i * 4 + 1];
        *out_w = w; *out_h = h;
    }
    SelectObject(dc, oldb);
    DeleteObject(dib);
    SelectObject(dc, oldf);
    DeleteDC(dc);
    DeleteObject(font);
    return mask;
}

/* Windows composites through the DWM and BitBlt/PrintWindow keep working,
 * so there is no equivalent trap here. */
const char* plat_capture_unavailable(void) { return NULL; }

/* No portal, and none needed: BitBlt reads the screen here. */
/* The Windows clipboard owns the bytes once they are handed over, so there
 * is nothing to serve afterwards. */
void plat_clipboard_serve(void) { }

bool plat_portal_screenshot(char* out_path, size_t out_sz) {
    (void)out_path; (void)out_sz; return false;
}

bool plat_open_in_editor(const char* path) {
    char norm[MAX_PATH];
    size_t n = strlen(path);
    if (n >= sizeof norm) n = sizeof norm - 1;
    for (size_t i = 0; i < n; i++) norm[i] = (path[i] == '/') ? '\\' : path[i];
    norm[n] = 0;

    /* Whatever this machine associates with EDITING an image -- Paint on a
     * stock install, Paint.NET or GIMP or Photoshop if the user said so. */
    HINSTANCE r = ShellExecuteA(NULL, "edit", norm, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)r > 32) return true;

    /* No edit verb registered for this type -- ask for Paint by name rather
     * than give up. It is not always on the PATH, but it has an App Paths
     * registry entry, which is what ShellExecute resolves names through. */
    char arg[MAX_PATH + 4];
    snprintf(arg, sizeof arg, "\"%s\"", norm);   /* spaces in the path */
    r = ShellExecuteA(NULL, "open", "mspaint.exe", arg, NULL, SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

void plat_open_folder_select(const char* file_path) {
    char norm[MAX_PATH];
    size_t n = strlen(file_path);
    if (n >= sizeof norm) n = sizeof norm - 1;
    for (size_t i = 0; i < n; i++) norm[i] = (file_path[i] == '/') ? '\\' : file_path[i];
    norm[n] = 0;

    /* SHOpenFolderAndSelectItems is the API Explorer itself uses. Unlike
     * `explorer.exe /select,path`, which spawns a FRESH window every single
     * time, this reuses a window already showing that folder -- it just
     * raises it and moves the selection to the new file. After a dozen
     * recordings that is the difference between one window and a dozen. */
    HRESULT hrco = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool we_init = SUCCEEDED(hrco);

    bool ok = false;
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathA(norm);
    if (pidl) {
        ok = SUCCEEDED(SHOpenFolderAndSelectItems(pidl, 0, NULL, 0));
        ILFree(pidl);
    }
    if (we_init) CoUninitialize();

    if (!ok) {
        /* Fall back to the blunt instrument if the shell refused. */
        char arg[MAX_PATH + 32];
        snprintf(arg, sizeof arg, "/select,\"%s\"", norm);
        ShellExecuteA(NULL, "open", "explorer.exe", arg, NULL, SW_SHOWNORMAL);
    }
}

/* Put the picture itself on the clipboard as CF_DIB, which is what a chat
 * box or word processor pastes. plat_clipboard_copy_file() puts a FILE on
 * the clipboard (CF_HDROP) -- different format, different paste behaviour,
 * and both are worth having. */
void plat_clipboard_copy_image(const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return;

    size_t px_bytes = (size_t)w * h * 4;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + px_bytes);
    if (!hg) return;
    void* mem = GlobalLock(hg);
    if (!mem) { GlobalFree(hg); return; }

    BITMAPINFOHEADER* bi = (BITMAPINFOHEADER*)mem;
    memset(bi, 0, sizeof *bi);
    bi->biSize        = sizeof(BITMAPINFOHEADER);
    bi->biWidth       = w;
    bi->biHeight      = h;          /* positive: bottom-up, as CF_DIB expects */
    bi->biPlanes      = 1;
    bi->biBitCount    = 32;
    bi->biCompression = BI_RGB;
    bi->biSizeImage   = (DWORD)px_bytes;

    uint8_t* dst = (uint8_t*)mem + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < h; y++) {
        const uint8_t* srow = rgba + (size_t)y * w * 4;
        uint8_t* drow = dst + (size_t)(h - 1 - y) * w * 4;   /* flip */
        for (int x = 0; x < w; x++) {
            drow[x*4 + 0] = srow[x*4 + 2];   /* B */
            drow[x*4 + 1] = srow[x*4 + 1];   /* G */
            drow[x*4 + 2] = srow[x*4 + 0];   /* R */
            drow[x*4 + 3] = 255;
        }
    }
    GlobalUnlock(hg);

    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        SetClipboardData(CF_DIB, hg);   /* clipboard owns hg now */
        CloseClipboard();
    } else {
        GlobalFree(hg);
    }
}

/* ---- 2D fallback painting ---------------------------------------------
 * GDI, double-buffered into a memory bitmap so the orb never flickers.
 * The window's region (set elsewhere) is what makes this round -- painting
 * a rectangle inside a circular region yields a circle, which is why this
 * needs no alpha channel and therefore no driver support beyond a DC. */
void plat_draw_orb_2d(struct GLFWwindow* w, int win_size, int orb_size,
                      unsigned char cr, unsigned char cg, unsigned char cb) {
    HWND hwnd = glfwGetWin32Window(w);
    if (!hwnd) return;

    HDC dc = GetDC(hwnd);
    if (!dc) return;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, win_size, win_size);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

    /* Backdrop. Clipped away outside the region, so it only shows inside
     * the orb's circle -- it reads as the dark shell the 3D cage has. */
    RECT full = { 0, 0, win_size, win_size };
    HBRUSH back = CreateSolidBrush(RGB(24, 24, 28));
    FillRect(mem, &full, back);
    DeleteObject(back);

    int cx = win_size / 2, cy = win_size / 2;
    int r  = orb_size / 2;

    /* Cage: a light ring at the orb's full radius. */
    HPEN   ring = CreatePen(PS_SOLID, 2, RGB(200, 200, 210));
    HBRUSH hollow = (HBRUSH)GetStockObject(NULL_BRUSH);
    HPEN   oldpen = (HPEN)SelectObject(mem, ring);
    HBRUSH oldbr  = (HBRUSH)SelectObject(mem, hollow);
    Ellipse(mem, cx - r, cy - r, cx + r, cy + r);

    /* Core: filled, at 62% of the radius, in the state colour. Same job the
     * 3D core does -- colour is the state, and that must survive. */
    int cr2 = (r * 62) / 100;
    HBRUSH core = CreateSolidBrush(RGB(cr, cg, cb));
    HPEN   corepen = CreatePen(PS_SOLID, 1, RGB(cr, cg, cb));
    SelectObject(mem, core);
    SelectObject(mem, corepen);
    Ellipse(mem, cx - cr2, cy - cr2, cx + cr2, cy + cr2);

    SelectObject(mem, oldpen);
    SelectObject(mem, oldbr);
    DeleteObject(ring);
    DeleteObject(core);
    DeleteObject(corepen);

    BitBlt(dc, 0, 0, win_size, win_size, mem, 0, 0, SRCCOPY);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, dc);
}

/* ---- single instance --------------------------------------------------
 * A named kernel mutex: ownership is tracked by the kernel and released
 * automatically however the process dies, so there is no stale lock file and
 * no PID to probe.
 *
 * This exists because a second copy cannot take the global hotkeys -- the
 * first already owns them -- so it starts with every capture key dead and a
 * menu full of unchecked boxes, which reads as a broken install rather than
 * a duplicate. */
bool plat_single_instance(const char* tag) {
    static HANDLE held = NULL;
    if (held) return true;
    SetLastError(0);
    char name[128];
    snprintf(name, sizeof name, "Local\\324x_ORB_Recorder_%s",
             (tag && *tag) ? tag : "single");
    held = CreateMutexA(NULL, TRUE, name);
    if (!held) return true;                 /* cannot tell -- do not block */
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(held);
        held = NULL;
        return false;
    }
    return true;
}

void plat_window_set_visible(struct GLFWwindow* w, bool visible) {
    HWND h = glfwGetWin32Window(w);
    ShowWindow(h, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (visible)
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

bool plat_taskbar_button_enforce(struct GLFWwindow* w, bool hidden) {
    return apply_taskbar_button(glfwGetWin32Window(w), hidden);
}

/* Windows popup menus -- including ours, the taskbar's, and any application's
 * context menu -- are top-level windows of class "#32768". Finding one is how
 * a screenshot tool photographs a menu precisely rather than photographing a
 * screen and asking the user to crop. */
bool plat_find_open_menu(int* x, int* y, int* w, int* h) {
    HWND m = NULL;
    for (HWND hw = GetTopWindow(NULL); hw; hw = GetWindow(hw, GW_HWNDNEXT)) {
        char cls[64] = {0};
        if (!IsWindowVisible(hw)) continue;
        if (!GetClassNameA(hw, cls, sizeof cls)) continue;
        if (strcmp(cls, "#32768") != 0) continue;
        RECT r;
        if (!GetWindowRect(hw, &r)) continue;
        if (r.right - r.left < 40 || r.bottom - r.top < 20) continue;
        m = hw;
        break;                      /* topmost visible menu wins */
    }
    if (!m) return false;

    RECT r;
    if (!GetWindowRect(m, &r)) return false;

    /* A few pixels of margin so the drop shadow is not sliced off. */
    const int PAD = 8;
    *x = r.left - PAD;
    *y = r.top  - PAD;
    *w = (r.right  - r.left) + PAD * 2;
    *h = (r.bottom - r.top)  + PAD * 2;
    return true;
}

/* ---- run at login -----------------------------------------------------
 * HKCU\Software\Microsoft\Windows\CurrentVersion\Run. Per-user, needs no
 * elevation, and is the same entry Settings > Startup Apps shows -- so the
 * user can turn it off where they would expect to.
 *
 * This is what makes "no installer" honest: copy the exe anywhere, run it,
 * tick the box. The path is re-read from the running module each time it is
 * enabled, so moving the exe and re-ticking repairs the entry. */

#define ORB_RUN_KEY   "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define ORB_RUN_NAME  "ORB_Recorder"

bool plat_get_run_at_startup(void) {
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, ORB_RUN_KEY, 0, KEY_QUERY_VALUE, &k)
        != ERROR_SUCCESS) return false;
    char buf[MAX_PATH * 2];
    DWORD sz = sizeof buf, type = 0;
    LONG r = RegQueryValueExA(k, ORB_RUN_NAME, NULL, &type, (LPBYTE)buf, &sz);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;

    /* Present but pointing at a different copy counts as OFF -- otherwise the
     * box reads ticked while a stale path launches something else. */
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH)) return true;
    return strstr(buf, exe) != NULL;
}

bool plat_set_run_at_startup(bool on) {
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, ORB_RUN_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return false;
    bool ok;
    if (on) {
        char exe[MAX_PATH];
        if (!GetModuleFileNameA(NULL, exe, MAX_PATH)) { RegCloseKey(k); return false; }
        char quoted[MAX_PATH + 4];
        snprintf(quoted, sizeof quoted, "\"%s\"", exe);   /* spaces in the path */
        ok = RegSetValueExA(k, ORB_RUN_NAME, 0, REG_SZ,
                            (const BYTE*)quoted, (DWORD)strlen(quoted) + 1)
             == ERROR_SUCCESS;
    } else {
        LONG r = RegDeleteValueA(k, ORB_RUN_NAME);
        ok = (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(k);
    return ok;
}

/* ---- clipboard file-drop --------------------------------------------- */
void plat_clipboard_copy_file(const char* file_path) {
    /* Normalize slashes -- CF_HDROP expects backslash paths. */
    char norm[MAX_PATH];
    size_t n = strlen(file_path);
    if (n >= sizeof norm - 1) n = sizeof norm - 2;
    for (size_t i = 0; i < n; i++) norm[i] = (file_path[i] == '/') ? '\\' : file_path[i];
    norm[n] = 0;

    /* DROPFILES header + one null-terminated ANSI path + double-null terminator. */
    size_t path_bytes = strlen(norm) + 1;
    size_t total = sizeof(DROPFILES) + path_bytes + 1;   /* +1 for final null */
    HGLOBAL hg = GlobalAlloc(GHND, total);
    if (!hg) return;
    DROPFILES* df = (DROPFILES*)GlobalLock(hg);
    if (!df) { GlobalFree(hg); return; }
    df->pFiles = sizeof(DROPFILES);
    df->pt.x = 0; df->pt.y = 0;
    df->fNC = FALSE;
    df->fWide = FALSE;
    char* dst = (char*)df + sizeof(DROPFILES);
    memcpy(dst, norm, path_bytes);
    dst[path_bytes] = 0;
    GlobalUnlock(hg);

    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        SetClipboardData(CF_HDROP, hg);
        CloseClipboard();
        /* hg is now owned by the clipboard -- don't free. */
    } else {
        GlobalFree(hg);
    }
}

/* ---- clipboard history -------------------------------------------------
 *
 * See platform.h for why this exists and what it refuses to record.
 */

void plat_clipboard_copy_text(const char* utf8) {
    if (!utf8) return;
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wn <= 0) return;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (size_t)wn * sizeof(WCHAR));
    if (!hg) return;
    WCHAR* w = (WCHAR*)GlobalLock(hg);
    if (!w) { GlobalFree(hg); return; }
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, wn);
    GlobalUnlock(hg);

    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, hg);   /* clipboard owns hg now */
        CloseClipboard();
    } else {
        GlobalFree(hg);
    }
}

void plat_clipboard_watch(struct GLFWwindow* w) {
    static bool started = false;
    if (started || !w) return;
    HWND h = glfwGetWin32Window(w);
    if (!h) return;
    if (AddClipboardFormatListener(h)) {
        started = true;
        /* Take what is already there as the first entry. The exclusion flags
         * below are properties of the clipboard CONTENT, not of the moment it
         * was copied, so a secret sitting there at launch is still protected. */
        InterlockedExchange(&g_clip_change, 1);
    }
}

bool plat_clipboard_changed(void) {
    return InterlockedExchange(&g_clip_change, 0) != 0;
}

/* Did whoever put this here ask us not to keep it?
 *
 * Two registered formats, both Microsoft-documented, both set by every
 * password manager worth using (KeePass, Bitwarden, 1Password):
 *
 *   ExcludeClipboardContentFromMonitorProcessing -- present at all means
 *       "no clipboard monitor should touch this", the stronger of the two.
 *   CanIncludeInClipboardHistory -- a DWORD; 0 means "not in any history".
 *
 * Must be called with the clipboard already open, because reading the second
 * one needs GetClipboardData. */
static bool clip_owner_forbids(void) {
    static UINT f_excl = 0, f_hist = 0;
    if (!f_excl) f_excl = RegisterClipboardFormatA(
                              "ExcludeClipboardContentFromMonitorProcessing");
    if (!f_hist) f_hist = RegisterClipboardFormatA("CanIncludeInClipboardHistory");

    if (f_excl && IsClipboardFormatAvailable(f_excl)) return true;

    if (f_hist && IsClipboardFormatAvailable(f_hist)) {
        HANDLE h = GetClipboardData(f_hist);
        if (h) {
            DWORD* v = (DWORD*)GlobalLock(h);
            if (v) {
                bool no = (*v == 0);
                GlobalUnlock(h);
                if (no) return true;
            }
        }
    }
    return false;
}

int plat_clipboard_read(char** out_text, uint8_t** out_rgba, int* out_w, int* out_h) {
    if (out_text) *out_text = NULL;
    if (out_rgba) *out_rgba = NULL;
    if (out_w)    *out_w = 0;
    if (out_h)    *out_h = 0;

    /* Another process can hold the clipboard open for a moment right after
     * writing it, which is precisely when we get told it changed. Retry a few
     * times rather than dropping the entry. */
    int opened = 0;
    for (int try_i = 0; try_i < 8 && !opened; try_i++) {
        opened = OpenClipboard(NULL);
        if (!opened) Sleep(15);
    }
    if (!opened) return PLAT_CLIP_NONE;

    int kind = PLAT_CLIP_NONE;

    if (clip_owner_forbids()) {
        CloseClipboard();
        return PLAT_CLIP_NONE;
    }

    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        WCHAR* w = h ? (WCHAR*)GlobalLock(h) : NULL;
        if (w) {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
            /* A cap, because this lives in RAM and someone will eventually
             * copy a log file into it. */
            if (n > 0 && n <= 1024 * 1024) {
                char* t = (char*)malloc((size_t)n);
                if (t) {
                    WideCharToMultiByte(CP_UTF8, 0, w, -1, t, n, NULL, NULL);
                    if (out_text) *out_text = t; else free(t);
                    kind = PLAT_CLIP_TEXT;
                }
            }
            GlobalUnlock(h);
        }
    } else if (IsClipboardFormatAvailable(CF_BITMAP)) {
        /* CF_BITMAP rather than CF_DIB deliberately: Windows synthesizes it
         * from whatever DIB variant is actually there, so GetDIBits gives us
         * one predictable 32-bit top-down buffer instead of us hand-decoding
         * 24-bit, BI_BITFIELDS and DIBv5 separately.
         *
         * Alpha is forced opaque -- the synthesized bitmap does not carry it,
         * and a half-transparent paste is worse than an opaque one. */
        HBITMAP hb = (HBITMAP)GetClipboardData(CF_BITMAP);
        BITMAP bm;
        if (hb && GetObject(hb, sizeof bm, &bm) &&
            bm.bmWidth > 0 && bm.bmHeight > 0 &&
            bm.bmWidth <= 16384 && bm.bmHeight <= 16384) {

            size_t px = (size_t)bm.bmWidth * bm.bmHeight * 4;
            uint8_t* buf = (uint8_t*)malloc(px);
            if (buf) {
                BITMAPINFO bi;
                memset(&bi, 0, sizeof bi);
                bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth       = bm.bmWidth;
                bi.bmiHeader.biHeight      = -bm.bmHeight;   /* top-down */
                bi.bmiHeader.biPlanes      = 1;
                bi.bmiHeader.biBitCount    = 32;
                bi.bmiHeader.biCompression = BI_RGB;

                HDC dc = GetDC(NULL);
                int got = GetDIBits(dc, hb, 0, (UINT)bm.bmHeight, buf,
                                    &bi, DIB_RGB_COLORS);
                ReleaseDC(NULL, dc);

                if (got) {
                    for (size_t i = 0; i < px; i += 4) {
                        uint8_t b = buf[i];
                        buf[i]     = buf[i + 2];   /* BGRA -> RGBA */
                        buf[i + 2] = b;
                        buf[i + 3] = 255;
                    }
                    if (out_rgba) *out_rgba = buf; else free(buf);
                    if (out_w) *out_w = bm.bmWidth;
                    if (out_h) *out_h = bm.bmHeight;
                    kind = PLAT_CLIP_IMAGE;
                } else {
                    free(buf);
                }
            }
        }
    }

    CloseClipboard();
    return kind;
}

/* ---- a plain list popup ------------------------------------------------ */
int plat_show_list_menu(struct GLFWwindow* w, const char* const* items, int n,
                        const char* title) {
    if (!w || n <= 0) return -1;
    HWND h = glfwGetWin32Window(w);
    HMENU m = CreatePopupMenu();
    if (!m) return -1;

    if (title && *title) {
        AppendMenuA(m, MF_STRING | MF_DISABLED | MF_GRAYED, 0, title);
        AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    }
    for (int i = 0; i < n; i++) {
        /* '&' in a menu string is an accelerator marker and would vanish,
         * turning "a&b" into "ab" with a underlined. Clipboard entries are
         * arbitrary text -- URLs are full of them -- so double each one. */
        char esc[256];
        {
            const char* src = items[i] ? items[i] : "";
            size_t o = 0;
            for (size_t k = 0; src[k] && o < sizeof esc - 2; k++) {
                if (src[k] == '&') esc[o++] = '&';
                esc[o++] = src[k];
            }
            esc[o] = 0;
        }
        WCHAR wbuf[512];
        if (MultiByteToWideChar(CP_UTF8, 0, esc, -1, wbuf, 512) <= 0)
            wbuf[0] = 0;
        /* Item ids are 1-based: TrackPopupMenu returns 0 for "dismissed". */
        AppendMenuW(m, MF_STRING, (UINT_PTR)(i + 1), wbuf);
    }

    POINT p; GetCursorPos(&p);
    SetTimer(h, ORB_MODAL_TIMER_ID, 30, NULL);   /* keep recording alive */
    SetForegroundWindow(h);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_TOPALIGN,
                             p.x, p.y, 0, h, NULL);
    KillTimer(h, ORB_MODAL_TIMER_ID);
    DestroyMenu(m);
    PostMessage(h, WM_NULL, 0, 0);
    return cmd > 0 ? cmd - 1 : -1;
}

/* ---- background jobs --------------------------------------------------
 * The trampoline CoInitializes the worker thread so ShellExecute works
 * from here (it silently no-ops on threads without COM initialized). */
typedef struct { PlatJobFn fn; void* arg; } WinJob;
static DWORD WINAPI win_job_trampoline(LPVOID lp) {
    WinJob* j = (WinJob*)lp;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    j->fn(j->arg);
    if (SUCCEEDED(hr)) CoUninitialize();
    free(j);
    return 0;
}
void plat_run_background(PlatJobFn fn, void* arg) {
    WinJob* j = (WinJob*)malloc(sizeof *j);
    j->fn = fn; j->arg = arg;
    HANDLE h = CreateThread(NULL, 0, win_job_trampoline, j, 0, NULL);
    if (h) CloseHandle(h);
    else   { fn(arg); free(j); }   /* fallback: synchronous */
}
