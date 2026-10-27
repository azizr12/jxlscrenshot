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

// Global state to avoid reloading the DLL repeatedly
static HMODULE g_hUxtheme = NULL;
static fnSetPreferredAppMode g_pSetPreferredAppMode = NULL;
static fnAllowDarkModeForWindow g_pAllowDarkModeForWindow = NULL;
static fnFlushMenuThemes g_pFlushMenuThemes = NULL;

static void InitializeDarkMode(void) {
    if (g_hUxtheme) return;

    g_hUxtheme = LoadLibraryW(L"uxtheme.dll");
    if (!g_hUxtheme) return;

    g_pSetPreferredAppMode = (fnSetPreferredAppMode)GetProcAddress(g_hUxtheme, MAKEINTRESOURCEA(135));
    g_pAllowDarkModeForWindow = (fnAllowDarkModeForWindow)GetProcAddress(g_hUxtheme, MAKEINTRESOURCEA(133));
    g_pFlushMenuThemes = (fnFlushMenuThemes)GetProcAddress(g_hUxtheme, MAKEINTRESOURCEA(136));

    // ForceDark is required for TaskDialogs to reliably apply the theme
    if (g_pSetPreferredAppMode) {
        g_pSetPreferredAppMode(ForceDark);
    }
    if (g_pFlushMenuThemes) {
        g_pFlushMenuThemes();
    }
}

static void ApplyDarkMode(HWND hwnd) {
    InitializeDarkMode();
    if (!hwnd || !g_pAllowDarkModeForWindow) return;

    g_pAllowDarkModeForWindow(hwnd, TRUE);
    
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
#define IDM_RELOAD       106
#define IDM_EXIT         103
#define IDM_OPENCONFIG   107
#define IDM_CHECK_UPDATE 108
#define IDM_OPENEXPORT   109
#define IDM_HOTKEYS      110

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
static void execute_about(void);

static void reload_config(void) {
    init_config();
    dbg("config reloaded: effort=%d lossless=%d distance=%.2f multimonitor=%s",
        g_cfg.effort, g_cfg.lossless, g_cfg.distance,
        g_cfg.multimonitor ? "separateimages" : "focusmonitor");
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

#define IDC_LOSSLESS   204
#define IDC_DIST_DN    205
#define IDC_DIST_UP    206
#define IDC_EFF_DN     207
#define IDC_EFF_UP     208

#define HK_W        300
#define HK_H        360
#define HK_TITLE_H  36
#define HK_Y_HEAD   178
#define HK_Y_LOSS   198
#define HK_Y_DIST   240
#define HK_Y_EFF    282
#define HK_ROW_H    34
#define HK_STEP_W   34
#define HK_VAL_W    64
#define HK_DN_X     (HK_W - 16 - HK_STEP_W - HK_VAL_W - HK_STEP_W)
#define HK_UP_X     (HK_W - 16 - HK_STEP_W)

#define HK_BG        RGB(32, 32, 32)
#define HK_BTN       RGB(51, 51, 51)
#define HK_BTN_HOT   RGB(68, 68, 68)
#define HK_BTN_DOWN  RGB(85, 85, 85)
#define HK_BORDER    RGB(75, 75, 75)
#define HK_ACCENT    RGB(0, 120, 215)
#define HK_TEXT      RGB(240, 240, 240)
#define HK_DIM       RGB(150, 150, 150)
#define HK_CLOSE_HOT RGB(196, 43, 28)

static HWND  g_hwnd_catcher = NULL;
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
    dbg("settings changed: effort=%d lossless=%d distance=%.2f",
        g_cfg.effort, g_cfg.lossless, g_cfg.distance);
}

static void hk_refresh_buttons(HWND hwnd) {
    wchar_t hk[128], txt[192];
    if (g_catcher_target == 1) wcscpy_s(txt, 192, L"Full Screen:  press a key...");
    else {
        format_hotkey_string(g_cfg.hk_full_vk, g_cfg.hk_full_mod, hk, 128);
        _snwprintf(txt, 192, L"Full Screen:  %s", hk); txt[191] = 0;
    }
    SetWindowTextW(GetDlgItem(hwnd, IDC_HK_FULL), txt);

    if (g_catcher_target == 2) wcscpy_s(txt, 192, L"Region:  press a key...");
    else {
        format_hotkey_string(g_cfg.hk_region_vk, g_cfg.hk_region_mod, hk, 128);
        _snwprintf(txt, 192, L"Region:  %s", hk); txt[191] = 0;
    }
    SetWindowTextW(GetDlgItem(hwnd, IDC_HK_REGION), txt);

    SetWindowTextW(GetDlgItem(hwnd, IDC_LOSSLESS),
                   g_cfg.lossless ? L"Lossless:  ON" : L"Lossless:  OFF (lossy)");
    SetWindowTextW(GetDlgItem(hwnd, IDC_DIST_DN), L"\u2212");
    SetWindowTextW(GetDlgItem(hwnd, IDC_DIST_UP), L"+");
    SetWindowTextW(GetDlgItem(hwnd, IDC_EFF_DN),  L"\u2212");
    SetWindowTextW(GetDlgItem(hwnd, IDC_EFF_UP),  L"+");
    EnableWindow(GetDlgItem(hwnd, IDC_DIST_DN), !g_cfg.lossless);
    EnableWindow(GetDlgItem(hwnd, IDC_DIST_UP), !g_cfg.lossless);
    
    {
        int ids[] = { IDC_LOSSLESS, IDC_DIST_DN, IDC_DIST_UP, IDC_EFF_DN, IDC_EFF_UP };
        for (int i = 0; i < 5; i++) InvalidateRect(GetDlgItem(hwnd, ids[i]), NULL, FALSE);
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
                  (id == IDC_LOSSLESS && g_cfg.lossless);

    COLORREF bg;
    if (id == IDC_HK_CLOSE) bg = hot ? HK_CLOSE_HOT : HK_BG;
    else bg = dis ? HK_BTN : (down ? HK_BTN_DOWN : (hot ? HK_BTN_HOT : HK_BTN));

    HBRUSH b = CreateSolidBrush(bg);
    FillRect(dc, &rc, b);
    DeleteObject(b);

    if (id == IDC_HK_CLOSE) {
        int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2, s = 5;
        HPEN pen = CreatePen(PS_SOLID, 1, HK_TEXT);
        HPEN old = (HPEN)SelectObject(dc, pen);
        MoveToEx(dc, cx - s, cy - s, NULL); LineTo(dc, cx + s + 1, cy + s + 1);
        MoveToEx(dc, cx + s, cy - s, NULL); LineTo(dc, cx - s - 1, cy + s + 1);
        SelectObject(dc, old); DeleteObject(pen);
        return;
    }

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
}

static LRESULT CALLBACK CatcherWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hkFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                   CLEARTYPE_QUALITY, 0, L"Segoe UI");
            g_hkFontSmall = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                        CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HINSTANCE hi = GetModuleHandle(NULL);
            struct { int id; int x, y, w, h; } b[] = {
                { IDC_HK_FULL,   16, 48,  HK_W - 32, 38 },
                { IDC_HK_REGION, 16, 94,  HK_W - 32, 38 },
                { IDC_HK_CLOSE,  HK_W - 41, 1, 38, HK_TITLE_H - 2 },
                { IDC_LOSSLESS,  16, HK_Y_LOSS, HK_W - 32, HK_ROW_H },
                { IDC_DIST_DN,   HK_DN_X, HK_Y_DIST, HK_STEP_W, HK_ROW_H },
                { IDC_DIST_UP,   HK_UP_X, HK_Y_DIST, HK_STEP_W, HK_ROW_H },
                { IDC_EFF_DN,    HK_DN_X, HK_Y_EFF,  HK_STEP_W, HK_ROW_H },
                { IDC_EFF_UP,    HK_UP_X, HK_Y_EFF,  HK_STEP_W, HK_ROW_H },
            };
            for (int i = 0; i < (int)(sizeof(b) / sizeof(b[0])); i++) {
                HWND hb = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                          b[i].x, b[i].y, b[i].w, b[i].h, hwnd,
                                          (HMENU)(INT_PTR)b[i].id, hi, NULL);
                SetWindowSubclass(hb, HkBtnSubclass, (UINT_PTR)b[i].id, 0);
            }
            hk_refresh_buttons(hwnd);
            return 0;
        }
        case WM_ERASEBKGND: return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            HBRUSH bg = CreateSolidBrush(HK_BG); FillRect(dc, &rc, bg); DeleteObject(bg);
            HBRUSH bd = CreateSolidBrush(HK_BORDER); FrameRect(dc, &rc, bd); DeleteObject(bd);

            SetBkMode(dc, TRANSPARENT);
            HFONT of = (HFONT)SelectObject(dc, g_hkFont);
            SetTextColor(dc, HK_TEXT);
            RECT t = { 16, 0, HK_W - 48, HK_TITLE_H };
            DrawTextW(dc, L"Settings", -1, &t, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

            SelectObject(dc, g_hkFontSmall);
            SetTextColor(dc, HK_DIM);
            RECT h = { 16, 140, HK_W - 16, 172 };
            DrawTextW(dc, g_catcher_target
                ? L"Press the new key combo. Esc cancels."
                : L"Click a button, then press the new key combo.",
                -1, &h, DT_WORDBREAK | DT_LEFT);

            SelectObject(dc, g_hkFontSmall);
            SetTextColor(dc, HK_DIM);
            RECT hd = { 16, HK_Y_HEAD, HK_W - 16, HK_Y_HEAD + 16 };
            DrawTextW(dc, L"JPEG XL", -1, &hd, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

            SelectObject(dc, g_hkFont);
            wchar_t vbuf[32];
            COLORREF dist_col = g_cfg.lossless ? HK_DIM : HK_TEXT;

            RECT lr = { 16, HK_Y_DIST, HK_DN_X - 4, HK_Y_DIST + HK_ROW_H };
            SetTextColor(dc, dist_col);
            DrawTextW(dc, L"Distance", -1, &lr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
            RECT vr = { HK_DN_X + HK_STEP_W, HK_Y_DIST, HK_UP_X, HK_Y_DIST + HK_ROW_H };
            _snwprintf(vbuf, 32, L"%.1f", g_cfg.distance);
            DrawTextW(dc, vbuf, -1, &vr, DT_SINGLELINE | DT_VCENTER | DT_CENTER);

            lr.top = HK_Y_EFF; lr.bottom = HK_Y_EFF + HK_ROW_H;
            SetTextColor(dc, HK_TEXT);
            DrawTextW(dc, L"Effort", -1, &lr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
            vr.top = HK_Y_EFF; vr.bottom = HK_Y_EFF + HK_ROW_H;
            _snwprintf(vbuf, 32, L"%d", g_cfg.effort);
            DrawTextW(dc, vbuf, -1, &vr, DT_SINGLELINE | DT_VCENTER | DT_CENTER);


            RECT info = { 16, HK_H - 35, HK_W - 16, HK_H - 5 };
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
            RECT info = { 16, HK_H - 35, HK_W - 16, HK_H - 5 };
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
            RECT info = { 16, HK_H - 35, HK_W - 16, HK_H - 5 };
            
            if (x >= info.left && x <= info.right && y >= info.top && y <= info.bottom) {
                // Trigger the existing IDM_OPENCONFIG command in the main tray window
                PostMessageW(g_hwndTray, WM_COMMAND, MAKEWPARAM(IDM_OPENCONFIG, 0), 0);
            }
            break;
        }
        case WM_NCHITTEST: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            if (pt.y >= 0 && pt.y < HK_TITLE_H && pt.x < HK_W - 41) return HTCAPTION; // drag area
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
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void execute_hotkey_settings(HWND hwnd) {
    (void)hwnd; // message-only tray window can't be an owner
    if (g_hwnd_catcher) { SetForegroundWindow(g_hwnd_catcher); return; }

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

    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - HK_W) / 2;
    int y = mi.rcWork.top  + ((mi.rcWork.bottom - mi.rcWork.top) - HK_H) / 2;

    g_hwnd_catcher = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"JxlShotCatcherClass",
        L"Settings", WS_POPUP, x, y, HK_W, HK_H, NULL, NULL, GetModuleHandle(NULL), NULL);

    if (g_hwnd_catcher) {
        ShowWindow(g_hwnd_catcher, SW_SHOW);
        SetForegroundWindow(g_hwnd_catcher);
    }
}

static void show_tray_menu(HWND hwnd) {
    POINT pt; GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_FULL, L"Capture Full Screen");
    AppendMenuW(hMenu, MF_STRING, IDM_REGION, L"Capture Region...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_SETPATH, L"Set Export Path...");
    AppendMenuW(hMenu, MF_STRING, IDM_OPENEXPORT, L"Open Export Folder");
    AppendMenuW(hMenu, MF_STRING, IDM_OPENCONFIG, L"Open Config File");
    AppendMenuW(hMenu, MF_STRING, IDM_HOTKEYS, L"Settings...");
    AppendMenuW(hMenu, MF_STRING, IDM_RELOAD, L"Reload Configuration");
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
    dbg("execute_full_capture: begin (effort=%d, multimonitor=%s)",
        g_cfg.effort, g_cfg.multimonitor ? "separateimages" : "focusmonitor");

    if (g_cfg.multimonitor == 1) {
        MonitorEntry monitors[16];
        int count = enumerate_monitors(monitors, 16);

        if (count <= 0) {
            MessageBoxW(NULL, L"No monitors found.", L"jxlshot", MB_ICONERROR);
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

            if (queue_grab_encode(&g, out_path)) saved++;
            free_grab(&g);
        }

        if (saved == 0) {
            MessageBoxW(NULL, L"Screen capture failed.", L"jxlshot", MB_ICONERROR);
        } else if (saved < count) {
            dbg("execute_full_capture: only %d of %d monitors saved", saved, count);
        }
        return;
    }

    Grab g;
    ZeroMemory(&g, sizeof(g));
    if (!grab_monitor(&g, monitor_under_cursor())) {
        dbg("execute_full_capture: grab failed, showing MessageBox");
        MessageBoxW(NULL, L"Screen capture failed.", L"jxlshot", MB_ICONERROR);
        return;
    }

    wchar_t out_path[MAX_PATH];
    build_out_path(out_path, MAX_PATH, g.is_hdr);
    dbg("execute_full_capture: writing to %ls", out_path);

    if (!queue_grab_encode(&g, out_path)) {
        MessageBoxW(NULL, L"Encoding or saving failed.", L"jxlshot", MB_ICONERROR);
    }
    free_grab(&g);
}

static void execute_open_releases(HWND hwnd) {
    // No version download and no temp file: let the browser do the work.
    ShellExecuteW(hwnd, L"open",
                  L"https://github.com/azizr12/jxlscrenshot/releases/latest",
                  NULL, NULL, SW_SHOWNORMAL);
}

static void execute_set_path(void) {
    BROWSEINFOW bi = { 0 }; bi.hwndOwner = NULL;
    bi.lpszTitle = L"Select Export Folder for Screenshots";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) {
            wcsncpy(g_cfg.export_path, path, MAX_PATH - 1); g_cfg.export_path[MAX_PATH - 1] = L'\0';
            wchar_t ini_path[MAX_PATH]; _snwprintf(ini_path, MAX_PATH, L"%s\\jxlshot.ini", g_exe_dir);
            WritePrivateProfileStringW(L"Capture", L"ExportPath", path, ini_path);
            MessageBoxW(NULL, L"Export path updated.", L"jxlshot", MB_ICONINFORMATION);
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

            RECT dirty;
            region_dirty_rect(&dirty);
            InvalidateRect(hwnd, &dirty, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (g_isDragging) {
                int nx = GET_X_LPARAM(lp);
                int ny = GET_Y_LPARAM(lp);
                if (nx == g_rcSel.right && ny == g_rcSel.bottom) return 0;

                // Repaint only where the old and new selection (plus label) were.
                RECT oldDirty, newDirty, dirty;
                region_dirty_rect(&oldDirty);
                g_rcSel.right = nx;
                g_rcSel.bottom = ny;
                region_dirty_rect(&newDirty);
                UnionRect(&dirty, &oldDirty, &newDirty);
                InvalidateRect(hwnd, &dirty, FALSE);
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
            
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void start_region_capture(void) {
    // Fallback safety: If the flag was somehow stuck, reset it before starting
    g_isRegionCapturing = TRUE; 
    g_blockRightClick = FALSE;
    if (g_hwndTray) KillTimer(g_hwndTray, TIMER_MOUSE_GRACE);

    if (g_hwndRegion) return;

    // Ensure a completely clean slate before starting a new capture
    g_isDragging = FALSE;
    g_rcSel.left = g_rcSel.top = g_rcSel.right = g_rcSel.bottom = 0;

    // Capture the monitor under the cursor once. This exact frame is shown in
    // the overlay and is also the source of the saved region, so HDR survives.
    free_grab(&g_regionFrame);
    HMONITOR target_monitor = monitor_under_cursor();
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(target_monitor, &mi)) {
        dbg("start_region_capture: GetMonitorInfo failed");
        g_isRegionCapturing = FALSE;
        begin_mouse_grace();
        return;
    }

    if (!grab_monitor(&g_regionFrame, target_monitor) ||
        !g_regionFrame.bits || g_regionFrame.w <= 0 || g_regionFrame.h <= 0) {
        dbg("start_region_capture: grab failed");
        free_grab(&g_regionFrame);
        g_isRegionCapturing = FALSE;
        begin_mouse_grace();
        return;
    }

    g_regionMonitorRect = mi.rcMonitor;
    g_screenW = g_regionFrame.w;
    g_screenH = g_regionFrame.h;
    HDC hdcScreen = GetDC(NULL);

    // Original screenshot: SDR preview of the captured frame on the overlay.
    g_hdcMem = CreateCompatibleDC(hdcScreen);
    g_hbmScreen = create_screen_dib(hdcScreen, (void **)&g_screenBits);
    if (!g_hdcMem || !g_hbmScreen || !g_screenBits) {
        if (g_hdcMem)   { DeleteDC(g_hdcMem); g_hdcMem = NULL; }
        if (g_hbmScreen){ DeleteObject(g_hbmScreen); g_hbmScreen = NULL; }
        g_screenBits = NULL;
        free_grab(&g_regionFrame);
        ReleaseDC(NULL, hdcScreen);
        g_isRegionCapturing = FALSE;
        begin_mouse_grace();
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

    // Arial label font and metrics, built once per capture.
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

    ReleaseDC(NULL, hdcScreen);

    WNDCLASSEXW wc = {0}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = RegionWndProc;
    wc.hInstance = GetModuleHandle(NULL); wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    wc.lpszClassName = L"JxlShotRegionClass"; RegisterClassExW(&wc);

    g_hwndRegion = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"JxlShotRegionClass", L"",
                                   WS_POPUP,
                                   g_regionMonitorRect.left, g_regionMonitorRect.top,
                                   g_screenW, g_screenH,
                                   NULL, NULL, GetModuleHandle(NULL), NULL);
    
    // If window creation fails, clean up GDI objects immediately to prevent leaks
    if (!g_hwndRegion) {
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
        return;
    }

    ShowWindow(g_hwndRegion, SW_SHOW); 
    
    // Crucial Fix: Force the overlay to take keyboard focus so it receives WM_KEYDOWN (Escape)
    // We use AttachThreadInput to bypass Windows' strict SetForegroundWindow limitations
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

LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lParam) {
    switch (msg) {
        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) show_tray_menu(hwnd);
            break;
        case WM_HOOK_FULL_CAPTURE: execute_full_capture(); break;
        case WM_HOOK_REGION_CAPTURE: start_region_capture(); break;
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
                case IDM_FULL: execute_full_capture(); break;
                case IDM_REGION: start_region_capture(); break;
                case IDM_SETPATH: execute_set_path(); break;
                case IDM_OPENEXPORT: execute_open_export_folder(); break;
                case IDM_OPENCONFIG: execute_open_config(hwnd); break;
                case IDM_HOTKEYS: execute_hotkey_settings(hwnd); break;
                case IDM_RELOAD: reload_config(); break;
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
    wcscpy(g_nid.szTip, L"JXL Screenshot Tool");

    // Registered before the window exists: explorer.exe broadcasts this message
    // to every top-level window after it starts, which is the only way to know
    // that the notification area was recreated and every icon was lost.
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    tray_icon_ensure(g_hwndTray);
    dbg("tray: startup complete (icon %s)", g_trayIconAdded ? "added" : "pending retry");

    install_keyboard_hook();

    // Route background save notifications to the UI thread from now on.
    g_saveNotify = show_save_notification_ui;
    g_saveOk = notify_save_ok_ui;

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