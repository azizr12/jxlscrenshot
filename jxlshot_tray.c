//       https://github.com/azizr12/jxlscrenshot/

/* ofc this will get flag as a virus
 * because its a c code and need low level access to work
 * there is no malware or shit
 * 
 * leave the app if you are paranoid ! !
 * trying to implement something will make this just garbage app
 * that take megabytes of binary data for a stupid key detection
 * fuck false postive and fuck modern antiviruses
 * RegisterHotKey  is shit tried it and always fail
 *
 * its just a vibe code bullshit
 * leave the app if you are paranoid
 */


// jxlshot_tray.c — System tray extension for jxlshot.

// Configuration is read from jxlshot.ini located next to the executable.
// Debug logs are written to jxlshot_debug.log next to the executable (opened
// fresh at every start, truncating the previous run's data to 0 bytes).



#define _WIN32_IE 0x0600
#define UNICODE
#define _UNICODE
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define JXLSHOT_TRAY_BUILD

#include "resource.h"
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "jxlshot.c" // Pulls in core logic, config, and unified dbg() logger

#include <uxtheme.h>
#pragma comment(lib, "comctl32.lib")

// Undocumented but stable uxtheme APIs for Win32 Dark Mode (Windows 10 1903+)
// Windows Dark Mode Support
typedef enum _PreferredAppMode {
    Default    = 0,
    AllowDark  = 1,
    ForceDark  = 2,
    ForceLight = 3,
    Max        = 4
} PreferredAppMode;

typedef PreferredAppMode (WINAPI *fnSetPreferredAppMode)(PreferredAppMode appMode);
typedef BOOL (WINAPI *fnAllowDarkModeForWindow)(HWND hWnd, BOOL allow);
typedef void (WINAPI *fnFlushMenuThemes)(void);
typedef HRESULT (WINAPI *fnSetWindowTheme)(HWND hwnd, LPCWSTR app, LPCWSTR idList);

// Global state to avoid reloading the DLL repeatedly
static HMODULE g_hUxtheme = NULL;
static fnSetPreferredAppMode g_pSetPreferredAppMode = NULL;
static fnAllowDarkModeForWindow g_pAllowDarkModeForWindow = NULL;
static fnFlushMenuThemes g_pFlushMenuThemes = NULL;
static fnSetWindowTheme g_pSetWindowTheme = NULL;

static void InitializeDarkMode(void) {
    if (!g_hUxtheme) {
        g_hUxtheme = LoadLibraryW(L"uxtheme.dll");
        if (!g_hUxtheme) return;

        g_pSetPreferredAppMode = (fnSetPreferredAppMode)GetProcAddress(g_hUxtheme, MAKEINTRESOURCEA(135));
        g_pAllowDarkModeForWindow = (fnAllowDarkModeForWindow)GetProcAddress(g_hUxtheme, MAKEINTRESOURCEA(133));
        g_pFlushMenuThemes = (fnFlushMenuThemes)GetProcAddress(g_hUxtheme, MAKEINTRESOURCEA(136));
        g_pSetWindowTheme = (fnSetWindowTheme)GetProcAddress(g_hUxtheme, "SetWindowTheme");
    }

    // The app mode and the menu theme are process-wide, so they must be
    // re-applied every time the user flips the sun/moon button. Without this
    // the tray menu and common dialogs kept the theme they were born with.
    if (g_pSetPreferredAppMode) {
        g_pSetPreferredAppMode(g_cfg.dark_mode ? ForceDark : ForceLight);
    }
    if (g_pFlushMenuThemes) {
        g_pFlushMenuThemes();
    }
}

static void ApplyDarkMode(HWND hwnd) {
    InitializeDarkMode();
    if (!hwnd) return;

    BOOL dark = g_cfg.dark_mode ? TRUE : FALSE;
    if (g_pAllowDarkModeForWindow) {
        g_pAllowDarkModeForWindow(hwnd, dark);
    }
    // Explorer is the light common-control theme; DarkMode_Explorer is its dark
    // counterpart. Switching this (rather than only flipping the allow flag)
    // makes an already-open window actually change theme.
    if (g_pSetWindowTheme) {
        g_pSetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    }

    // Crucial: Tell the window and its children to redraw with the new theme
    SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

static HWND g_hwndMenuOwner = NULL;

#define WM_TRAYICON            (WM_USER + 1)
#define WM_HOOK_FULL_CAPTURE   (WM_USER + 10)
#define WM_HOOK_REGION_CAPTURE (WM_USER + 11)

// After a successful capture the tray icon turns green, then this timer puts
// the normal icon back.
#define TIMER_TRAY_ICON        1
#define TRAY_ICON_OK_MS        1500

// Recovery timers. The notification area is owned by explorer.exe
// it can be restarted (crash, update, logon transition) and every icon in it disappears
// It can also not be ready yet when the Run key launches at logon
// in which case the first NIM_ADD simply fails
// Neither case may leave app running forever without an icon.
// And the code also doesn't allow jxlshot to run two PID in same time
#define TIMER_TRAY_RETRY       2
#define TRAY_RETRY_MS          3000
#define TIMER_TRAY_HEALTH      4
#define TRAY_HEALTH_MS         60000

// Right-clicks are blocked for a short grace period after a region capture
// ends, so the button release that finished the selection cannot leak to the
// desktop. This is a WM_TIMER (see TrayWndProc), not a clock deadline, so the
// low-level mouse hook contains no timing API calls.
#define TIMER_MOUSE_GRACE      3
#define MOUSE_GRACE_MS         500

#define ID_TRAY          1
#define IDM_FULL         101
#define IDM_REGION       102
#define IDM_SETPATH      104
#define IDM_ABOUT        105
#define IDM_DEBUG        106
#define IDM_EXIT         103
#define IDM_OPENCONFIG   107
#define IDM_CHECK_UPDATE 108
#define IDM_OPENEXPORT   109
#define IDM_HOTKEYS      110
#define IDM_GUI          111
#define IDM_CLOSEPREVIEW 112

// Explicitly define the icon resource ID here to prevent "undeclared" errors in CI/CD pipelines
#define IDI_APP_ICON  1001

static NOTIFYICONDATAW g_nid;
static BOOL            g_trayIconAdded = FALSE;
static UINT            g_msgTaskbarCreated = 0;
static HICON           g_iconNormal = NULL; // shared module icon, never destroyed
static HICON           g_iconOk = NULL;     // green-dot variant, owned by us
static HWND            g_hwndTray = NULL;
static HHOOK           g_hhkKeyboard = NULL;
static HHOOK           g_hhkMouse = NULL;
static BOOL            g_isRegionCapturing = FALSE;
static BOOL            g_blockRightClick = FALSE;

// Arm the right-click grace period. Driven by a WM_TIMER on the tray window.
static void begin_mouse_grace(void) {
    if (!g_hwndTray) { g_blockRightClick = FALSE; return; }
    g_blockRightClick = TRUE;
    SetTimer(g_hwndTray, TIMER_MOUSE_GRACE, MOUSE_GRACE_MS, NULL);
}

// Forward declarations to fix implicit declaration errors
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
static void install_mouse_hook(void);
static void uninstall_mouse_hook(void);
static void install_keyboard_hook(void);
static void uninstall_keyboard_hook(void);
static void execute_about(void);
static void execute_gui(void);
static void execute_set_path(void);
static void gui_refresh(void);
static HWND gui_hwnd(void);
static void gui_start_capture(int kind);
static BOOL gui_is_visible(void);
static void gui_preview_show(const Grab *g, const wchar_t *path);
static BOOL preview_close_for_capture(void);
static BOOL gui_preview_is_visible(void);
static void gui_preview_close(void);

// Toggle debug logging from the tray menu. The label always shows the current
// state, and the choice is written back to the INI so it survives a restart.
static void toggle_debug(void) {
    g_cfg.debug_enabled = !g_cfg.debug_enabled;

    wchar_t ini[MAX_PATH];
    _snwprintf(ini, MAX_PATH, L"%s\\jxlshot.ini", g_exe_dir);
    ini[MAX_PATH - 1] = L'\0';
    WritePrivateProfileStringW(L"Capture", L"Debug",
        g_cfg.debug_enabled ? L"1" : L"0", ini);

    if (g_cfg.debug_enabled && !g_dbg) {
        dbg_init(); // open (and truncate) the log for this session
        dbg("debug logging enabled from tray menu");
    }
    // When disabled, g_dbg is deliberately left open: encode/drain worker
    // threads may be inside dbg() right now, and fclose() here would race
    // them. dbg() checks debug_enabled, so nothing is written while OFF.
}

// Tray Icon & Context Menu

static void format_hotkey_string(UINT vk, UINT mod, wchar_t *out, size_t out_size) {
    if (vk == 0) { wcsncpy_s(out, out_size, L"None (Click to set)", _TRUNCATE); return; }
    wchar_t buf[128] = L"";
    if (mod & MOD_CONTROL) wcscat_s(buf, 128, L"Ctrl+");
    if (mod & MOD_SHIFT)   wcscat_s(buf, 128, L"Shift+");
    if (mod & MOD_ALT)     wcscat_s(buf, 128, L"Alt+");
    if (mod & MOD_WIN)     wcscat_s(buf, 128, L"Win+");

    if (vk == VK_SNAPSHOT) wcscat_s(buf, 128, L"PrintScreen");
    else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        wchar_t c[2] = {(wchar_t)vk, L'\0'}; wcscat_s(buf, 128, c);
    }
    else if (vk >= VK_F1 && vk <= VK_F24) {
        size_t l = wcslen(buf);
        _snwprintf(buf + l, 128 - l, L"F%d", (int)(vk - VK_F1 + 1));
    }
    else {
        UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        LONG lparam = (LONG)(sc << 16);
        if ((vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE || vk == VK_DIVIDE)
            lparam |= (1 << 24); // extended key
        wchar_t name[64] = L"";
        if (GetKeyNameTextW(lparam, name, 64) > 0) wcscat_s(buf, 128, name);
        else wcscat_s(buf, 128, L"Key");
    }
    wcsncpy_s(out, out_size, buf, _TRUNCATE);
}

// Hotkey & Settings window (frameless, dark)

#define WM_HOTKEY_CAPTURED (WM_USER + 20)
#define IDC_HK_FULL    101
#define IDC_HK_REGION  102
#define IDC_HK_CLOSE   103

#define IDC_LOSSLESS     204
#define IDC_DIST_DN      205
#define IDC_DIST_UP      206
#define IDC_EFF_DN       207
#define IDC_EFF_UP       208
#define IDC_DISABLE_HOOK 209
#define IDC_SETPATH      210
#define IDC_PREVIEW      211

// Design metrics at 96 DPI. g_hkDpi holds the DPI of the monitor the window is
// on and hk_*() scale these for the actual window.
#define HK_W        300
#define HK_H        500
#define HK_TITLE_H  40
#define HK_Y_HEAD   178
#define HK_Y_LOSS   198
#define HK_Y_DIST   240
#define HK_Y_EFF    282
#define HK_Y_SYS    324
#define HK_Y_EXPORT 344
#define HK_Y_PREVIEW 384
#define HK_Y_HOOK   424
#define HK_ROW_H    34
#define HK_STEP_W   34
#define HK_VAL_W    64

// Close button: a square, flush in the corner, exactly like the Windows 8/10
// title-bar close button.
#define HK_CLOSE_W     40
#define HK_CLOSE_GLYPH 5

static void gui_draw_close_button(HDC dc, const RECT *rc, BOOL hot, BOOL fillIdle,
                                  COLORREF idleBg, COLORREF idleFg, int extent);

static int g_hkDpi = 96;

static int hk_w(void)       { return ui_scale(g_hkDpi, HK_W); }
static int hk_h(void)       { return ui_scale(g_hkDpi, HK_H); }
static int hk_title_h(void) { return ui_scale(g_hkDpi, HK_TITLE_H); }
static int hk_s(int px)     { return ui_scale(g_hkDpi, px); }
static int hk_dn_x(void)    { return hk_w() - hk_s(16 + HK_STEP_W + HK_VAL_W + HK_STEP_W); }
static int hk_up_x(void)    { return hk_w() - hk_s(16 + HK_STEP_W); }

// Same idea as the GUI palette: variables so the settings window can follow the
// sun/moon toggle instead of being permanently dark.
static COLORREF g_hkBg, g_hkBtn, g_hkBtnHot, g_hkBtnDown, g_hkBorder;
static COLORREF g_hkAccent, g_hkText, g_hkDim;

#define HK_BG        (g_hkBg)
#define HK_BTN       (g_hkBtn)
#define HK_BTN_HOT   (g_hkBtnHot)
#define HK_BTN_DOWN  (g_hkBtnDown)
#define HK_BORDER    (g_hkBorder)
#define HK_ACCENT    (g_hkAccent)
#define HK_TEXT      (g_hkText)
#define HK_DIM       (g_hkDim)

static void hk_update_theme(void) {
    if (g_cfg.dark_mode) {
        g_hkBg       = RGB(32, 32, 32);
        g_hkBtn      = RGB(51, 51, 51);
        g_hkBtnHot   = RGB(68, 68, 68);
        g_hkBtnDown  = RGB(85, 85, 85);
        g_hkBorder   = RGB(75, 75, 75);
        g_hkAccent   = RGB(0, 120, 215);
        g_hkText     = RGB(240, 240, 240);
        g_hkDim      = RGB(150, 150, 150);
    } else {
        g_hkBg       = RGB(248, 248, 248);
        g_hkBtn      = RGB(255, 255, 255);
        g_hkBtnHot   = RGB(229, 239, 251);
        g_hkBtnDown  = RGB(204, 220, 240);
        g_hkBorder   = RGB(205, 205, 205);
        g_hkAccent   = RGB(0, 120, 215);
        g_hkText     = RGB(26, 26, 26);
        g_hkDim      = RGB(110, 110, 110);
    }
}

static HWND  g_hwnd_catcher = NULL;
static BOOL  g_hkHiddenForCapture = FALSE; // settings hidden by a running capture
static int   g_catcher_target = 0; // 0 = idle, 1 = full, 2 = region
static volatile BOOL g_hotkeyCapturing = FALSE; // checked by the keyboard hook
static DWORD g_swallowUpVk = 0; // swallow key-up of the captured key
static HFONT g_hkFont = NULL, g_hkFontSmall = NULL;
static HWND  g_hkHoverBtn = NULL;

static BOOL is_modifier_vk(DWORD vk) {
    switch (vk) {
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN:
            return TRUE;
    }
    return FALSE;
}

static UINT current_mods(void) {
    UINT mod = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mod |= MOD_SHIFT;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) mod |= MOD_ALT;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) mod |= MOD_WIN;
    return mod;
}

static void jxl_save_settings(void) {
    wchar_t ini[MAX_PATH], s[32];
    _snwprintf(ini, MAX_PATH, L"%s\\jxlshot.ini", g_exe_dir);
    WritePrivateProfileStringW(L"Capture", L"Lossless", g_cfg.lossless ? L"1" : L"0", ini);
    _snwprintf(s, 32, L"%.1f", g_cfg.distance);
    WritePrivateProfileStringW(L"Capture", L"Distance", s, ini);
    _snwprintf(s, 32, L"%d", g_cfg.effort);
    WritePrivateProfileStringW(L"Capture", L"Effort", s, ini);
    WritePrivateProfileStringW(L"Capture", L"ShowPreview", g_cfg.show_preview ? L"1" : L"0", ini);
    dbg("settings changed: effort=%d lossless=%d distance=%.2f",
        g_cfg.effort, g_cfg.lossless, g_cfg.distance);
    gui_refresh(); // repaint the main window's status line / capture hints
}

static void hk_refresh_buttons(HWND hwnd) {
    wchar_t hk[128], txt[192];
    
    // Full Screen Hotkey
    if (g_catcher_target == 1) wcscpy_s(txt, 192, L"Full Screen:  press a key...");
    else {
        format_hotkey_string(g_cfg.hk_full_vk, g_cfg.hk_full_mod, hk, 128);
        _snwprintf(txt, 192, L"Full Screen:  %s", hk); txt[191] = 0;
    }
    SetWindowTextW(GetDlgItem(hwnd, IDC_HK_FULL), txt);

    // Region Hotkey
    if (g_catcher_target == 2) wcscpy_s(txt, 192, L"Region:  press a key...");
    else {
        format_hotkey_string(g_cfg.hk_region_vk, g_cfg.hk_region_mod, hk, 128);
        _snwprintf(txt, 192, L"Region:  %s", hk); txt[191] = 0;
    }
    SetWindowTextW(GetDlgItem(hwnd, IDC_HK_REGION), txt);


    SetWindowTextW(GetDlgItem(hwnd, IDC_LOSSLESS),
                   g_cfg.lossless ? L"Lossless:  ON" : L"Lossless:  OFF (lossy)");
    SetWindowTextW(GetDlgItem(hwnd, IDC_SETPATH), L"Set Export Folder...");
    SetWindowTextW(GetDlgItem(hwnd, IDC_PREVIEW),
                   g_cfg.show_preview ? L"Capture Preview:  ON" : L"Capture Preview:  OFF");
    SetWindowTextW(GetDlgItem(hwnd, IDC_DIST_DN), L"\u2212");
    SetWindowTextW(GetDlgItem(hwnd, IDC_DIST_UP), L"+");
    SetWindowTextW(GetDlgItem(hwnd, IDC_EFF_DN),  L"\u2212");
    SetWindowTextW(GetDlgItem(hwnd, IDC_EFF_UP),  L"+");
    EnableWindow(GetDlgItem(hwnd, IDC_DIST_DN), !g_cfg.lossless);
    EnableWindow(GetDlgItem(hwnd, IDC_DIST_UP), !g_cfg.lossless);
    

    SetWindowTextW(GetDlgItem(hwnd, IDC_DISABLE_HOOK),
                   g_cfg.disable_keyboard_hook ? L"Keyboard Hook:  Disabled" : L"Keyboard Hook:  Enabled");


    {
        int ids[] = { IDC_LOSSLESS, IDC_SETPATH, IDC_PREVIEW, IDC_DIST_DN, IDC_DIST_UP, IDC_EFF_DN, IDC_EFF_UP, IDC_DISABLE_HOOK };
        for (int i = 0; i < 8; i++) {
            InvalidateRect(GetDlgItem(hwnd, ids[i]), NULL, FALSE);
        }
    }

    InvalidateRect(GetDlgItem(hwnd, IDC_HK_FULL), NULL, FALSE);
    InvalidateRect(GetDlgItem(hwnd, IDC_HK_REGION), NULL, FALSE);
    InvalidateRect(hwnd, NULL, FALSE);
}

static void hk_begin_capture(HWND hwnd, int target) {
    g_catcher_target = target;
    g_swallowUpVk = 0;
    g_hotkeyCapturing = TRUE; // hook now stops firing captures and routes keys here
    hk_refresh_buttons(hwnd);
    SetFocus(hwnd); // so Space/Enter can't "click" the button
}

static void hk_end_capture(HWND hwnd) {
    g_catcher_target = 0;
    g_hotkeyCapturing = FALSE;
    hk_refresh_buttons(hwnd);
}

static void hk_draw_button(LPDRAWITEMSTRUCT di) {
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    int id = (int)di->CtlID;
    BOOL hot = (g_hkHoverBtn == di->hwndItem);
    BOOL down = (di->itemState & ODS_SELECTED) != 0;
    BOOL dis = (di->itemState & ODS_DISABLED) != 0;
    BOOL active = (id == IDC_HK_FULL && g_catcher_target == 1) ||
                  (id == IDC_HK_REGION && g_catcher_target == 2) ||
                  (id == IDC_LOSSLESS && g_cfg.lossless) ||
                  (id == IDC_PREVIEW && g_cfg.show_preview) ||
                  (id == IDC_DISABLE_HOOK && g_cfg.disable_keyboard_hook == 0);

    // The settings close button is drawn by the exact same code as the strip's
    // close button: a square idle box in the window colour, theme glyph, red on
    // hover, like the Windows 8/10 title-bar button.
    if (id == IDC_HK_CLOSE) {
        gui_draw_close_button(dc, &rc, hot, TRUE, HK_BG, HK_TEXT,
                              hk_s(HK_CLOSE_GLYPH));
        return;
    }

    COLORREF bg = dis ? HK_BTN : (down ? HK_BTN_DOWN : (hot ? HK_BTN_HOT : HK_BTN));

    HBRUSH b = CreateSolidBrush(bg);
    FillRect(dc, &rc, b);
    DeleteObject(b);

    HBRUSH fb = CreateSolidBrush(active ? HK_ACCENT : HK_BORDER);
    FrameRect(dc, &rc, fb);
    if (active) { InflateRect(&rc, -1, -1); FrameRect(dc, &rc, fb); InflateRect(&rc, 1, 1); }
    DeleteObject(fb);

    wchar_t txt[192];
    GetWindowTextW(di->hwndItem, txt, 192);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, dis ? HK_DIM : (active ? HK_ACCENT : HK_TEXT));
    HFONT of = (HFONT)SelectObject(dc, g_hkFont);
    DrawTextW(dc, txt, -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
    SelectObject(dc, of);
}

static LRESULT CALLBACK HkBtnSubclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    (void)ref;
    switch (m) {
        case WM_MOUSEMOVE:
            if (g_hkHoverBtn != h) {
                g_hkHoverBtn = h;
                TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
                TrackMouseEvent(&t);
                InvalidateRect(h, NULL, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            if (g_hkHoverBtn == h) g_hkHoverBtn = NULL;
            InvalidateRect(h, NULL, FALSE);
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(h, HkBtnSubclass, id);
            break;
    }
    return DefSubclassProc(h, m, w, l);
}

static void hk_save(int target, UINT vk, UINT mod) {
    LPCWSTR vk_key  = (target == 1) ? L"HotkeyFullVK"  : L"HotkeyRegionVK";
    LPCWSTR mod_key = (target == 1) ? L"HotkeyFullMod" : L"HotkeyRegionMod";
    if (target == 1) { g_cfg.hk_full_vk = vk;   g_cfg.hk_full_mod = mod; }
    else             { g_cfg.hk_region_vk = vk; g_cfg.hk_region_mod = mod; }

    wchar_t ini_path[MAX_PATH], vk_str[32], mod_str[32];
    _snwprintf(ini_path, MAX_PATH, L"%s\\jxlshot.ini", g_exe_dir);
    _snwprintf(vk_str, 32, L"%u", vk);
    _snwprintf(mod_str, 32, L"%u", mod);
    WritePrivateProfileStringW(L"Capture", vk_key, vk_str, ini_path);
    WritePrivateProfileStringW(L"Capture", mod_key, mod_str, ini_path);
    gui_refresh(); // the capture cards show the new hotkey
}

static void hk_create_fonts(void) {
    if (g_hkFont)      { DeleteObject(g_hkFont); g_hkFont = NULL; }
    if (g_hkFontSmall) { DeleteObject(g_hkFontSmall); g_hkFontSmall = NULL; }

    g_hkFont = CreateFontW(-hk_s(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_hkFontSmall = CreateFontW(-hk_s(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

// Everything in the Settings window is laid out from the 96-DPI design values
// in HK_* so the window follows the Windows display-scaling setting.
static void hk_layout_children(HWND hwnd) {
    int m  = hk_s(16);
    int cw = hk_s(HK_CLOSE_W);
    int th = hk_title_h();
    int bw = hk_w() - 2 * m;
    int row = hk_s(HK_ROW_H);
    int step = hk_s(HK_STEP_W);

    struct { int id; int x, y, w, h; } b[] = {
        { IDC_HK_FULL,      m, hk_s(48),  bw, hk_s(38) },
        { IDC_HK_REGION,    m, hk_s(94),  bw, hk_s(38) },
        { IDC_HK_CLOSE,     hk_w() - cw, 0, cw, th },
        { IDC_LOSSLESS,     m, hk_s(HK_Y_LOSS), bw, row },
        { IDC_DIST_DN,      hk_dn_x(), hk_s(HK_Y_DIST), step, row },
        { IDC_DIST_UP,      hk_up_x(), hk_s(HK_Y_DIST), step, row },
        { IDC_EFF_DN,       hk_dn_x(), hk_s(HK_Y_EFF),  step, row },
        { IDC_EFF_UP,       hk_up_x(), hk_s(HK_Y_EFF),  step, row },
        { IDC_SETPATH,      m, hk_s(HK_Y_EXPORT),  bw, row },
        { IDC_PREVIEW,      m, hk_s(HK_Y_PREVIEW), bw, row },
        { IDC_DISABLE_HOOK, m, hk_s(HK_Y_HOOK),    bw, row },
    };

    for (int i = 0; i < (int)(sizeof(b) / sizeof(b[0])); i++) {
        HWND hb = GetDlgItem(hwnd, b[i].id);
        if (hb) SetWindowPos(hb, NULL, b[i].x, b[i].y, b[i].w, b[i].h,
                             SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static LRESULT CALLBACK CatcherWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            if (g_hkDpi < 48) g_hkDpi = ui_dpi_for_window(hwnd);
            hk_update_theme();
            hk_create_fonts();

            HINSTANCE hi = GetModuleHandle(NULL);
            static const int ids[] = {
                IDC_HK_FULL, IDC_HK_REGION, IDC_HK_CLOSE, IDC_LOSSLESS,
                IDC_DIST_DN, IDC_DIST_UP, IDC_EFF_DN, IDC_EFF_UP,
                IDC_SETPATH, IDC_PREVIEW, IDC_DISABLE_HOOK
            };
            for (int i = 0; i < (int)(sizeof(ids) / sizeof(ids[0])); i++) {
                HWND hb = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                          0, 0, 10, 10, hwnd,
                                          (HMENU)(INT_PTR)ids[i], hi, NULL);
                SetWindowSubclass(hb, HkBtnSubclass, (UINT_PTR)ids[i], 0);
            }
            hk_layout_children(hwnd);
            hk_refresh_buttons(hwnd);
            return 0;
        }
        case WM_ERASEBKGND: return 1;

        case WM_DPICHANGED: {
            g_hkDpi = (int)LOWORD(wParam);
            if (g_hkDpi < 48) g_hkDpi = ui_dpi_for_window(hwnd);
            RECT *pr = (RECT *)lParam;
            if (pr) {
                SetWindowPos(hwnd, NULL, pr->left, pr->top,
                             pr->right - pr->left, pr->bottom - pr->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            hk_create_fonts();
            hk_layout_children(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            HBRUSH bg = CreateSolidBrush(HK_BG); FillRect(dc, &rc, bg); DeleteObject(bg);
            HBRUSH bd = CreateSolidBrush(HK_BORDER); FrameRect(dc, &rc, bd); DeleteObject(bd);

            int W = hk_w(), H = hk_h();
            int m = hk_s(16);
            int row = hk_s(HK_ROW_H);
            int yHead = hk_s(HK_Y_HEAD);
            int yDist = hk_s(HK_Y_DIST);
            int yEff  = hk_s(HK_Y_EFF);
            int ySys  = hk_s(HK_Y_SYS);
            int dnX = hk_dn_x(), upX = hk_up_x(), step = hk_s(HK_STEP_W);

            SetBkMode(dc, TRANSPARENT);
            HFONT of = (HFONT)SelectObject(dc, g_hkFont);
            SetTextColor(dc, HK_TEXT);
            RECT t = { m, 0, W - hk_s(48), hk_title_h() };
            DrawTextW(dc, L"Settings", -1, &t, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

            SelectObject(dc, g_hkFontSmall);
            SetTextColor(dc, HK_DIM);
            RECT h = { m, hk_s(140), W - m, hk_s(172) };
            DrawTextW(dc, g_catcher_target
                ? L"Press the new key combo. Esc cancels."
                : L"Click a button, then press the new key combo.",
                -1, &h, DT_WORDBREAK | DT_LEFT);

            SelectObject(dc, g_hkFontSmall);
            SetTextColor(dc, HK_DIM);
            RECT hd = { m, yHead, W - m, yHead + hk_s(16) };
            DrawTextW(dc, L"JPEG XL", -1, &hd, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

            SelectObject(dc, g_hkFont);
            wchar_t vbuf[32];
            COLORREF dist_col = g_cfg.lossless ? HK_DIM : HK_TEXT;

            RECT lr = { m, yDist, dnX - hk_s(4), yDist + row };
            SetTextColor(dc, dist_col);
            DrawTextW(dc, L"Distance", -1, &lr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
            RECT vr = { dnX + step, yDist, upX, yDist + row };
            _snwprintf(vbuf, 32, L"%.1f", g_cfg.distance);
            DrawTextW(dc, vbuf, -1, &vr, DT_SINGLELINE | DT_VCENTER | DT_CENTER);

            lr.top = yEff; lr.bottom = yEff + row;
            SetTextColor(dc, HK_TEXT);
            DrawTextW(dc, L"Effort", -1, &lr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
            vr.top = yEff; vr.bottom = yEff + row;
            _snwprintf(vbuf, 32, L"%d", g_cfg.effort);
            DrawTextW(dc, vbuf, -1, &vr, DT_SINGLELINE | DT_VCENTER | DT_CENTER);

            SelectObject(dc, g_hkFontSmall);
            SetTextColor(dc, HK_DIM);
            RECT hs = { m, ySys, W - m, ySys + hk_s(16) };
            DrawTextW(dc, L"System", -1, &hs, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

            // Current export folder, right-aligned in the section header.
            {
                const wchar_t *slash = wcsrchr(g_cfg.export_path, L'\\');
                const wchar_t *name = (slash && slash[1]) ? slash + 1 : g_cfg.export_path;
                if (name[0]) {
                    RECT er = { W / 2, ySys, W - m, ySys + hk_s(16) };
                    DrawTextW(dc, name, -1, &er,
                              DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_END_ELLIPSIS);
                }
            }


            RECT info = { m, H - hk_s(35), W - m, H - hk_s(5) };
            SIZE sz;

            SetTextColor(dc, HK_DIM);
            DrawTextW(dc, L"Open ", -1, &info, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
            GetTextExtentPoint32W(dc, L"Open ", 5, &sz);
            info.left += sz.cx;

            SetTextColor(dc, HK_ACCENT);
            DrawTextW(dc, L".ini", -1, &info, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
            GetTextExtentPoint32W(dc, L".ini", 4, &sz);
            info.left += sz.cx;

            SetTextColor(dc, HK_DIM);
            DrawTextW(dc, L" file for more infos", -1, &info, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

            SelectObject(dc, of);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SETCURSOR: {
            // Change cursor to hand when hovering over the bottom info area
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            RECT info = { hk_s(16), hk_h() - hk_s(35),
                          hk_w() - hk_s(16), hk_h() - hk_s(5) };
            if (pt.x >= info.left && pt.x <= info.right && pt.y >= info.top && pt.y <= info.bottom) {
                SetCursor(LoadCursor(NULL, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            // Check if the user clicked anywhere in the bottom info area
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            RECT info = { hk_s(16), hk_h() - hk_s(35),
                          hk_w() - hk_s(16), hk_h() - hk_s(5) };
            
            if (x >= info.left && x <= info.right && y >= info.top && y <= info.bottom) {
                // Trigger the existing IDM_OPENCONFIG command in the main tray window
                PostMessageW(g_hwndTray, WM_COMMAND, MAKEWPARAM(IDM_OPENCONFIG, 0), 0);
            }
            break;
        }
        case WM_NCHITTEST: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            if (pt.y >= 0 && pt.y < hk_title_h() &&
                pt.x < hk_w() - hk_s(HK_CLOSE_W)) return HTCAPTION; // drag area
            return HTCLIENT;
        }
        case WM_DRAWITEM:
            hk_draw_button((LPDRAWITEMSTRUCT)lParam);
            return TRUE;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_HK_FULL:   hk_begin_capture(hwnd, 1); break;
                case IDC_HK_REGION: hk_begin_capture(hwnd, 2); break;
                case IDC_HK_CLOSE:  DestroyWindow(hwnd); break;
                case IDC_LOSSLESS:
                    g_cfg.lossless = !g_cfg.lossless;
                    jxl_save_settings(); hk_refresh_buttons(hwnd); break;
                case IDC_DIST_DN:
                    g_cfg.distance -= 0.5f;
                    if (g_cfg.distance < 0.5f) g_cfg.distance = 0.5f;
                    jxl_save_settings(); hk_refresh_buttons(hwnd); break;
                case IDC_DIST_UP:
                    g_cfg.distance += 0.5f;
                    if (g_cfg.distance > 25.0f) g_cfg.distance = 25.0f;
                    jxl_save_settings(); hk_refresh_buttons(hwnd); break;
                case IDC_EFF_DN:
                    if (g_cfg.effort > 1)  g_cfg.effort--;
                    jxl_save_settings(); hk_refresh_buttons(hwnd); break;

                case IDC_EFF_UP:
                    if (g_cfg.effort < 10) g_cfg.effort++;
                    jxl_save_settings(); hk_refresh_buttons(hwnd); break;

                case IDC_SETPATH:
                    // Folder picker lives here now; the main strip only opens
                    // the current export folder.
                    execute_set_path();
                    hk_refresh_buttons(hwnd);
                    break;

                case IDC_PREVIEW:
                    g_cfg.show_preview = !g_cfg.show_preview;
                    jxl_save_settings();
                    hk_refresh_buttons(hwnd);
                    if (!g_cfg.show_preview) gui_preview_close();
                    dbg("settings changed: show_preview=%d", g_cfg.show_preview);
                    break;

                case IDC_DISABLE_HOOK:
                    g_cfg.disable_keyboard_hook = !g_cfg.disable_keyboard_hook;                    {
                        wchar_t ini[MAX_PATH], s[32];
                        _snwprintf(ini, MAX_PATH, L"%s\\jxlshot.ini", g_exe_dir);
                        _snwprintf(s, 32, L"%d", g_cfg.disable_keyboard_hook);
                        WritePrivateProfileStringW(L"Capture", L"DisableKeyboardHook", s, ini);
                        dbg("settings changed: disable_keyboard_hook=%d", g_cfg.disable_keyboard_hook);
                    }
                    // Dynamically apply the change immediately
                    if (g_cfg.disable_keyboard_hook) {
                        uninstall_keyboard_hook();
                    } else {
                        install_keyboard_hook();
                    }
                    hk_refresh_buttons(hwnd);
                    break;
            }
            return 0;
        case WM_HOTKEY_CAPTURED: { // posted by the keyboard hook
            if (!g_catcher_target) return 0;
            UINT vk = (UINT)wParam, mod = (UINT)lParam;
            if (vk == VK_ESCAPE && mod == 0) { hk_end_capture(hwnd); return 0; }
            hk_save(g_catcher_target, vk, mod);
            hk_end_capture(hwnd);
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && g_hotkeyCapturing) hk_end_capture(hwnd);
            return 0;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY:
            g_hotkeyCapturing = FALSE;
            g_catcher_target = 0;
            g_hkHoverBtn = NULL;
            if (g_hkFont)      { DeleteObject(g_hkFont); g_hkFont = NULL; }
            if (g_hkFontSmall) { DeleteObject(g_hkFontSmall); g_hkFontSmall = NULL; }
            g_hwnd_catcher = NULL;
            g_hkHiddenForCapture = FALSE;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Position the settings window beside the main strip like "PotPlayer style",
// instead of covering it.
// Falls back to the old centred position when the strip is closed (settings opened from the tray menu).
static void position_settings_window(HWND h) {
    if (!h) return;

    int x, y;
    BOOL placed = FALSE;

    HWND anchor = gui_hwnd();
    if (anchor && IsWindowVisible(anchor)) {
        RECT ar;
        GetWindowRect(anchor, &ar);

        HMONITOR mon = MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(mon, &mi)) {
            int w = hk_w(), h = hk_h();

            // Prefer the right-hand side, then the left, then clamp on-screen.
            x = ar.right + hk_s(8);
            if (x + w > mi.rcWork.right) {
                x = ar.left - w - hk_s(8);
                if (x < mi.rcWork.left) x = mi.rcWork.left + hk_s(8);
                if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
            }

            y = ar.top;
            if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
            if (y < mi.rcWork.top) y = mi.rcWork.top;

            placed = TRUE;
        }
    }

    if (!placed) {
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        int w = hk_w(), h = hk_h();
        x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
        y = mi.rcWork.top  + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    }

    SetWindowPos(h, NULL, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// The Settings window must never appear in a screenshot. Any capture path hides
// it before grabbing and brings it back when the capture sequence ends (or when
// the preview closes, whichever is later).
static BOOL settings_hide_for_capture(void) {
    if (!g_hwnd_catcher || !IsWindow(g_hwnd_catcher)) return FALSE;
    if (!IsWindowVisible(g_hwnd_catcher)) return FALSE;

    ShowWindow(g_hwnd_catcher, SW_HIDE);
    g_hkHiddenForCapture = TRUE;
    return TRUE;
}

static void settings_restore_after_capture(void) {
    if (!g_hkHiddenForCapture) return;
    g_hkHiddenForCapture = FALSE;

    if (g_hwnd_catcher && IsWindow(g_hwnd_catcher)) {
        ShowWindow(g_hwnd_catcher, SW_SHOW);
        // Only take focus when the strip is not coming back as well, so the
        // window that started the capture stays in front.
        if (!gui_is_visible()) SetForegroundWindow(g_hwnd_catcher);
    }
}

// The preview is the window the user is looking at after a capture, so the
// Settings window only returns once the preview is gone (PreviewWndProc calls
// settings_restore_after_capture on WM_DESTROY).
static void settings_finish_capture(void) {
    if (gui_preview_is_visible()) return;
    settings_restore_after_capture();
}

// Toggle: the same Settings button (tray menu or strip) hides an open window.
static void execute_hotkey_settings(HWND hwnd) {
    (void)hwnd; // message-only tray window can't be an owner
    if (g_hwnd_catcher && IsWindow(g_hwnd_catcher)) {
        if (IsWindowVisible(g_hwnd_catcher)) {
            ShowWindow(g_hwnd_catcher, SW_HIDE);
            // A manual hide wins over a pending capture restore.
            g_hkHiddenForCapture = FALSE;
        } else {
            position_settings_window(g_hwnd_catcher);
            ShowWindow(g_hwnd_catcher, SW_SHOW);
            SetForegroundWindow(g_hwnd_catcher);
            // It is visible again, so a capture finishing later must not
            // re-show (or re-position) it as a "restore".
            g_hkHiddenForCapture = FALSE;
        }
        return;
    }

    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = CatcherWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"JxlShotCatcherClass";
        RegisterClassExW(&wc);
        registered = TRUE;
    }

    // Pick the DPI of the monitor the window will open on, so the first paint
    // is already at the right scale (WM_CREATE runs before the window is shown).
    HMONITOR target_mon = MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
    HWND anchor = gui_hwnd();
    if (anchor && IsWindowVisible(anchor))
        target_mon = MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST);
    g_hkDpi = ui_dpi_for_monitor(target_mon);

    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    int w = hk_w(), h = hk_h();
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
    int y = mi.rcWork.top  + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;

    g_hwnd_catcher = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"JxlShotCatcherClass",
        L"Settings", WS_POPUP, x, y, w, h, NULL, NULL, GetModuleHandle(NULL), NULL);

    if (g_hwnd_catcher) {
        // PotPlayer style: open beside the main strip, never on top of it.
        position_settings_window(g_hwnd_catcher);
        ApplyDarkMode(g_hwnd_catcher);
        ShowWindow(g_hwnd_catcher, SW_SHOW);
        SetForegroundWindow(g_hwnd_catcher);
    }
}

static void show_tray_menu(HWND hwnd) {
    POINT pt; GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_GUI, L"Open Window...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_FULL, L"Capture Full Screen");
    AppendMenuW(hMenu, MF_STRING, IDM_REGION, L"Capture Region...");
    if (gui_preview_is_visible()) {
        AppendMenuW(hMenu, MF_STRING, IDM_CLOSEPREVIEW, L"Close Preview");
    }
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_SETPATH, L"Set Export Path...");
    AppendMenuW(hMenu, MF_STRING, IDM_OPENEXPORT, L"Open Export Folder");
    AppendMenuW(hMenu, MF_STRING, IDM_OPENCONFIG, L"Open Config File");
    AppendMenuW(hMenu, MF_STRING, IDM_HOTKEYS, L"Settings...");
    wchar_t debug_label[32];
    _snwprintf(debug_label, 32, L"Debug: %s", g_cfg.debug_enabled ? L"ON" : L"OFF");
    AppendMenuW(hMenu, MF_STRING, IDM_DEBUG, debug_label);
    AppendMenuW(hMenu, MF_STRING, IDM_CHECK_UPDATE, L"Open Releases Page");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_ABOUT, L"About...");
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");
    
    // Force dark mode on the transient menu window before displaying it
    if (g_hwndMenuOwner) {
        ApplyDarkMode(g_hwndMenuOwner);
    }
    
    // Bring the hidden menu owner to the foreground so the menu inherits its theme
    HWND prevFg = GetForegroundWindow();
    if (g_hwndMenuOwner) {
        SetForegroundWindow(g_hwndMenuOwner);
    } else {
        SetForegroundWindow(hwnd);
    }
    
    // TPM_RETURNCMD: post the command after the menu is gone and run it from
    // the normal message loop, like the hotkey. Handled inside TrackPopupMenu
    // the screen grab froze the popup that had just been clicked.
    UINT cmd = (UINT)TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0,
                                    g_hwndMenuOwner ? g_hwndMenuOwner : hwnd, NULL);
    DestroyMenu(hMenu);
    PostMessage(hwnd, WM_NULL, 0, 0);

    // Hand the foreground back. The owner is only a menu host, and leaving it
    // front-most is the one thing the menu path does that the hotkey does not.
    if (prevFg && prevFg != g_hwndMenuOwner && IsWindow(prevFg)) SetForegroundWindow(prevFg);

    if (cmd) PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
}

// Hand a captured frame to the encoder. On success the worker owns the pixels
// and g->bits is cleared; on failure the frame is consumed here. Returns 1 if
// the frame was accepted (threaded or synchronous fallback).
static int queue_grab_encode(Grab *g, const wchar_t *path) {
    if (!g || !g->bits || !path) return 0;
    // OFFLOAD TO BACKGROUND THREAD
    // Thread owns the buffer now
    EncodeTask *task = (EncodeTask *)malloc(sizeof(EncodeTask));
    if (task) {
        task->bits = g->bits;
        g->bits = NULL;
        task->w = g->w;
        task->h = g->h;
        task->is_hdr = g->is_hdr;
        task->lossless = g_cfg.lossless;
        task->distance = g_cfg.distance;
        wcsncpy_s(task->out_path, MAX_PATH, path, _TRUNCATE);

        InterlockedIncrement(&g_activeEncodes);
        HANDLE hThread = CreateThread(NULL, 0, EncodeWorker, task, 0, NULL);
        if (hThread) {
            CloseHandle(hThread);
            dbg("queue_grab_encode: encoding offloaded (%ls)", path);
            return 1;
        }

        InterlockedDecrement(&g_activeEncodes);
        dbg("queue_grab_encode: CreateThread failed, saving synchronously");
        int ok = save_rgb_as_jxl(task->bits, task->w, task->h, task->is_hdr,
                                task->lossless, task->distance, task->out_path);
        free(task->bits);
        free(task);
        return ok;
    }

    dbg("queue_grab_encode: malloc failed, saving synchronously");
    int ok = save_rgb_as_jxl(g->bits, g->w, g->h, g->is_hdr,
                            g_cfg.lossless, g_cfg.distance, path);
    free_grab(g);
    return ok;
}

static void execute_full_capture(void) {
    // Never let the previous preview or the Settings window end up inside the
    // new screenshot. When this was the only delay (tray/hotkey path, no GUI
    // timer), give the compositor a moment to remove them before grabbing.
    BOOL hadPreview = preview_close_for_capture();
    BOOL hadSettings = settings_hide_for_capture();
    if (hadPreview || hadSettings) Sleep(120);

    dbg("execute_full_capture: begin (effort=%d, multimonitor=%s)",
        g_cfg.effort, g_cfg.multimonitor ? "separateimages" : "focusmonitor");

    if (g_cfg.multimonitor == 1) {
        MonitorEntry monitors[16];
        int count = enumerate_monitors(monitors, 16);

        if (count <= 0) {
            MessageBoxW(NULL, L"No monitors found.", L"jxlshot", MB_ICONERROR);
            settings_finish_capture();
            return;
        }

        int saved = 0;
        for (int i = 0; i < count; i++) {
            Grab g;
            ZeroMemory(&g, sizeof(g));
            if (!grab_monitor(&g, monitors[i].hmon)) {
                dbg("execute_full_capture: monitor %d grab failed", i + 1);
                free_grab(&g);
                continue;
            }

            wchar_t tag[16];
            _snwprintf(tag, 16, L"_m%d", i + 1);
            tag[15] = 0;

            wchar_t out_path[MAX_PATH];
            build_out_path_tag(out_path, MAX_PATH, g.is_hdr, tag);
            dbg("execute_full_capture: monitor %d -> %ls", i + 1, out_path);

            // Show one preview (the first monitor that produced a frame).
            // Separate-images mode still saves every monitor.
            if (saved == 0) gui_preview_show(&g, out_path);

            if (queue_grab_encode(&g, out_path)) saved++;
            free_grab(&g);
        }

        if (saved == 0) {
            MessageBoxW(NULL, L"Screen capture failed.", L"jxlshot", MB_ICONERROR);
        } else if (saved < count) {
            dbg("execute_full_capture: only %d of %d monitors saved", saved, count);
        }
        settings_finish_capture();
        return;
    }

    Grab g;
    ZeroMemory(&g, sizeof(g));
    if (!grab_monitor(&g, monitor_under_cursor())) {
        dbg("execute_full_capture: grab failed, showing MessageBox");
        MessageBoxW(NULL, L"Screen capture failed.", L"jxlshot", MB_ICONERROR);
        settings_finish_capture();
        return;
    }

    wchar_t out_path[MAX_PATH];
    build_out_path(out_path, MAX_PATH, g.is_hdr);
    dbg("execute_full_capture: writing to %ls", out_path);

    // Show the Snipping Tool style preview of the frame before handing the
    // pixels to the background encoder.
    gui_preview_show(&g, out_path);

    if (!queue_grab_encode(&g, out_path)) {
        MessageBoxW(NULL, L"Encoding or saving failed.", L"jxlshot", MB_ICONERROR);
    }
    free_grab(&g);

    settings_finish_capture();
}

static void execute_open_releases(HWND hwnd) {
    // No version download and no temp file: let the browser do the work.
    ShellExecuteW(hwnd, L"open",
                  L"https://github.com/azizr12/jxlscrenshot/releases",
                  NULL, NULL, SW_SHOWNORMAL);
}

static void execute_set_path(void) {
    BROWSEINFOW bi = { 0 };
    // Own the picker by the Settings window when it is open: that window is
    // topmost, so an ownerless dialog would open behind it.
    bi.hwndOwner = g_hwnd_catcher;
    bi.lpszTitle = L"Select Export Folder for Screenshots";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) {
            wcsncpy(g_cfg.export_path, path, MAX_PATH - 1); g_cfg.export_path[MAX_PATH - 1] = L'\0';
            wchar_t ini_path[MAX_PATH]; _snwprintf(ini_path, MAX_PATH, L"%s\\jxlshot.ini", g_exe_dir);
            WritePrivateProfileStringW(L"Capture", L"ExportPath", path, ini_path);
            MessageBoxW(g_hwnd_catcher, L"Export path updated.", L"jxlshot", MB_ICONINFORMATION);
        }
        CoTaskMemFree(pidl);
    }
}

static void execute_open_config(HWND hwnd) {
    wchar_t ini_path[MAX_PATH];
    _snwprintf(ini_path, MAX_PATH, L"%s\\jxlshot.ini", g_exe_dir);
    
    // Attempt 1: Explicitly open with Notepad
    HINSTANCE hResult = ShellExecuteW(hwnd, L"open", L"notepad.exe", ini_path, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)hResult > 32) {
        return; // Success
    }
    
    // Attempt 2: Fallback to default .txt/.ini viewer
    hResult = ShellExecuteW(hwnd, L"open", ini_path, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)hResult > 32) {
        return; // Success
    }
    
    // Attempt 3: Fallback to simply opening the root folder containing the INI
    ShellExecuteW(hwnd, L"explore", g_exe_dir, NULL, NULL, SW_SHOWNORMAL);
}

static void execute_open_export_folder(void) {
    wchar_t path_to_open[MAX_PATH];
    
    // Use the configured export path, or fall back to the executable directory
    if (g_cfg.export_path[0] != L'\0') {
        wcsncpy_s(path_to_open, MAX_PATH, g_cfg.export_path, _TRUNCATE);
    } else {
        wcsncpy_s(path_to_open, MAX_PATH, g_exe_dir, _TRUNCATE);
    }
    
    // Open the folder in Windows Explorer
    ShellExecuteW(NULL, L"explore", path_to_open, NULL, NULL, SW_SHOWNORMAL);
}

// Interactive Region Selection

static HWND    g_hwndRegion = NULL;
static HDC     g_hdcMem = NULL;   // untouched screenshot
static HDC     g_hdcDim = NULL;   // pre-dimmed screenshot (built once)
static HDC     g_hdcBack = NULL;  // off-screen frame, presented in one blit
static HDC     g_measureDC = NULL;
static HBITMAP g_hbmScreen = NULL, g_hbmDim = NULL, g_hbmBack = NULL;
static uint8_t *g_screenBits = NULL; // pixels of the frame shown under the overlay
static Grab     g_regionFrame;        // one captured monitor frame: overlay + crop
static RECT     g_regionMonitorRect;  // screen coordinates of that monitor
static HFONT   g_dimFont = NULL; // Arial label for the live W × H readout
static int     g_dimTextH = 16;  // label height, cached when the font is built
static int     g_dimOutline = 1; // label outline thickness, scaled by DPI
static int     g_screenW, g_screenH;
static RECT    g_rcSel;
static BOOL    g_isDragging = FALSE;

static HBITMAP create_screen_dib(HDC dc, void **bits) {
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_screenW;
    bi.bmiHeader.biHeight = -g_screenH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(dc, &bi, DIB_RGB_COLORS, bits, NULL, 0);
}

// Display-only conversion from linear scRGB to 8-bit sRGB. The saved region
// keeps the original FP16 scRGB pixels; this only builds the overlay preview.
static uint8_t linear_to_srgb8(float v) {
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;

    float s = (v <= 0.0031308f)
        ? 12.92f * v
        : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;

    int n = (int)(s * 255.0f + 0.5f);
    if (n < 0) n = 0;
    if (n > 255) n = 255;
    return (uint8_t)n;
}

static uint8_t g_hdr_srgb_lut[65536];
static BOOL    g_hdr_srgb_lut_ready = FALSE;

static void init_hdr_srgb_lut(void) {
    for (uint32_t i = 0; i < 65536; i++) {
        g_hdr_srgb_lut[i] = linear_to_srgb8(half_to_float((uint16_t)i));
    }
    g_hdr_srgb_lut_ready = TRUE;
}

// Build the 32-bit overlay DIB from the single captured frame. SDR is copied
// as-is; HDR is converted for the preview only.
static void build_overlay_from_region_frame(void) {
    if (!g_screenBits || !g_regionFrame.bits) return;

    const int w = g_screenW;
    const int h = g_screenH;
    const size_t src_bpp = g_regionFrame.is_hdr ? 6 : 3;
    const uint8_t *src = g_regionFrame.bits;
    uint8_t *dst = g_screenBits;

    if (g_regionFrame.is_hdr) {
        if (!g_hdr_srgb_lut_ready) init_hdr_srgb_lut();

        for (size_t i = 0, n = (size_t)w * h; i < n; i++, src += src_bpp, dst += 4) {
            uint16_t hr = (uint16_t)(src[0] | (src[1] << 8));
            uint16_t hg = (uint16_t)(src[2] | (src[3] << 8));
            uint16_t hb = (uint16_t)(src[4] | (src[5] << 8));

            dst[0] = g_hdr_srgb_lut[hb];
            dst[1] = g_hdr_srgb_lut[hg];
            dst[2] = g_hdr_srgb_lut[hr];
            dst[3] = 255;
        }
    } else {
        for (size_t i = 0, n = (size_t)w * h; i < n; i++, src += src_bpp, dst += 4) {
            dst[0] = src[2]; // B
            dst[1] = src[1]; // G
            dst[2] = src[0]; // R
            dst[3] = 255;
        }
    }
}

// Normalized selection rectangle (drag direction does not matter).
// Mouse coordinates are pixels: dragging from 0 to 1919 covers 1920 pixels,
// so a non-zero span must include its last row/column. A click without a drag
// still has zero width and height, so it still cancels the capture.

static void region_sel_rect(RECT *r) {
    int x = min(g_rcSel.left, g_rcSel.right);
    int y = min(g_rcSel.top, g_rcSel.bottom);
    int w = abs(g_rcSel.right - g_rcSel.left);
    int h = abs(g_rcSel.bottom - g_rcSel.top);
    if (w > 0) w++;
    if (h > 0) h++;
    SetRect(r, x, y, x + w, y + h);
}

static BOOL region_has_selection(void) {
    RECT r;
    region_sel_rect(&r);
    return g_isDragging || !IsRectEmpty(&r);
}

// Bounds of the "1234 × 567" label, matching exactly where WM_PAINT draws it.
static void region_label_rect(const RECT *sel, RECT *out) {
    int w = sel->right - sel->left;
    int h = sel->bottom - sel->top;

    wchar_t text[64];
    int len = _snwprintf(text, 64, L"%d × %d", w, h);
    if (len < 0) len = 0;

    SIZE sz = {0};
    if (g_measureDC) {
        HFONT font = g_dimFont ? g_dimFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HGDIOBJ old = SelectObject(g_measureDC, font);
        GetTextExtentPoint32W(g_measureDC, text, len, &sz);
        SelectObject(g_measureDC, old);
    }

    int textX = sel->left + 8;
    int textY = (sel->top - g_dimTextH - 8) < 0 ? sel->top + 8 : sel->top - g_dimTextH - 8;

    SetRect(out,
            textX - g_dimOutline,
            textY - g_dimOutline,
            textX + sz.cx + g_dimOutline + 2,
            textY + g_dimTextH + g_dimOutline + 2);
}

// Everything that has to be repainted when the selection changes.
static void region_dirty_rect(RECT *out) {
    RECT sel, label;
    region_sel_rect(&sel);
    region_label_rect(&sel, &label);
    UnionRect(out, &sel, &label);
    InflateRect(out, 4, 4); // 2 px border plus a little slack
}


// GPU region overlay (D3D11 flip-model swap chain, HDR and SDR)

// GDI windows are 8-bit SDR.
// therefore flattened the captured scRGB FP16 frame into sRGB,
// so the preview looked SDR even though the saved crop was still HDR.
// The frame is now presented through a real swap chain instead
// scRGB FP16 on HDR monitors,
// and B8G8R8A8 on SDR monitors.
// The dim layer, selection border and size label are drawn by a tiny shader,
// so the HDR path keeps highlights above SDR white.

// The same renderer is used on SDR monitors on purpose
// it makes the GPU path testable on any display,
// and SDR output is bit-identical in intent to the old GDI preview.
// Any machine where the D3D setup fails keeps using the original GDI overlay.
// The saved region always comes from g_regionFrame,
// so the file is unaffected by which preview path is used.

#include <d3dcompiler.h>
// pD3DCompile typedef; the DLL is loaded at runtime

#define GPU_OVERLAY_DIM        0.53f   // dim factor outside the selection
#define GPU_OVERLAY_BORDER     2.0f    // accent border width, in physical pixels

typedef struct {
    float sel[4]; // left, top, right, bottom (pixels)
    float screen[2];
    float dim;
    float border;
    float label_pos[2];
    float label_size[2];
    // border colour in the render target's colour space, plus the exponent
    // used to lift the 8-bit sRGB label into that space (1.0 for SDR).
    float border_color[4]; // r, g, b, label gamma
} GpuOverlayCB;

typedef struct {
    ID3D11Device             *device;
    ID3D11DeviceContext      *ctx;
    IDXGISwapChain3          *swap;
    ID3D11RenderTargetView   *rtv;
    ID3D11Texture2D          *src_tex;
    ID3D11ShaderResourceView *src_srv;
    ID3D11Texture2D          *label_tex;
    ID3D11ShaderResourceView *label_srv;
    ID3D11VertexShader       *vs;
    ID3D11PixelShader        *ps;
    ID3D11Buffer             *cb;
    ID3D11SamplerState       *samp;
    int w, h;
    int is_hdr;
    int label_w, label_h;
    int label_x, label_y;
    HDC label_dc;
    HBITMAP label_bmp;
    void *label_bits;
} GpuOverlay;

static GpuOverlay g_gpu;
static BOOL       g_gpuOverlayActive = FALSE;

static const char k_gpu_overlay_shader[] =
    "cbuffer P : register(b0) {\n"
    "  float4 sel; float2 scr; float dim; float bw;\n"
    "  float2 lpos; float2 lsize; float4 bc;\n"
    "};\n"
    "Texture2D<float4> Src : register(t0);\n"
    "Texture2D<float4> Lbl : register(t1);\n"
    "SamplerState Samp : register(s0);\n"
    "struct V { float4 pos : SV_Position; };\n"
    "V VS(uint id : SV_VertexID) {\n"
    "  V o;\n"
    // Covers the viewport and must stay clockwise on the render target
    // the default rasterizer state culls counter-clockwise faces,
    // and a mirrored triangle is silently dropped, leaving the whole overlay black.
    "  o.pos = float4((id == 1) ? 3.0 : -1.0, (id == 2) ? -3.0 : 1.0, 0.0, 1.0);\n"
    "  return o;\n"
    "}\n"
    "float4 PS(V i) : SV_Target {\n"
    "  float2 p = i.pos.xy;\n"
    // SV_Position is already the pixel centre, so p/scr and (p-lpos)/lsize hit
    // texel centres exactly for a 1:1 blit; no half-pixel correction needed.
    "  float3 c = Src.SampleLevel(Samp, p / scr, 0.0).rgb;\n"
    "  bool inside = (p.x >= sel.x && p.x < sel.z && p.y >= sel.y && p.y < sel.w);\n"
    "  if (!inside) c *= dim;\n"
    "  if (lsize.x > 0.0) {\n"
    "    float2 uv = (p - lpos) / lsize;\n"
    "    if (uv.x >= 0.0 && uv.x < 1.0 && uv.y >= 0.0 && uv.y < 1.0) {\n"
    "      float4 t = Lbl.SampleLevel(Samp, uv, 0.0);\n"
    "      t.rgb = pow(max(t.rgb, 0.0), bc.w);\n"
    "      c = c * (1.0 - t.a) + t.rgb * t.a;\n"
    "    }\n"
    "  }\n"
    "  if (inside && bw > 0.0) {\n"
    "    if (p.y < sel.y + bw || p.y >= sel.w - bw ||\n"
    "        p.x < sel.x + bw || p.x >= sel.z - bw)\n"
    "      c = bc.rgb;\n"
    "  }\n"
    "  return float4(c, 1.0);\n"
    "}\n";

static ID3DBlob *gpu_overlay_compile(const char *entry, const char *target) {
    static HMODULE       compiler = NULL;
    static pD3DCompile   compile_fn = NULL;
    static int           tried = 0;

    if (!tried) {
        tried = 1;
        compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        if (compiler) {
            compile_fn = (pD3DCompile)(void *)GetProcAddress(compiler, "D3DCompile");
        }
    }
    if (!compile_fn) {
        dbg("gpu overlay: D3DCompile unavailable, keeping SDR preview");
        return NULL;
    }

    ID3DBlob *code = NULL;
    ID3DBlob *err = NULL;
    HRESULT hr = compile_fn(k_gpu_overlay_shader, sizeof k_gpu_overlay_shader - 1,
                            NULL, NULL, NULL, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        dbg("gpu overlay: shader %s compile failed (0x%08lX) %s", entry, hr,
            err ? (const char *)err->lpVtbl->GetBufferPointer(err) : "");
        if (err) err->lpVtbl->Release(err);
        if (code) code->lpVtbl->Release(code);
        return NULL;
    }
    if (err) err->lpVtbl->Release(err);
    return code;
}

static void gpu_overlay_destroy(void) {
    g_gpuOverlayActive = FALSE;

    if (g_gpu.ctx) g_gpu.ctx->lpVtbl->ClearState(g_gpu.ctx);
    if (g_gpu.rtv)       g_gpu.rtv->lpVtbl->Release(g_gpu.rtv);
    if (g_gpu.src_srv)   g_gpu.src_srv->lpVtbl->Release(g_gpu.src_srv);
    if (g_gpu.src_tex)   g_gpu.src_tex->lpVtbl->Release(g_gpu.src_tex);
    if (g_gpu.label_srv) g_gpu.label_srv->lpVtbl->Release(g_gpu.label_srv);
    if (g_gpu.label_tex) g_gpu.label_tex->lpVtbl->Release(g_gpu.label_tex);
    if (g_gpu.samp)      g_gpu.samp->lpVtbl->Release(g_gpu.samp);
    if (g_gpu.cb)        g_gpu.cb->lpVtbl->Release(g_gpu.cb);
    if (g_gpu.vs)        g_gpu.vs->lpVtbl->Release(g_gpu.vs);
    if (g_gpu.ps)        g_gpu.ps->lpVtbl->Release(g_gpu.ps);
    if (g_gpu.swap)      g_gpu.swap->lpVtbl->Release(g_gpu.swap);
    if (g_gpu.ctx)       g_gpu.ctx->lpVtbl->Release(g_gpu.ctx);
    if (g_gpu.device)    g_gpu.device->lpVtbl->Release(g_gpu.device);

    if (g_gpu.label_dc)  { DeleteDC(g_gpu.label_dc); g_gpu.label_dc = NULL; }
    if (g_gpu.label_bmp) { DeleteObject(g_gpu.label_bmp); g_gpu.label_bmp = NULL; }

    ZeroMemory(&g_gpu, sizeof g_gpu);
}

// Upload the captured frame as a 4-channel texture.
// HDR is RGB16F scRGB (6 bytes/px) and becomes RGBA16F with A = 1.0h;
// SDR is RGB24 and becomes BGRA8 with A = 255.
static int gpu_overlay_upload_source(void) {
    if (!g_gpu.device || !g_gpu.ctx || !g_regionFrame.bits ||
        g_regionFrame.w <= 0 || g_regionFrame.h <= 0) {
        return 0;
    }

    D3D11_TEXTURE2D_DESC td;
    ZeroMemory(&td, sizeof td);
    td.Width = (UINT)g_regionFrame.w;
    td.Height = (UINT)g_regionFrame.h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = g_gpu.is_hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT
                             : DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(g_gpu.device->lpVtbl->CreateTexture2D(g_gpu.device, &td, NULL, &g_gpu.src_tex))) {
        dbg("gpu overlay: CreateTexture2D(source) failed");
        return 0;
    }

    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g_gpu.ctx->lpVtbl->Map(g_gpu.ctx, (ID3D11Resource *)g_gpu.src_tex,
                                      0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        dbg("gpu overlay: Map(source) failed");
        return 0;
    }

    if (g_gpu.is_hdr) {
        const uint16_t *src = (const uint16_t *)g_regionFrame.bits;
        for (int y = 0; y < g_regionFrame.h; y++) {
            uint16_t *dst = (uint16_t *)((uint8_t *)map.pData + (size_t)y * map.RowPitch);
            for (int x = 0; x < g_regionFrame.w; x++) {
                dst[0] = src[0];   // R
                dst[1] = src[1];   // G
                dst[2] = src[2];   // B
                dst[3] = 0x3C00;   // A = 1.0h
                dst += 4;
                src += 3;
            }
        }
    } else {
        const uint8_t *src = (const uint8_t *)g_regionFrame.bits;
        for (int y = 0; y < g_regionFrame.h; y++) {
            uint8_t *dst = (uint8_t *)map.pData + (size_t)y * map.RowPitch;
            for (int x = 0; x < g_regionFrame.w; x++) {
                dst[0] = src[2];   // B
                dst[1] = src[1];   // G
                dst[2] = src[0];   // R
                dst[3] = 255;      // A
                dst += 4;
                src += 3;
            }
        }
    }
    g_gpu.ctx->lpVtbl->Unmap(g_gpu.ctx, (ID3D11Resource *)g_gpu.src_tex, 0);

    D3D11_SHADER_RESOURCE_VIEW_DESC svd;
    ZeroMemory(&svd, sizeof svd);
    svd.Format = td.Format;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    if (FAILED(g_gpu.device->lpVtbl->CreateShaderResourceView(
            g_gpu.device, (ID3D11Resource *)g_gpu.src_tex, &svd, &g_gpu.src_srv))) {
        dbg("gpu overlay: CreateShaderResourceView(source) failed");
        return 0;
    }
    return 1;
}

static int gpu_overlay_create_label_texture(int w, int h, const void *bits) {
    if (g_gpu.label_srv) { g_gpu.label_srv->lpVtbl->Release(g_gpu.label_srv); g_gpu.label_srv = NULL; }
    if (g_gpu.label_tex) { g_gpu.label_tex->lpVtbl->Release(g_gpu.label_tex); g_gpu.label_tex = NULL; }

    D3D11_TEXTURE2D_DESC td;
    ZeroMemory(&td, sizeof td);
    td.Width = (UINT)(w > 0 ? w : 1);
    td.Height = (UINT)(h > 0 ? h : 1);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init;
    ZeroMemory(&init, sizeof init);
    init.pSysMem = bits;
    init.SysMemPitch = (UINT)td.Width * 4;

    if (FAILED(g_gpu.device->lpVtbl->CreateTexture2D(g_gpu.device, &td,
                                                     bits ? &init : NULL, &g_gpu.label_tex))) {
        dbg("gpu overlay: CreateTexture2D(label) failed");
        return 0;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC svd;
    ZeroMemory(&svd, sizeof svd);
    svd.Format = td.Format;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    if (FAILED(g_gpu.device->lpVtbl->CreateShaderResourceView(
            g_gpu.device, (ID3D11Resource *)g_gpu.label_tex, &svd, &g_gpu.label_srv))) {
        dbg("gpu overlay: CreateShaderResourceView(label) failed");
        return 0;
    }
    return 1;
}

// Render the "W x H" readout into a small BGRA DIB (dark box + white text)
// and upload it.
// GDI leaves the alpha bytes alone,
// so the box alpha can be seeded in memory before TextOutW draws into it.
static void gpu_overlay_update_label(const RECT *sel) {
    if (!g_gpu.device || !g_gpu.ctx || !sel) return;

    int textW = (int)(sel->right - sel->left);
    int textH = (int)(sel->bottom - sel->top);

    wchar_t text[64];
    int len = _snwprintf(text, 64, L"%d × %d", textW, textH);
    if (len < 0) len = 0;

    HFONT font = g_dimFont ? g_dimFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ oldFont = SelectObject(g_measureDC, font);

    SIZE sz = { 0 };
    GetTextExtentPoint32W(g_measureDC, text, len, &sz);
    TEXTMETRICW tm;
    ZeroMemory(&tm, sizeof tm);
    GetTextMetricsW(g_measureDC, &tm);
    SelectObject(g_measureDC, oldFont);

    const int pad = 5;
    int w = sz.cx + pad * 2;
    int h = (tm.tmHeight > 0 ? tm.tmHeight : 16) + pad * 2;
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    int recreated = 0;
    if (w != g_gpu.label_w || h != g_gpu.label_h || !g_gpu.label_bits) {
        if (g_gpu.label_dc)  { DeleteDC(g_gpu.label_dc); g_gpu.label_dc = NULL; }
        if (g_gpu.label_bmp) { DeleteObject(g_gpu.label_bmp); g_gpu.label_bmp = NULL; }

        HDC screen = GetDC(NULL);
        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof bi);
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void *bits = NULL;
        g_gpu.label_bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        g_gpu.label_dc = CreateCompatibleDC(screen);
        ReleaseDC(NULL, screen);

        if (!g_gpu.label_bmp || !g_gpu.label_dc || !bits) {
            dbg("gpu overlay: label DIB creation failed");
            return;
        }

        SelectObject(g_gpu.label_dc, g_gpu.label_bmp);
        g_gpu.label_bits = bits;
        g_gpu.label_w = w;
        g_gpu.label_h = h;
        recreated = 1;
    }

    uint8_t *px = (uint8_t *)g_gpu.label_bits;
    size_t npix = (size_t)w * h;
    for (size_t i = 0; i < npix; i++, px += 4) {
        px[0] = 28;  // B
        px[1] = 28;  // G
        px[2] = 28;  // R
        px[3] = 215; // A
    }

    SelectObject(g_gpu.label_dc, font);
    SetBkMode(g_gpu.label_dc, TRANSPARENT);
    SetTextColor(g_gpu.label_dc, RGB(255, 255, 255));
    TextOutW(g_gpu.label_dc, pad, pad, text, len);

    // GDI writes the glyph RGB but leaves the alpha bytes undefined,
    // so the readout came out hollow/haloed once the texture was blended.
    // Keep the box translucent and make every inked pixel fully opaque;
    // antialiased edges keep their grey in RGB,
    // so they still blend smoothly against the frame.
    px = (uint8_t *)g_gpu.label_bits;
    for (size_t i = 0; i < npix; i++, px += 4) {
        int ink = px[0] + px[1] + px[2];
        px[3] = (ink > 28 * 3 + 2) ? 255 : 215;
    }

    if (recreated) {
        if (!gpu_overlay_create_label_texture(w, h, g_gpu.label_bits)) return;
    } else {
        g_gpu.ctx->lpVtbl->UpdateSubresource(g_gpu.ctx, (ID3D11Resource *)g_gpu.label_tex,
                                             0, NULL, g_gpu.label_bits, (UINT)w * 4, 0);
    }

    int textX = sel->left + 8;
    int textY = (sel->top - (tm.tmHeight > 0 ? tm.tmHeight : 16) - 8) < 0
                ? sel->top + 8
                : sel->top - (tm.tmHeight > 0 ? tm.tmHeight : 16) - 8;
    g_gpu.label_x = textX - pad;
    g_gpu.label_y = textY - pad;
}

static void gpu_overlay_present(void) {
    if (!g_gpuOverlayActive || !g_gpu.ctx || !g_gpu.swap) return;

    RECT sel;
    region_sel_rect(&sel);
    BOOL has_sel = region_has_selection() && !IsRectEmpty(&sel);

    GpuOverlayCB cb;
    ZeroMemory(&cb, sizeof cb);
    cb.screen[0] = (float)g_gpu.w;
    cb.screen[1] = (float)g_gpu.h;
    cb.dim = GPU_OVERLAY_DIM;
    cb.border = GPU_OVERLAY_BORDER;
    // UI colours are authored in sRGB. The SDR target is sRGB, so they pass
    // through unchanged; scRGB is linear, so the border and the label both need
    // the sRGB -> linear lift.
    if (g_gpu.is_hdr) {
        cb.border_color[0] = 0.0f;
        cb.border_color[1] = 0.187f;   // sRGB(0, 120, 215) in linear scRGB
        cb.border_color[2] = 0.673f;
        cb.border_color[3] = 2.2f;
    } else {
        cb.border_color[0] = 0.0f;
        cb.border_color[1] = 120.0f / 255.0f;
        cb.border_color[2] = 215.0f / 255.0f;
        cb.border_color[3] = 1.0f;
    }

    if (has_sel) {
        gpu_overlay_update_label(&sel);
        cb.sel[0] = (float)sel.left;
        cb.sel[1] = (float)sel.top;
        cb.sel[2] = (float)sel.right;
        cb.sel[3] = (float)sel.bottom;
        cb.label_pos[0] = (float)g_gpu.label_x;
        cb.label_pos[1] = (float)g_gpu.label_y;
        cb.label_size[0] = (float)g_gpu.label_w;
        cb.label_size[1] = (float)g_gpu.label_h;
    }

    // Constant buffers created with D3D11_USAGE_DYNAMIC must be written with
    // Map/Unmap; UpdateSubresource is not valid for them
    // (the runtime leaves the buffer undefined, which made the overlay render solid black).
    D3D11_MAPPED_SUBRESOURCE cbmap;
    if (SUCCEEDED(g_gpu.ctx->lpVtbl->Map(g_gpu.ctx, (ID3D11Resource *)g_gpu.cb,
                                          0, D3D11_MAP_WRITE_DISCARD, 0, &cbmap))) {
        memcpy(cbmap.pData, &cb, sizeof cb);
        g_gpu.ctx->lpVtbl->Unmap(g_gpu.ctx, (ID3D11Resource *)g_gpu.cb, 0);
    } else {
        dbg("gpu overlay: Map(constant buffer) failed");
    }

    D3D11_VIEWPORT vp;
    ZeroMemory(&vp, sizeof vp);
    vp.Width = (float)g_gpu.w;
    vp.Height = (float)g_gpu.h;
    vp.MaxDepth = 1.0f;

    ID3D11ShaderResourceView *srvs[2] = { g_gpu.src_srv, g_gpu.label_srv };
    g_gpu.ctx->lpVtbl->OMSetRenderTargets(g_gpu.ctx, 1, &g_gpu.rtv, NULL);
    g_gpu.ctx->lpVtbl->RSSetViewports(g_gpu.ctx, 1, &vp);
    g_gpu.ctx->lpVtbl->IASetPrimitiveTopology(g_gpu.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_gpu.ctx->lpVtbl->VSSetShader(g_gpu.ctx, g_gpu.vs, NULL, 0);
    g_gpu.ctx->lpVtbl->PSSetShader(g_gpu.ctx, g_gpu.ps, NULL, 0);
    g_gpu.ctx->lpVtbl->PSSetConstantBuffers(g_gpu.ctx, 0, 1, &g_gpu.cb);
    g_gpu.ctx->lpVtbl->PSSetShaderResources(g_gpu.ctx, 0, 2, srvs);
    g_gpu.ctx->lpVtbl->PSSetSamplers(g_gpu.ctx, 0, 1, &g_gpu.samp);
    g_gpu.ctx->lpVtbl->Draw(g_gpu.ctx, 3, 0);

    HRESULT hr = g_gpu.swap->lpVtbl->Present(g_gpu.swap, 1, 0);
    if (FAILED(hr)) {
        dbg("gpu overlay: Present failed hr=0x%08lX", hr);
    }
}

static int gpu_overlay_init(HWND hwnd, HMONITOR monitor, int w, int h, int is_hdr) {
    ZeroMemory(&g_gpu, sizeof g_gpu);
    g_gpu.is_hdr = is_hdr;

    IDXGIFactory1 *factory1 = NULL;
    IDXGIFactory2 *factory2 = NULL;
    IDXGIAdapter1 *adapter = NULL;
    ID3DBlob *vs_blob = NULL;
    ID3DBlob *ps_blob = NULL;
    IDXGISwapChain1 *swap1 = NULL;
    ID3D11Texture2D *backbuffer = NULL;
    int ok = 0;

    // Find the adapter that drives this monitor while staying compatible with
    // Windows 7: CreateDXGIFactory2 is Windows 8.1+, so QI for it instead of
    // importing it statically. QI simply fails on older systems -> GDI path.
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory1))) {
        dbg("gpu overlay: CreateDXGIFactory1 failed");
        goto done;
    }
    if (FAILED(factory1->lpVtbl->QueryInterface(factory1, &IID_IDXGIFactory2, (void **)&factory2))) {
        dbg("gpu overlay: IDXGIFactory2 unavailable");
        goto done;
    }

    for (UINT ai = 0; !adapter; ai++) {
        IDXGIAdapter1 *cand = NULL;
        if (factory1->lpVtbl->EnumAdapters1(factory1, ai, &cand) == DXGI_ERROR_NOT_FOUND) break;
        if (!cand) continue;

        for (UINT oi = 0; ; oi++) {
            IDXGIOutput *out = NULL;
            if (cand->lpVtbl->EnumOutputs(cand, oi, &out) == DXGI_ERROR_NOT_FOUND) break;
            if (!out) continue;

            DXGI_OUTPUT_DESC od;
            if (SUCCEEDED(out->lpVtbl->GetDesc(out, &od)) && od.Monitor == monitor) {
                adapter = cand;
                out->lpVtbl->Release(out);
                break;
            }
            out->lpVtbl->Release(out);
        }
        if (!adapter) cand->lpVtbl->Release(cand);
    }
    if (!adapter) {
        dbg("gpu overlay: no adapter for monitor");
        goto done;
    }

    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL got;
    if (FAILED(D3D11CreateDevice((IDXGIAdapter *)adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3,
                                 D3D11_SDK_VERSION, &g_gpu.device, &got, &g_gpu.ctx))) {
        dbg("gpu overlay: D3D11CreateDevice failed");
        goto done;
    }

    vs_blob = gpu_overlay_compile("VS", "vs_4_0");
    ps_blob = gpu_overlay_compile("PS", "ps_4_0");
    if (!vs_blob || !ps_blob) goto done;
    if (FAILED(g_gpu.device->lpVtbl->CreateVertexShader(g_gpu.device,
            vs_blob->lpVtbl->GetBufferPointer(vs_blob),
            vs_blob->lpVtbl->GetBufferSize(vs_blob), NULL, &g_gpu.vs))) {
        dbg("gpu overlay: CreateVertexShader failed");
        goto done;
    }
    if (FAILED(g_gpu.device->lpVtbl->CreatePixelShader(g_gpu.device,
            ps_blob->lpVtbl->GetBufferPointer(ps_blob),
            ps_blob->lpVtbl->GetBufferSize(ps_blob), NULL, &g_gpu.ps))) {
        dbg("gpu overlay: CreatePixelShader failed");
        goto done;
    }
    vs_blob->lpVtbl->Release(vs_blob); vs_blob = NULL;
    ps_blob->lpVtbl->Release(ps_blob); ps_blob = NULL;

    DXGI_SWAP_CHAIN_DESC1 sd;
    ZeroMemory(&sd, sizeof sd);
    sd.Width = (UINT)w;
    sd.Height = (UINT)h;
    sd.Format = is_hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    if (FAILED(factory2->lpVtbl->CreateSwapChainForHwnd(
            factory2, (IUnknown *)g_gpu.device, hwnd, &sd, NULL, NULL, &swap1))) {
        dbg("gpu overlay: CreateSwapChainForHwnd failed");
        goto done;
    }
    if (FAILED(swap1->lpVtbl->QueryInterface(swap1, &IID_IDXGISwapChain3, (void **)&g_gpu.swap))) {
        dbg("gpu overlay: IDXGISwapChain3 unavailable");
        goto done;
    }
    factory2->lpVtbl->MakeWindowAssociation(factory2, hwnd, DXGI_MWA_NO_ALT_ENTER);

    if (is_hdr) {
        // scRGB: linear, full-range, sRGB/Rec.709 primaries. This is the same
        // space the desktop duplication frame is captured in, so values pass
        // through unchanged and >1.0 highlights stay HDR.
        if (FAILED(g_gpu.swap->lpVtbl->SetColorSpace1(
                g_gpu.swap, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709))) {
            dbg("gpu overlay: SetColorSpace1(scRGB) failed");
            goto done;
        }
    } else {
        // sRGB is the default for a B8G8R8A8 flip chain; request it explicitly
        // but do not abort if the driver refuses an otherwise working chain.
        g_gpu.swap->lpVtbl->SetColorSpace1(
            g_gpu.swap, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    }

    if (FAILED(g_gpu.swap->lpVtbl->GetBuffer(g_gpu.swap, 0,
            &IID_ID3D11Texture2D, (void **)&backbuffer))) {
        dbg("gpu overlay: GetBuffer failed");
        goto done;
    }
    if (FAILED(g_gpu.device->lpVtbl->CreateRenderTargetView(
            g_gpu.device, (ID3D11Resource *)backbuffer, NULL, &g_gpu.rtv))) {
        dbg("gpu overlay: CreateRenderTargetView failed");
        goto done;
    }

    D3D11_BUFFER_DESC bd;
    ZeroMemory(&bd, sizeof bd);
    bd.ByteWidth = sizeof(GpuOverlayCB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_gpu.device->lpVtbl->CreateBuffer(g_gpu.device, &bd, NULL, &g_gpu.cb))) {
        dbg("gpu overlay: CreateBuffer(cb) failed");
        goto done;
    }

    D3D11_SAMPLER_DESC smd;
    ZeroMemory(&smd, sizeof smd);
    smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    smd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(g_gpu.device->lpVtbl->CreateSamplerState(g_gpu.device, &smd, &g_gpu.samp))) {
        dbg("gpu overlay: CreateSamplerState failed");
        goto done;
    }

    if (!gpu_overlay_upload_source()) goto done;

    // A 1x1 transparent label keeps the shader's second texture bound even
    // before a selection exists.
    {
        uint32_t transparent = 0;
        if (!gpu_overlay_create_label_texture(1, 1, &transparent)) goto done;
    }

    g_gpu.w = w;
    g_gpu.h = h;
    g_gpuOverlayActive = TRUE;
    ok = 1;

done:
    if (backbuffer) backbuffer->lpVtbl->Release(backbuffer);
    if (swap1) swap1->lpVtbl->Release(swap1);
    if (vs_blob) vs_blob->lpVtbl->Release(vs_blob);
    if (ps_blob) ps_blob->lpVtbl->Release(ps_blob);
    if (factory2) factory2->lpVtbl->Release(factory2);
    if (factory1) factory1->lpVtbl->Release(factory1);
    if (adapter) adapter->lpVtbl->Release(adapter);

    if (!ok) gpu_overlay_destroy();
    return ok;
}

static void crop_and_encode_region(RECT *r) {
    if (!g_regionFrame.bits || g_regionFrame.w <= 0 || g_regionFrame.h <= 0) {
        dbg("crop_and_encode_region: no captured frame");
        return;// Early exit is safe
    }

    dbg("crop_and_encode_region: using displayed frame (effort=%d, %dx%d, %s)",
        g_cfg.effort, r->right - r->left, r->bottom - r->top,
        g_regionFrame.is_hdr ? "HDR" : "SDR");

    if (r->left   < 0)                r->left   = 0;
    if (r->top    < 0)                r->top    = 0;
    if (r->right  > g_regionFrame.w)  r->right  = g_regionFrame.w;
    if (r->bottom > g_regionFrame.h)  r->bottom = g_regionFrame.h;

    int rw = r->right - r->left;
    int rh = r->bottom - r->top;
    if (rw <= 0 || rh <= 0) return;

    Grab crop;
    if (!crop_grab(&g_regionFrame, r->left, r->top, rw, rh, &crop)) {
        dbg("crop_and_encode_region: crop failed");
        return;
    }

    wchar_t out_path[MAX_PATH];
    build_out_path(out_path, MAX_PATH, crop.is_hdr);

    // Preview the exact crop the user selected, in the same HDR/SDR format
    // that will be encoded.
    gui_preview_show(&crop, out_path);

    if (!queue_grab_encode(&crop, out_path)) {
        dbg("crop_and_encode_region: save failed");
    }
    free_grab(&crop);// Guaranteed cleanup before early exit
}

LRESULT CALLBACK RegionWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1; // fully repainted in WM_PAINT; never erase first

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            if (g_gpuOverlayActive) {
                gpu_overlay_present();
                EndPaint(hwnd, &ps);
                return 0;
            }

            RECT dirty = ps.rcPaint;
            int dx = dirty.left;
            int dy = dirty.top;
            int dw = dirty.right - dirty.left;
            int dh = dirty.bottom - dirty.top;

            if (dw > 0 && dh > 0) {
                // 1. Start from the screenshot that was dimmed once at capture time.
                BitBlt(g_hdcBack, dx, dy, dw, dh, g_hdcDim, dx, dy, SRCCOPY);

                if (region_has_selection()) {
                    RECT sel;
                    region_sel_rect(&sel);

                    // 2. Restore the clear (non-dimmed) image inside the selection.
                    RECT inter;
                    if (IntersectRect(&inter, &sel, &dirty)) {
                        BitBlt(g_hdcBack, inter.left, inter.top,
                               inter.right - inter.left, inter.bottom - inter.top,
                               g_hdcMem, inter.left, inter.top, SRCCOPY);
                    }

                    int saved = SaveDC(g_hdcBack);
                    IntersectClipRect(g_hdcBack, dx, dy, dx + dw, dy + dh);

                    // 3. Accent border.
                    HPEN hPen = CreatePen(PS_SOLID, 2, RGB(0, 120, 215));
                    HPEN hOldPen = (HPEN)SelectObject(g_hdcBack, hPen);
                    HBRUSH hOldBrush = (HBRUSH)SelectObject(g_hdcBack, GetStockObject(NULL_BRUSH));
                    Rectangle(g_hdcBack, sel.left, sel.top, sel.right, sel.bottom);
                    SelectObject(g_hdcBack, hOldBrush);
                    SelectObject(g_hdcBack, hOldPen);
                    DeleteObject(hPen);

                    // 4. Dimension label: white glyphs with a dark outline.
                    SetBkMode(g_hdcBack, TRANSPARENT);

                    wchar_t dimText[64];
                    _snwprintf(dimText, 64, L"%d × %d",
                               sel.right - sel.left, sel.bottom - sel.top);
                    int dimLen = (int)wcslen(dimText);

                    HFONT oldFont = (HFONT)SelectObject(g_hdcBack,
                        g_dimFont ? g_dimFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT));

                    TEXTMETRICW tm;
                    GetTextMetricsW(g_hdcBack, &tm);

                    int textY = (sel.top - tm.tmHeight - 8) < 0
                                ? sel.top + 8
                                : sel.top - tm.tmHeight - 8;
                    int textX = sel.left + 8;

                    // Draw the dark border around the glyphs, then the white core.
                    SetTextColor(g_hdcBack, RGB(0, 0, 0));
                    for (int oy = -g_dimOutline; oy <= g_dimOutline; oy++) {
                        for (int ox = -g_dimOutline; ox <= g_dimOutline; ox++) {
                            if (ox || oy) TextOutW(g_hdcBack, textX + ox, textY + oy, dimText, dimLen);
                        }
                    }
                    SetTextColor(g_hdcBack, RGB(255, 255, 255));
                    TextOutW(g_hdcBack, textX, textY, dimText, dimLen);

                    SelectObject(g_hdcBack, oldFont);
                    RestoreDC(g_hdcBack, saved);
                }

                // 5. Present the finished frame in one blit, so no half-drawn
                //    frame (dimmed or undimmed) can ever reach the screen.
                BitBlt(hdc, dx, dy, dw, dh, g_hdcBack, dx, dy, SRCCOPY);
            }

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            g_isDragging = TRUE; 
            g_rcSel.left = GET_X_LPARAM(lp); 
            g_rcSel.top = GET_Y_LPARAM(lp);
            g_rcSel.right = g_rcSel.left; 
            g_rcSel.bottom = g_rcSel.top;
            SetCapture(hwnd);

            if (g_gpuOverlayActive) {
                InvalidateRect(hwnd, NULL, FALSE);
            } else {
                RECT dirty;
                region_dirty_rect(&dirty);
                InvalidateRect(hwnd, &dirty, FALSE);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (g_isDragging) {
                int nx = GET_X_LPARAM(lp);
                int ny = GET_Y_LPARAM(lp);
                if (nx == g_rcSel.right && ny == g_rcSel.bottom) return 0;

                if (g_gpuOverlayActive) {
                    // The D3D path presents the whole frame anyway;
                    // A full invalidation also covers the label box exactly.
                    g_rcSel.right = nx;
                    g_rcSel.bottom = ny;
                    InvalidateRect(hwnd, NULL, FALSE);
                } else {
                    // Repaint only where the old and new selection (plus label) were.
                    RECT oldDirty, newDirty, dirty;
                    region_dirty_rect(&oldDirty);
                    g_rcSel.right = nx;
                    g_rcSel.bottom = ny;
                    region_dirty_rect(&newDirty);
                    UnionRect(&dirty, &oldDirty, &newDirty);
                    InvalidateRect(hwnd, &dirty, FALSE);
                }
            } 
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_isDragging) {
                g_isDragging = FALSE; 
                ReleaseCapture();
        
                RECT r;
                region_sel_rect(&r);
        
                ShowWindow(hwnd, SW_HIDE);

                // Crop the frame the user actually saw before WM_DESTROY frees
                // the overlay's DIB. Do not grab the screen a second time here:
                // that frame can differ from the one under the selection.
                if (!IsRectEmpty(&r)) {
                    crop_and_encode_region(&r);
                }

                DestroyWindow(hwnd);
                
                // Force desktop to repaint immediately to clear any ghosting artifacts
                RedrawWindow(NULL, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE);
            } 
            return 0;
        }
        case WM_KEYDOWN: {
            if (wp == VK_ESCAPE) {
                g_isDragging = FALSE; 
                ReleaseCapture();
        
                ShowWindow(hwnd, SW_HIDE);
                DestroyWindow(hwnd);
                
                // Force desktop to repaint immediately to clear any ghosting artifacts
                RedrawWindow(NULL, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE);
            } 
            return 0;
        }
        
        /* Right-Click Cancellation */
        case WM_RBUTTONDOWN: {
            // Right-click cancels the selection, exactly like the Escape key
            g_isDragging = FALSE; 
            ReleaseCapture();
    
            ShowWindow(hwnd, SW_HIDE);
            DestroyWindow(hwnd);
            
            // Force desktop to repaint immediately to clear any ghosting artifacts
            RedrawWindow(NULL, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE);
            return 0;
        }
        
        case WM_DESTROY: {
            g_isRegionCapturing = FALSE;
            begin_mouse_grace(); // Start the 500ms block grace period
// DO NOT call uninstall_mouse_hook() here anymore!
            gpu_overlay_destroy();
            if (g_hdcBack)  { DeleteDC(g_hdcBack); g_hdcBack = NULL; }
            if (g_hbmBack)  { DeleteObject(g_hbmBack); g_hbmBack = NULL; }
            if (g_hdcDim)   { DeleteDC(g_hdcDim); g_hdcDim = NULL; }
            if (g_hbmDim)   { DeleteObject(g_hbmDim); g_hbmDim = NULL; }
            if (g_hdcMem)   { DeleteDC(g_hdcMem); g_hdcMem = NULL; }
            if (g_hbmScreen){ DeleteObject(g_hbmScreen); g_hbmScreen = NULL; }
            g_screenBits = NULL;
            free_grab(&g_regionFrame);
            if (g_measureDC){ DeleteDC(g_measureDC); g_measureDC = NULL; }
            if (g_dimFont)  { DeleteObject(g_dimFont); g_dimFont = NULL; }
            
            // Completely clear all region selection state so the app remembers nothing
            g_hwndRegion = NULL;
            g_isDragging = FALSE;
            g_regionMonitorRect.left = g_regionMonitorRect.top = 0;
            g_regionMonitorRect.right = g_regionMonitorRect.bottom = 0;
            g_rcSel.left = g_rcSel.top = g_rcSel.right = g_rcSel.bottom = 0;

            // Bring the Settings window back unless a preview opened on top of
            // this capture (the preview restores it when it closes).
            settings_finish_capture();

            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Give the overlay focus so it receives WM_KEYDOWN (Escape) and start the
// mouse hook. AttachThreadInput bypasses the strict SetForegroundWindow rules.
static void show_region_overlay(void) {
    ShowWindow(g_hwndRegion, SW_SHOW);

    HWND hCurWnd = GetForegroundWindow();
    DWORD dwCurID = GetCurrentThreadId();
    DWORD dwForeID = GetWindowThreadProcessId(hCurWnd, NULL);

    if (dwCurID != dwForeID) {
        AttachThreadInput(dwCurID, dwForeID, TRUE);
        SetForegroundWindow(g_hwndRegion);
        SetFocus(g_hwndRegion);
        AttachThreadInput(dwCurID, dwForeID, FALSE);
    } else {
        SetForegroundWindow(g_hwndRegion);
        SetFocus(g_hwndRegion);
    }

    UpdateWindow(g_hwndRegion);
    install_mouse_hook();
}

static void start_region_capture(void) {
    // The previous preview and the Settings window would otherwise be frozen
    // into the overlay frame.
    BOOL hadPreview = preview_close_for_capture();
    BOOL hadSettings = settings_hide_for_capture();
    if (hadPreview || hadSettings) Sleep(120);

    // Fallback safety: If the flag was somehow stuck, reset it before starting
    g_isRegionCapturing = TRUE; 
    g_blockRightClick = FALSE;
    if (g_hwndTray) KillTimer(g_hwndTray, TIMER_MOUSE_GRACE);

    if (g_hwndRegion) return;

    // Ensure a completely clean slate before starting a new capture
    g_isDragging = FALSE;
    g_rcSel.left = g_rcSel.top = g_rcSel.right = g_rcSel.bottom = 0;

    // Capture the monitor under the cursor once.
    // This exact frame is shown in the overlay and is also the source of the saved region, so HDR survives.
    free_grab(&g_regionFrame);
    HMONITOR target_monitor = monitor_under_cursor();
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(target_monitor, &mi)) {
        dbg("start_region_capture: GetMonitorInfo failed");
        g_isRegionCapturing = FALSE;
        begin_mouse_grace();
        settings_finish_capture();
        return;
    }

    if (!grab_monitor(&g_regionFrame, target_monitor) ||
        !g_regionFrame.bits || g_regionFrame.w <= 0 || g_regionFrame.h <= 0) {
        dbg("start_region_capture: grab failed");
        free_grab(&g_regionFrame);
        g_isRegionCapturing = FALSE;
        begin_mouse_grace();
        settings_finish_capture();
        return;
    }

    g_regionMonitorRect = mi.rcMonitor;
    g_screenW = g_regionFrame.w;
    g_screenH = g_regionFrame.h;
    HDC hdcScreen = GetDC(NULL);

    // Arial label font and metrics
    if (!g_dimFont) {
        LOGFONTW lf;
        ZeroMemory(&lf, sizeof lf);
        lf.lfHeight = -MulDiv(14, GetDeviceCaps(hdcScreen, LOGPIXELSY), 72);
        lf.lfWeight = FW_BOLD;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = ANTIALIASED_QUALITY;
        lf.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
        wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Arial");
        g_dimFont = CreateFontIndirectW(&lf);
    }
    g_dimTextH = 16;
    {
        HGDIOBJ oldFont = SelectObject(hdcScreen,
            g_dimFont ? g_dimFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
        TEXTMETRICW tm;
        if (GetTextMetricsW(hdcScreen, &tm)) g_dimTextH = tm.tmHeight;
        SelectObject(hdcScreen, oldFont);
    }
    g_dimOutline = GetDeviceCaps(hdcScreen, LOGPIXELSY) / 96;
    if (g_dimOutline < 1) g_dimOutline = 1;

    g_measureDC = CreateCompatibleDC(hdcScreen);

    WNDCLASSEXW wc = {0}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = RegionWndProc;
    wc.hInstance = GetModuleHandle(NULL); wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    wc.lpszClassName = L"JxlShotRegionClass"; RegisterClassExW(&wc);

    g_hwndRegion = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"JxlShotRegionClass", L"",
                                   WS_POPUP,
                                   g_regionMonitorRect.left, g_regionMonitorRect.top,
                                   g_screenW, g_screenH,
                                   NULL, NULL, GetModuleHandle(NULL), NULL);

    // If window creation fails, clean up immediately to prevent leaks
    if (!g_hwndRegion) {
        free_grab(&g_regionFrame);
        if (g_measureDC){ DeleteDC(g_measureDC); g_measureDC = NULL; }
        if (g_dimFont)  { DeleteObject(g_dimFont); g_dimFont = NULL; }
        ReleaseDC(NULL, hdcScreen);
        g_isRegionCapturing = FALSE;
        begin_mouse_grace();
        settings_finish_capture();
        return;
    }

    // Use the GPU overlay on both HDR and SDR monitors. 
    // HDR keeps the captured scRGB FP16 data instead of an 8-bit approximation
    // SDR runs through the same presenter so the renderer can be verified on any display.
    // The saved crop is the same frame either way.
    if (gpu_overlay_init(g_hwndRegion, target_monitor, g_screenW, g_screenH,
                         g_regionFrame.is_hdr)) {
        dbg("start_region_capture: GPU overlay active (%s)",
            g_regionFrame.is_hdr ? "HDR scRGB" : "SDR sRGB");
        ReleaseDC(NULL, hdcScreen);
        show_region_overlay();
        return;
    }
    dbg("start_region_capture: GPU overlay unavailable, using GDI preview");

    // Original screenshot: SDR preview of the captured frame on the overlay.
    g_hdcMem = CreateCompatibleDC(hdcScreen);
    g_hbmScreen = create_screen_dib(hdcScreen, (void **)&g_screenBits);
    if (!g_hdcMem || !g_hbmScreen || !g_screenBits) {
        dbg("start_region_capture: overlay DIB creation failed");
        ReleaseDC(NULL, hdcScreen);
        DestroyWindow(g_hwndRegion); // WM_DESTROY releases the shared resources
        return;
    }
    SelectObject(g_hdcMem, g_hbmScreen);
    build_overlay_from_region_frame();

    // Dimmed copy: blend the dark layer once, not on every mouse move.
    g_hdcDim = CreateCompatibleDC(hdcScreen);
    g_hbmDim = create_screen_dib(hdcScreen, NULL);
    SelectObject(g_hdcDim, g_hbmDim);
    BitBlt(g_hdcDim, 0, 0, g_screenW, g_screenH, g_hdcMem, 0, 0, SRCCOPY);
    HDC hdcBlack = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmBlack = CreateCompatibleBitmap(hdcScreen, 1, 1);
    HBITMAP hbmBlackOld = (HBITMAP)SelectObject(hdcBlack, hbmBlack);
    RECT rcBlack = {0, 0, 1, 1};
    FillRect(hdcBlack, &rcBlack, (HBRUSH)GetStockObject(BLACK_BRUSH));
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 120, 0 };
    AlphaBlend(g_hdcDim, 0, 0, g_screenW, g_screenH, hdcBlack, 0, 0, 1, 1, bf);
    SelectObject(hdcBlack, hbmBlackOld);
    DeleteObject(hbmBlack);
    DeleteDC(hdcBlack);

    // Back buffer: WM_PAINT composes here, then presents one finished frame.
    g_hdcBack = CreateCompatibleDC(hdcScreen);
    g_hbmBack = create_screen_dib(hdcScreen, NULL);
    SelectObject(g_hdcBack, g_hbmBack);
    BitBlt(g_hdcBack, 0, 0, g_screenW, g_screenH, g_hdcDim, 0, 0, SRCCOPY);

    ReleaseDC(NULL, hdcScreen);
    show_region_overlay();
}

// Low-Level Keyboard Hook

// Helper function to verify EXACT modifier match. 
// If the INI requires Ctrl, Ctrl must be pressed. 
// If the INI does NOT require Ctrl, Ctrl must NOT be pressed.

static BOOL check_modifiers(UINT required_mod) {
    BOOL ctrl_pressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    BOOL shift_pressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    BOOL alt_pressed = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    BOOL win_pressed = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;

    BOOL req_ctrl = (required_mod & MOD_CONTROL) != 0;
    BOOL req_shift = (required_mod & MOD_SHIFT) != 0;
    BOOL req_alt = (required_mod & MOD_ALT) != 0;
    BOOL req_win = (required_mod & MOD_WIN) != 0;

    // All required modifiers must match the pressed state exactly
    if (ctrl_pressed != req_ctrl) return FALSE;
    if (shift_pressed != req_shift) return FALSE;
    if (alt_pressed != req_alt) return FALSE;
    if (win_pressed != req_win) return FALSE;

    return TRUE;
}

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT *pKB = (KBDLLHOOKSTRUCT *)lParam;
        BOOL k_down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        BOOL k_up   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);

        // Hotkey-settings capture mode: app hotkeys are halted, keys are routed to the dialog.
        if (g_hotkeyCapturing) {
            if (!is_modifier_vk(pKB->vkCode)) {
                if (k_down) {
                    if (g_hwnd_catcher) {
                        g_swallowUpVk = pKB->vkCode;
                        PostMessageW(g_hwnd_catcher, WM_HOTKEY_CAPTURED, pKB->vkCode, current_mods());
                    }
                    return 1; // block from OS / other apps
                }
                if (k_up) return 1;
            }
            return CallNextHookEx(g_hhkKeyboard, nCode, wParam, lParam); // let modifiers through
        }

// Swallow the key-up of the key we just captured (e.g. PrintScreen)
        if (k_up && g_swallowUpVk && pKB->vkCode == g_swallowUpVk) {
            g_swallowUpVk = 0;
            return 1;
        }
// Intercept Escape globally while region selection is active.
// This guarantees Esc works even if Windows refuses to give the overlay keyboard focus,
// and prevents the Esc key from accidentally exiting fullscreen games or closing underlying menus.

        if (g_hwndRegion && pKB->vkCode == VK_ESCAPE && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
            PostMessageW(g_hwndRegion, WM_KEYDOWN, VK_ESCAPE, 0);
            return 1; // Block the key from reaching the underlying application
        }
// Check Full Capture Hotkey 
// Condition: Key is defined (!= 0)
// AND pressed key matches AND modifiers match exactly
        if (k_down) {
            // Check Full Capture Hotkey
            if (g_cfg.hk_full_vk != 0 && pKB->vkCode == g_cfg.hk_full_vk) {
                if (check_modifiers(g_cfg.hk_full_mod)) {
                    dbg("MATCH: Triggering FULL capture (VK=%d, MOD=%d)", g_cfg.hk_full_vk, g_cfg.hk_full_mod);
                    PostMessageW(g_hwndTray, WM_HOOK_FULL_CAPTURE, 0, 0);
                    return 1; // Block key from propagating to other apps
                }
            }
            
            /// Check Region Capture Hotkey
            if (g_cfg.hk_region_vk != 0 && pKB->vkCode == g_cfg.hk_region_vk) {
                if (check_modifiers(g_cfg.hk_region_mod)) {
                    dbg("MATCH: Triggering REGION capture (VK=%d, MOD=%d)", g_cfg.hk_region_vk, g_cfg.hk_region_mod);
                    PostMessageW(g_hwndTray, WM_HOOK_REGION_CAPTURE, 0, 0);
                    return 1; // Block key from propagating to other apps
                }
            }

        }
    }
    
    // If no match, or if the configured VK is 0 (null/disabled), pass the key to the OS normally
    return CallNextHookEx(g_hhkKeyboard, nCode, wParam, lParam);
}

static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        // Block if currently capturing, OR during the post-capture grace period
        BOOL is_active = g_isRegionCapturing || g_blockRightClick;
        
        if (is_active && (wParam == WM_RBUTTONDOWN || wParam == WM_RBUTTONUP)) {
            if (wParam == WM_RBUTTONDOWN && g_hwndRegion) {
                PostMessageW(g_hwndRegion, WM_RBUTTONDOWN, 0, 0);
            }
            return 1; // Completely block the event from reaching the OS / underlying apps
        }
    }
    return CallNextHookEx(g_hhkMouse, nCode, wParam, lParam);
}

// Low-level keyboard/mouse hooks. Windows delivers a WH_*_LL callback on the
// thread that installed the hook, and silently removes the hook if a callback
// is not serviced within LowLevelHooksTimeout (~300 ms). The UI thread blocks
// for hundreds of milliseconds during a capture, so the hooks live on their
// own thread that does nothing but pump messages.
static HANDLE g_hookThread = NULL;
static DWORD  g_hookThreadId = 0;
static volatile LONG g_hookStop = 0;

static DWORD WINAPI HookThreadProc(LPVOID param) {
    (void)param;

    // Retry instead of giving up on the first failure
    // SetWindowsHookEx canfail while the desktop/shell is still coming up at logon
    for (int tries = 0; !g_hhkKeyboard && !g_hookStop; tries++) {
        g_hhkKeyboard = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
        if (!g_hhkKeyboard) {
            if (tries == 0) dbg("hook: SetWindowsHookEx(WH_KEYBOARD_LL) failed (err=%lu), retrying", GetLastError());
            Sleep(500);
        }
    }
    for (int tries = 0; !g_hhkMouse && !g_hookStop; tries++) {
        g_hhkMouse = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandle(NULL), 0);
        if (!g_hhkMouse) {
            if (tries == 0) dbg("hook: SetWindowsHookEx(WH_MOUSE_LL) failed (err=%lu), retrying", GetLastError());
            Sleep(500);
        }
    }

    MSG msg;
    while (!g_hookStop && GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_hhkKeyboard) { UnhookWindowsHookEx(g_hhkKeyboard); g_hhkKeyboard = NULL; }
    if (g_hhkMouse)    { UnhookWindowsHookEx(g_hhkMouse);    g_hhkMouse = NULL; }
    return 0;
}

// Start the dedicated hook thread. If it cannot be created, fall back to
// installing the hooks on the calling (UI) thread, exactly as before.
static void install_keyboard_hook(void) {
    if (g_hookThread || g_hhkKeyboard) return;
    InterlockedExchange(&g_hookStop, 0);
    g_hookThreadId = 0;
    g_hookThread = CreateThread(NULL, 0, HookThreadProc, NULL, 0, &g_hookThreadId);
    if (!g_hookThread) {
        g_hhkKeyboard = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
        g_hhkMouse    = SetWindowsHookExW(WH_MOUSE_LL,    LowLevelMouseProc,    GetModuleHandle(NULL), 0);
    }
}

// Unhook both hooks and join the hook thread. Safe to call more than once.
static void uninstall_keyboard_hook(void) {
    InterlockedExchange(&g_hookStop, 1);
    if (g_hhkKeyboard) { UnhookWindowsHookEx(g_hhkKeyboard); g_hhkKeyboard = NULL; }
    if (g_hhkMouse)    { UnhookWindowsHookEx(g_hhkMouse);    g_hhkMouse = NULL; }
    if (g_hookThread) {
        if (g_hookThreadId) PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_hookThread, 3000);
        CloseHandle(g_hookThread);
        g_hookThread = NULL;
        g_hookThreadId = 0;
    }
}

// The mouse hook now lives on the hook thread alongside the keyboard hook, so
// there is nothing to install lazily. Kept for the existing call sites.
static void install_mouse_hook(void) { }
static void uninstall_mouse_hook(void) { }

// Save-result notifications marshalled from the EncodeWorker to the UI thread.

#define WM_JXLSHOT_SAVE_NOTIFY (WM_USER + 30)
#define WM_JXLSHOT_SAVE_OK     (WM_USER + 31)

typedef struct {
    UINT    icon;
    wchar_t title[64];
    wchar_t body[1024];
} SaveNotify;

// Installed into jxlshot.c's g_saveNotify. Copies the message and posts it to
// the tray window so the MessageBox is owned by a real top-level window and is
// shown on the thread that owns the message loop (not the encode worker).
static void show_save_notification_ui(UINT icon, const wchar_t *title, const wchar_t *body) {
    SaveNotify *n = (SaveNotify *)malloc(sizeof *n);
    if (n) {
        n->icon = icon;
        wcsncpy_s(n->title, 64, title, _TRUNCATE);
        wcsncpy_s(n->body, 1024, body, _TRUNCATE);
        if (g_hwndTray && PostMessageW(g_hwndTray, WM_JXLSHOT_SAVE_NOTIFY, 0, (LPARAM)n))
            return;
        free(n);
    }
    // Last resort (no tray window yet, or during shutdown): show it inline.
    MessageBoxW(NULL, body, title, icon | MB_OK);
}


// Success feedback: a green-dot tray icon for a successful capture

// Builds the app icon with a green dot in the lower-right corner.
// Returns a new icon owned by the caller, or NULL.
static HICON make_success_icon(HICON base, int size) {
    if (size < 8) size = 16;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size; // negative: top-down, row 0 is the top
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = CreateCompatibleDC(NULL);
    if (!screen) return NULL;

    HICON icon = NULL;
    void *bits = NULL;
    HBITMAP color = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (color && bits) {
        HGDIOBJ old = SelectObject(screen, color);
        memset(bits, 0, (size_t)size * (size_t)size * 4);
        if (base) DrawIconEx(screen, 0, 0, base, size, size, 0, NULL, DI_NORMAL);

        // GDI never writes the alpha channel, so the dot is painted pixel by
        // pixel (BGRA) instead of with Ellipse() - otherwise it stays invisible.
        const int r = (size / 4 < 3) ? 3 : size / 4;
        const int cx = size - r - 1, cy = size - r - 1;
        unsigned char *px = (unsigned char *)bits;
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                int dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy <= r * r) {
                    unsigned char *p = px + ((size_t)y * (size_t)size + (size_t)x) * 4;
                    p[0] = 60; p[1] = 190; p[2] = 60; p[3] = 255; // green
                }
            }
        }
        SelectObject(screen, old);

        HBITMAP mask = CreateBitmap(size, size, 1, 1, NULL);
        if (mask) {
            ICONINFO ii;
            ZeroMemory(&ii, sizeof(ii));
            ii.fIcon = TRUE;
            ii.hbmColor = color;
            ii.hbmMask = mask;
            icon = CreateIconIndirect(&ii);
            DeleteObject(mask);
        }
    }
    if (color) DeleteObject(color);
    DeleteDC(screen);
    return icon;
}

// Installed into jxlshot.c's g_saveOk; runs on the EncodeWorker thread.
static void notify_save_ok_ui(void) {
    if (g_hwndTray) PostMessageW(g_hwndTray, WM_JXLSHOT_SAVE_OK, 0, 0);
}

// UI thread: swap in the green icon; the timer puts the normal one back.
static void show_save_ok(HWND hwnd) {
    if (!g_iconOk) g_iconOk = make_success_icon(g_iconNormal, GetSystemMetrics(SM_CXSMICON));
    if (!g_iconOk) return;
    g_nid.hIcon = g_iconOk;
    g_nid.uFlags = NIF_ICON;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    SetTimer(hwnd, TIMER_TRAY_ICON, TRAY_ICON_OK_MS, NULL);
}

// Tray icon (re-)registration

// Adds the notification-area icon, or refreshes it if it is already there
// (NIM_ADD on an existing hWnd/uID pair just updates it).
static BOOL tray_icon_add(void) {
    if (Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        g_trayIconAdded = TRUE;
        return TRUE;
    }
    g_trayIconAdded = FALSE;
    return FALSE;
}

// Puts the icon back. Called at startup, after "TaskbarCreated" and from the
// health timer, so a lost icon heals itself instead of leaving a live process
// with no UI. On failure a retry timer is armed: at logon the shell is often
// not ready yet when the Run key starts us.
static void tray_icon_ensure(HWND hwnd) {
    if (tray_icon_add()) {
        KillTimer(hwnd, TIMER_TRAY_RETRY);
        SetTimer(hwnd, TIMER_TRAY_HEALTH, TRAY_HEALTH_MS, NULL);
        return;
    }

    // Only the first failure is logged: a shell that is not up yet can fail
    // this for a few seconds and would otherwise spam the log.
    static BOOL logged = FALSE;
    if (!logged) {
        dbg("tray: Shell_NotifyIcon(NIM_ADD) failed, retrying every %d ms", TRAY_RETRY_MS);
        logged = TRUE;
    }
    SetTimer(hwnd, TIMER_TRAY_RETRY, TRAY_RETRY_MS, NULL);
}

// Tray Window Procedure & Entry Point

// A capture started from the tray or a hotkey must not include (or cover) the
// main strip. When it is open, reuse the GUI's own hide-wait-capture-restore
// flow instead of grabbing the screen immediately.
static void request_full_capture(void) {
    if (gui_is_visible()) gui_start_capture(1);
    else                  execute_full_capture();
}

static void request_region_capture(void) {
    if (gui_is_visible()) gui_start_capture(2);
    else                  start_region_capture();
}

LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lParam) {
    switch (msg) {
        case WM_TRAYICON:
            // Left click opens the main window (Snipping Tool style),
            // Right click open the context menu.
            if (lParam == WM_LBUTTONUP) execute_gui();
            else if (lParam == WM_RBUTTONUP) show_tray_menu(hwnd);
            break;
        case WM_HOOK_FULL_CAPTURE: request_full_capture(); break;
        case WM_HOOK_REGION_CAPTURE: request_region_capture(); break;
        case WM_JXLSHOT_SAVE_NOTIFY: {
            SaveNotify *n = (SaveNotify *)lParam;
            if (n) {
                MessageBoxW(g_hwndMenuOwner, n->body, n->title, n->icon | MB_OK);
                free(n);
            }
            return 0;
        }
        case WM_JXLSHOT_SAVE_OK:
            show_save_ok(hwnd);
            return 0;
        case WM_TIMER:
            if (wp == TIMER_TRAY_ICON) {
                KillTimer(hwnd, TIMER_TRAY_ICON);
                g_nid.hIcon = g_iconNormal;
                g_nid.uFlags = NIF_ICON;
                Shell_NotifyIconW(NIM_MODIFY, &g_nid);
                g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
                return 0;
            }
            if (wp == TIMER_TRAY_RETRY) {
                KillTimer(hwnd, TIMER_TRAY_RETRY);
                tray_icon_ensure(hwnd);
                return 0;
            }
            if (wp == TIMER_TRAY_HEALTH) {
                // NIM_MODIFY returns FALSE when the icon no longer exists
                // example after an explorer restart Re-add it.
                g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
                if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid)) {
                    dbg("tray: health check lost the icon, re-adding");
                    tray_icon_ensure(hwnd);
                }
                return 0;
            }
            if (wp == TIMER_MOUSE_GRACE) {
                KillTimer(hwnd, TIMER_MOUSE_GRACE);
                g_blockRightClick = FALSE;
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lParam);
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDM_GUI: execute_gui(); break;
                case IDM_FULL: request_full_capture(); break;
                case IDM_REGION: request_region_capture(); break;
                case IDM_CLOSEPREVIEW: gui_preview_close(); break;
                case IDM_SETPATH: execute_set_path(); break;
                case IDM_OPENEXPORT: execute_open_export_folder(); break;
                case IDM_OPENCONFIG: execute_open_config(hwnd); break;
                case IDM_HOTKEYS: execute_hotkey_settings(hwnd); break;
                case IDM_DEBUG: toggle_debug(); break;
                case IDM_CHECK_UPDATE: execute_open_releases(hwnd); break;
                case IDM_ABOUT: execute_about(); break;
                // DestroyWindow (not PostQuitMessage) so WM_DESTROY actually runs
                // and the tray icon is removed. Posting WM_QUIT directly skipped
                // WM_DESTROY entirely and left a ghost icon in the tray.
                case IDM_EXIT:
                    if (g_hwndTray) DestroyWindow(g_hwndTray);
                    else PostQuitMessage(0);
                    break;
            } break;
        case WM_DESTROY: {
            // Null the owner pointer *before* destroying it: this same wndproc
            // also runs for g_hwndMenuOwner, so a stale pointer would recurse.
            HWND menuOwner = g_hwndMenuOwner;
            g_hwndMenuOwner = NULL;
            uninstall_mouse_hook();
            uninstall_keyboard_hook(); // joins the hook thread
            KillTimer(hwnd, TIMER_TRAY_ICON);
            KillTimer(hwnd, TIMER_TRAY_RETRY);
            KillTimer(hwnd, TIMER_TRAY_HEALTH);
            KillTimer(hwnd, TIMER_MOUSE_GRACE);
            if (g_iconOk) {
                DestroyIcon(g_iconOk);
                g_iconOk = NULL;
            }
            g_nid.hIcon = g_iconNormal; // never leave a destroyed icon referenced
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            g_trayIconAdded = FALSE;
            if (menuOwner) DestroyWindow(menuOwner);
            PostQuitMessage(0);
            break;
        }
        default:
            // explorer.exe broadcasts this after it (re)starts.
            if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {
                if (hwnd == g_hwndTray) {
                    dbg("tray: TaskbarCreated received, re-adding icon");
                    g_trayIconAdded = FALSE;
                    tray_icon_ensure(hwnd);
                }
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lParam);
    }
    return 0;
}

#include "about_dialog.c" // Pulls in the custom About dialog implementation
#include "gui.c"          // Snipping Tool style main window

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR szCmdLine, int sw) {
    // 1. Initialize COM for Shell APIs (SHBrowseForFolder), TaskDialog, and DXGI/D3D11 stability
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    // Single instance: a second tray would install duplicate global hooks and a
    // second tray icon, and whichever exits first would tear the hooks down from
    // under the other. The handle is intentionally kept for the process lifetime.
    SetLastError(0);
    HANDLE hInstanceLock = CreateMutexW(NULL, TRUE, L"JxlShotTray_SingleInstance_Mutex");
    if (hInstanceLock && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hInstanceLock);
        CoUninitialize();
        return 0;
    }

    set_dpi_aware(); 
    init_paths(); 
    ensure_default_ini(); 
    init_config();
    dbg_init();
    // Initialize dark mode BEFORE creating any windows or dialogs
    InitializeDarkMode();

    WNDCLASSEXW wc = {0}; 
    wc.cbSize = sizeof(wc); 
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInst; 
    wc.lpszClassName = L"JxlShotTrayClass"; 
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"Failed to initialize the tray application window class.", L"jxlshot", MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    g_hwndTray = CreateWindowExW(WS_EX_TOOLWINDOW, L"JxlShotTrayClass", L"", WS_POPUP,
                                 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hwndTray) {
        MessageBoxW(NULL, L"Failed to create the tray window.", L"jxlshot", MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    // Create a hidden popup window specifically to own the context menu
    g_hwndMenuOwner = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        L"JxlShotTrayClass",
        L"", 
        WS_POPUP,
        0, 0, 0, 0,
        NULL, NULL, hInst, NULL
    );
    if (g_hwndMenuOwner) {
        ShowWindow(g_hwndMenuOwner, SW_HIDE);
        ApplyDarkMode(g_hwndMenuOwner);
    } else {
        dbg("WinMain: menu-owner window creation failed; menus will fall back to the tray window");
    }

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid); 
    g_nid.hWnd = g_hwndTray; 
    g_nid.uID = ID_TRAY;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; 
    g_nid.uCallbackMessage = WM_TRAYICON;

    g_iconNormal = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
    g_nid.hIcon = g_iconNormal;
    wcscpy(g_nid.szTip, L"JXL Shot");

    // Registered before the window exists: explorer.exe broadcasts this message
    // to every top-level window after it starts, which is the only way to know
    // that the notification area was recreated and every icon was lost.
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    tray_icon_ensure(g_hwndTray);
    dbg("tray: startup complete (icon %s)", g_trayIconAdded ? "added" : "pending retry");

    // Only install the global keyboard/mouse hooks if not disabled in the INI
    if (!g_cfg.disable_keyboard_hook) {
        install_keyboard_hook();
    } else {
        dbg("tray: global keyboard hook disabled by config (DisableKeyboardHook=1)");
    }

    // Route background save notifications to the UI thread from now on.
    g_saveNotify = show_save_notification_ui;
    g_saveOk = notify_save_ok_ui;

    // Once the encoder has closed the .jxl, the preview can replace its raw
    // capture with the decoded file (PQ / Rec.2020 -> scRGB in libjxl).
    g_saveReady = preview_on_saved;

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) { 
        TranslateMessage(&msg); 
        DispatchMessage(&msg); 
    }

    // if the loop ended without WM_DESTROY (GetMessage error)
    // do not leave a ghost icon in the notification area.
    if (g_trayIconAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayIconAdded = FALSE;
    }

    // Wait for any in-flight background save before returning: WinMain returning
    // calls ExitProcess(), which would hard-kill the worker mid-write and leave a
    // truncated .jxl on disk. The message loop has stopped, so no new worker can
    // be spawned meanwhile.
    drain_encodes();

    uninstall_keyboard_hook();

    // Clean up uxtheme before exiting
    if (g_hUxtheme) {
        FreeLibrary(g_hUxtheme);
        g_hUxtheme = NULL;
    }

    // Clean up COM before exiting
    CoUninitialize();

    return (int)msg.wParam;
}