// gui.c — Snipping Tool style front end for jxlshot.


#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>

#define GUI_DEF_W        600
#define GUI_DEF_H        190
#define GUI_MIN_W        400
#define GUI_MIN_H        96
#define GUI_MAX_W        2000
#define GUI_MAX_H        400

#define GUI_BTN_COUNT    4

// Close button shared by the strip and the preview
#define GUI_CLOSE_W      34
#define GUI_CLOSE_H      24
#define GUI_CLOSE_GLYPH  4
#define GUI_BTN_GAP      4

#define IDC_GUI_FULL     301
#define IDC_GUI_REGION   302
#define IDC_GUI_SETTINGS 303
#define IDC_GUI_EXPORT   304
#define IDC_GUI_ABOUT    306
#define IDC_GUI_CLOSE    307


#define TIMER_GUI_CAPTURE     1
#define TIMER_GUI_REGION_DONE 2
#define GUI_CAPTURE_DELAY_MS  220
#define GUI_REGION_POLL_MS    120

// Capture preview window.

#define PREVIEW_MAX_TEX      4096 // cap the stored bitmap for huge monitors (4K stays 1:1)
#define PREVIEW_X_W          GUI_CLOSE_W   // same button as the strip, flush in the corner
#define PREVIEW_X_H          GUI_CLOSE_H
#define PREVIEW_FULL_BORDER  4    // white frame for full-screen captures
#define PREVIEW_IDLE_MS      1300 // idle time before the close button hides again
#define PREVIEW_TIMER_X      1

#define IDC_PREV_COPY        401
#define IDC_PREV_FOLDER      402
#define IDC_PREV_CLOSE       403
#define IDC_PREV_OPENWITH    404
#define IDC_PREV_RELOAD      405

// EncodeWorker -> preview window: the .jxl file is complete, decode it and
// replace the raw preview. WM_APP is safe here; the preview class is private.
#define WM_PREVIEW_FILE_READY (WM_APP + 12)


// Palette. These are variables, not #defines, so the Dark/Light toggle can swap
// the whole window between themes at runtime. gui_update_theme fills them from
// g_cfg.dark_mode before any painting happens.
static COLORREF g_guiBg, g_guiBtn, g_guiBtnHot, g_guiBtnDown, g_guiBorder;
static COLORREF g_guiAccent, g_guiText, g_guiDim, g_guiCloseBg;

#define GUI_BG        (g_guiBg)
#define GUI_BTN       (g_guiBtn)
#define GUI_BTN_HOT   (g_guiBtnHot)
#define GUI_BTN_DOWN  (g_guiBtnDown)
#define GUI_BORDER    (g_guiBorder)
#define GUI_ACCENT    (g_guiAccent)
#define GUI_TEXT      (g_guiText)
#define GUI_DIM       (g_guiDim)
#define GUI_CLOSE_HOT (g_guiCloseBg)


static int gui_dpi_for_monitor(HMONITOR mon) {
    int dpi = ui_dpi_for_monitor(mon);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        int w = mi.rcMonitor.right - mi.rcMonitor.left;
        int h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        int shortSide = (w < h) ? w : h;
        int resDpi = MulDiv(shortSide, 96, 1080);
        if (resDpi > dpi) dpi = resDpi;
    }
    return dpi;
}

static int gui_dpi_for_window(HWND hwnd) {
    return gui_dpi_for_monitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
}


#define ui_dpi_for_window   gui_dpi_for_window
#define ui_dpi_for_monitor  gui_dpi_for_monitor

typedef struct {
    int   width, height;
    int   dpi;
    int   titleH;
    int   margin, gap;
    int   btnTop, btnW, btnH;
    BOOL  footer;
    BOOL  compact;
    BOOL  shortLabels;
    BOOL  subtitle;
    float iconScale;
} GuiLayout;

static RECT gui_theme_rect(const GuiLayout *L, const RECT *closeRc) {
    RECT r;
    r.right = closeRc->left - ui_scale(L->dpi, 6);
    r.left = r.right - ui_scale(L->dpi, 84);   // "Dark" | "Light" selection bar
    r.top = 0;
    r.bottom = L->titleH;
    return r;
}


static const struct {
    int     id;
    LPCWSTR longText;
    LPCWSTR shortText;
} kGuiButtons[GUI_BTN_COUNT] = {
    { IDC_GUI_FULL,     L"Full Screen", L"Full"     },
    { IDC_GUI_REGION,   L"Region",      L"Region"   },
    { IDC_GUI_SETTINGS, L"Settings",    L"Settings" },
    { IDC_GUI_EXPORT,   L"Open Folder", L"Open"     },
};

static int  g_guiThemeHot = 0;   // 0 none, 1 Dark segment, 2 Light segment

// Win8.1/10 style close glyph

static void gui_draw_close_icon(HDC dc, int cx, int cy, int extent, COLORREF col) {
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    if (!pen) return;
    HPEN oldPen = (HPEN)SelectObject(dc, pen);

    int s = extent * 2;
    int l = cx - extent;
    int t = cy - extent;

    MoveToEx(dc, l, t, NULL);
    LineTo(dc, l + s, t + s);
    MoveToEx(dc, l + s - 1, t, NULL);
    LineTo(dc, l - 1, t + s);

    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

// Flat close button used by both windows
static void gui_draw_close_button(HDC dc, const RECT *rc, BOOL hot, BOOL fillIdle,
                                  COLORREF idleBg, COLORREF idleFg, int extent) {
    if (hot || fillIdle) {
        HBRUSH b = CreateSolidBrush(hot ? RGB(232, 17, 35) : idleBg);
        FillRect(dc, rc, b);
        DeleteObject(b);
    }
    gui_draw_close_icon(dc, (rc->left + rc->right) / 2, (rc->top + rc->bottom) / 2,
                        extent, hot ? RGB(255, 255, 255) : idleFg);
}


static HWND    g_hwndGui = NULL;
static HBRUSH  g_guiBgBrush = NULL;
static HFONT   g_guiTitleFont = NULL;
static HFONT   g_guiCardFont = NULL;
static HFONT   g_guiSmallFont = NULL;
static HFONT   g_guiXFont = NULL;
static HFONT   g_guiTitleFontSm = NULL;
static HFONT   g_guiCardFontSm = NULL;
static HFONT   g_guiSmallFontSm = NULL;
static HFONT   g_guiXFontSm = NULL;
static HWND    g_guiHoverBtn = NULL;
static BOOL    g_guiCloseHot = FALSE;
static BOOL    g_guiCompact = FALSE;
static BOOL    g_guiShortLabels = FALSE;
static BOOL    g_guiShortLabelsSet = FALSE;
static int     g_guiPendingCapture = 0; // 0 = idle, 1 = full, 2 = region

// True while the strip is hidden for a capture it started itself. If a preview
// opens, the strip stays hidden until the preview is closed instead of popping
// back underneath it.
static BOOL    g_guiRestorePending = FALSE;

// Preview window state. The bitmap is a 32-bpp top-down DIB used by the GDI
// fallback and the clipboard code; the normal on-screen path is the D3D
// presenter below, which keeps HDR/SDR intact.
static HWND      g_hwndPreview = NULL;
static HBITMAP   g_prevBmp = NULL;
static void     *g_prevBits = NULL;
static int       g_prevW = 0, g_prevH = 0;      // stored bitmap dimensions
static BITMAPINFO g_prevBmi;
static wchar_t    g_prevPath[MAX_PATH];         // file the capture was saved to
static BOOL       g_prevXShown = FALSE;         // overlay close button visible
static BOOL       g_prevXHot = FALSE;           // mouse over the overlay close button
static int        g_prevBorder = 0;             // white frame width (full-screen captures)
static BOOL       g_prevClosingForCapture = FALSE; // destroy is part of a new grab

// Set once the preview has switched from the raw capture to the decoded .jxl.
static BOOL       g_prevShowingDecoded = FALSE;

// Path handed from the EncodeWorker to the preview window. The worker only
// stores the string here and posts WM_PREVIEW_FILE_READY; decoding and the
// D3D upload stay on the window thread. SRWLOCK_INIT needs no init call, so
// an early-arriving worker is always safe.
static SRWLOCK    g_prevReadyLock = SRWLOCK_INIT;
static wchar_t    g_prevReadyPath[MAX_PATH];

// The preview path is written by the UI thread but compared by the EncodeWorker
// when a save finishes, so both sides go through the lock. Unlocked reads of
// g_prevPath elsewhere are UI-thread only and stay safe.
static void preview_set_path(const wchar_t *path) {
    AcquireSRWLockExclusive(&g_prevReadyLock);
    wcsncpy_s(g_prevPath, MAX_PATH, path ? path : L"", _TRUNCATE);
    ReleaseSRWLockExclusive(&g_prevReadyLock);
}

static void preview_get_path(wchar_t *out) {
    AcquireSRWLockShared(&g_prevReadyLock);
    wcsncpy_s(out, MAX_PATH, g_prevPath, _TRUNCATE);
    ReleaseSRWLockShared(&g_prevReadyLock);
}


// Theme

// Picks the palette for the current g_cfg.dark_mode and rebuilds the window
// background brush. Called on create and whenever the toggle is hit.
static void gui_update_theme(void) {
    if (g_cfg.dark_mode) {
        g_guiBg       = RGB(30, 30, 30);
        g_guiBtn      = RGB(48, 48, 48);
        g_guiBtnHot   = RGB(62, 62, 62);
        g_guiBtnDown  = RGB(76, 76, 76);
        g_guiBorder   = RGB(64, 64, 64);
        g_guiAccent   = RGB(0, 120, 215);
        g_guiText     = RGB(240, 240, 240);
        g_guiDim      = RGB(150, 150, 150);
        g_guiCloseBg  = RGB(232, 17, 35);
    } else {
        g_guiBg       = RGB(246, 246, 246);
        g_guiBtn      = RGB(255, 255, 255);
        g_guiBtnHot   = RGB(229, 239, 251);
        g_guiBtnDown  = RGB(204, 220, 240);
        g_guiBorder   = RGB(205, 205, 205);
        g_guiAccent   = RGB(0, 120, 215);
        g_guiText     = RGB(26, 26, 26);
        g_guiDim      = RGB(110, 110, 110);
        g_guiCloseBg  = RGB(232, 17, 35);
    }

    if (g_guiBgBrush) {
        DeleteObject(g_guiBgBrush);
        g_guiBgBrush = NULL;
    }
    g_guiBgBrush = CreateSolidBrush(g_guiBg);
}

// Persist just the theme choice; used by the Dark/Light toggle.
static void gui_save_theme(void) {
    wchar_t ini[MAX_PATH];
    _snwprintf(ini, MAX_PATH, L"%s\\jxlshot.ini", g_exe_dir);
    ini[MAX_PATH - 1] = L'\0';
    WritePrivateProfileStringW(L"Capture", L"DarkMode",
        g_cfg.dark_mode ? L"1" : L"0", ini);
}

// Persist the theme together with the strip's geometry when the window closes,
// so both survive a restart.
static void gui_save_window_state(void) {
    wchar_t ini[MAX_PATH], s[32];
    _snwprintf(ini, MAX_PATH, L"%s\\jxlshot.ini", g_exe_dir);
    ini[MAX_PATH - 1] = L'\0';

    WritePrivateProfileStringW(L"Capture", L"DarkMode",
        g_cfg.dark_mode ? L"1" : L"0", ini);

    _snwprintf(s, 32, L"%d", g_cfg.gui_x); WritePrivateProfileStringW(L"Capture", L"GuiX", s, ini);
    _snwprintf(s, 32, L"%d", g_cfg.gui_y); WritePrivateProfileStringW(L"Capture", L"GuiY", s, ini);
    _snwprintf(s, 32, L"%d", g_cfg.gui_w); WritePrivateProfileStringW(L"Capture", L"GuiW", s, ini);
    _snwprintf(s, 32, L"%d", g_cfg.gui_h); WritePrivateProfileStringW(L"Capture", L"GuiH", s, ini);
}


// Layout


static void gui_get_layout(HWND hwnd, GuiLayout *L) {
    RECT rc;
    GetClientRect(hwnd, &rc);

    ZeroMemory(L, sizeof(*L));
    L->width = rc.right;
    L->height = rc.bottom;
    L->dpi = ui_dpi_for_window(hwnd);

    // One UI only: the compact strip with the small title bar and icons. It
    // stays exactly like this at every window size, so expanding the window
    // just gives the same four cards more room. Metrics are 96-DPI design
    // values scaled to the monitor the window is on.
    L->footer = FALSE;
    L->compact = TRUE;
    L->titleH = ui_scale(L->dpi, 24);
    L->margin = ui_scale(L->dpi, 8);
    L->gap = ui_scale(L->dpi, 4);

    int topPad = ui_scale(L->dpi, 4);
    int bottomPad = ui_scale(L->dpi, 4);
    L->btnTop = L->titleH + topPad;
    L->btnH = L->height - bottomPad - L->btnTop;
    if (L->btnH < ui_scale(L->dpi, 16)) L->btnH = ui_scale(L->dpi, 16);

    L->btnW = (L->width - 2 * L->margin - (GUI_BTN_COUNT - 1) * L->gap) / GUI_BTN_COUNT;
    if (L->btnW < ui_scale(L->dpi, 10)) L->btnW = ui_scale(L->dpi, 10);

    L->shortLabels = (L->btnW < ui_scale(L->dpi, 90));
    L->subtitle = FALSE;

    // Icons scale with the display DPI
    float base = (float)L->dpi / 96.0f;
    float fit  = (float)((L->btnW < L->btnH) ? L->btnW : L->btnH) / (100.0f * base);
    if (fit < 1.0f) fit = 1.0f;
    if (fit > 2.5f) fit = 2.5f;
    L->iconScale = base * fit;
}

// Close button: a flush rectangle in the top-right corner, no rounding, no gap.
static RECT gui_close_rect(const GuiLayout *L) {
    RECT r;
    r.right  = L->width;          // touches the right edge
    r.left   = r.right - ui_scale(L->dpi, GUI_CLOSE_W);
    r.top    = 0;                 // touches the top edge
    r.bottom = L->titleH;
    return r;
}

// Keeps the strip completely inside one physical monitor and inside the size
// limits. Used while moving/resizing as well as before the window is created,
// so the title bar can never be dragged past a screen edge and the window can
// never grow taller than GUI_MAX_H.
static void gui_clamp_to_monitor(RECT *r) {
    HMONITOR mon = MonitorFromRect(r, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;

    RECT m = mi.rcMonitor;
    int availW = m.right - m.left;
    int availH = m.bottom - m.top;

    int dpi = ui_dpi_for_monitor(mon);
    int maxW = ui_scale(dpi, GUI_MAX_W);
    int maxH = ui_scale(dpi, GUI_MAX_H);
    int minW = ui_scale(dpi, GUI_MIN_W);
    int minH = ui_scale(dpi, GUI_MIN_H);

    int w = r->right - r->left;
    int h = r->bottom - r->top;

    if (w > maxW) w = maxW;
    if (h > maxH) h = maxH;
    if (w > availW)    w = availW;
    if (h > availH)    h = availH;
    if (w < minW) w = minW;
    if (h < minH) h = minH;
    if (w > availW)    w = availW;   // monitor narrower than the minimum
    if (h > availH)    h = availH;

    int x = r->left;
    int y = r->top;
    if (x + w > m.right)  x = m.right - w;
    if (y + h > m.bottom) y = m.bottom - h;
    if (x < m.left) x = m.left;
    if (y < m.top)  y = m.top;

    r->left = x;
    r->top = y;
    r->right = x + w;
    r->bottom = y + h;
}

static void gui_layout_children(HWND hwnd) {
    GuiLayout L;
    gui_get_layout(hwnd, &L);

    if (!g_guiShortLabelsSet || g_guiShortLabels != L.shortLabels) {
        for (int i = 0; i < GUI_BTN_COUNT; i++) {
            HWND hb = GetDlgItem(hwnd, kGuiButtons[i].id);
            if (hb) {
                SetWindowTextW(hb, L.shortLabels ? kGuiButtons[i].shortText
                                                 : kGuiButtons[i].longText);
            }
        }
        g_guiShortLabels = L.shortLabels;
        g_guiShortLabelsSet = TRUE;
    }

    g_guiCompact = L.compact;

    int x = L.margin;
    for (int i = 0; i < GUI_BTN_COUNT; i++) {
        HWND hb = GetDlgItem(hwnd, kGuiButtons[i].id);
        if (hb) MoveWindow(hb, x, L.btnTop, L.btnW, L.btnH, TRUE);
        x += L.btnW + L.gap;
    }
}


// Drawing helpers


static void gui_round_rect(HDC dc, const RECT *rc, int radius, COLORREF fill, COLORREF border) {
    HRGN rgn = CreateRoundRectRgn(rc->left, rc->top, rc->right + 1, rc->bottom + 1,
                                  radius, radius);
    if (!rgn) return;

    HBRUSH b = CreateSolidBrush(fill);
    FillRgn(dc, rgn, b);
    DeleteObject(b);

    if (border != fill) {
        HBRUSH bb = CreateSolidBrush(border);
        FrameRgn(dc, rgn, bb, 1, 1);
        DeleteObject(bb);
    }
    DeleteObject(rgn);
}

// Full-screen glyph
static void gui_draw_monitor_icon(HDC dc, int cx, int cy, COLORREF col) {
    HPEN pen = CreatePen(PS_SOLID, 2, col);
    if (!pen) return;
    HPEN oldPen = (HPEN)SelectObject(dc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));

    Rectangle(dc, cx - 16, cy - 13, cx + 16, cy + 9);
    MoveToEx(dc, cx, cy + 9, NULL);
    LineTo(dc, cx, cy + 16);
    MoveToEx(dc, cx - 7, cy + 16, NULL);
    LineTo(dc, cx + 7, cy + 16);

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

// Region glyph
static void gui_draw_region_icon(HDC dc, int cx, int cy, COLORREF col) {
    RECT box = { cx - 16, cy - 13, cx + 16, cy + 14 };
    const int len = 9;

    HPEN pen = CreatePen(PS_SOLID, 2, col);
    if (!pen) return; // Fail gracefully if pen creation fails

    HPEN oldPen = (HPEN)SelectObject(dc, pen);

    MoveToEx(dc, box.left, box.top + len, NULL);
    LineTo(dc, box.left, box.top);
    LineTo(dc, box.left + len, box.top);
    MoveToEx(dc, box.right - len, box.top, NULL);
    LineTo(dc, box.right, box.top);
    LineTo(dc, box.right, box.top + len);
    MoveToEx(dc, box.left, box.bottom - len, NULL);
    LineTo(dc, box.left, box.bottom);
    LineTo(dc, box.left + len, box.bottom);
    MoveToEx(dc, box.right - len, box.bottom, NULL);
    LineTo(dc, box.right, box.bottom);
    LineTo(dc, box.right, box.bottom - len);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

// Settings glyph
static void gui_draw_sliders_icon(HDC dc, int cx, int cy, COLORREF col) {
    HPEN pen = CreatePen(PS_SOLID, 2, col);
    if (!pen) return;
    HPEN oldPen = (HPEN)SelectObject(dc, pen);

    for (int i = -1; i <= 1; i++) {
        int y = cy + i * 9;
        MoveToEx(dc, cx - 15, y, NULL);
        LineTo(dc, cx + 15, y);
    }

    HBRUSH knob = CreateSolidBrush(col);
    RECT k1 = { cx - 8, cy - 9 - 3, cx - 2, cy - 9 + 3 };
    RECT k2 = { cx + 2, cy - 3,      cx + 8, cy + 3 };
    RECT k3 = { cx - 5, cy + 9 - 3,  cx + 1, cy + 9 + 3 };
    FillRect(dc, &k1, knob);
    FillRect(dc, &k2, knob);
    FillRect(dc, &k3, knob);
    DeleteObject(knob);

    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

static void gui_draw_folder_icon(HDC dc, int cx, int cy, COLORREF col) {
    HPEN pen = CreatePen(PS_SOLID, 2, col);
    if (!pen) return;

    HPEN oldPen = (HPEN)SelectObject(dc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));

    const POINT f[7] = {
        { cx - 15, cy - 8 }, { cx - 5, cy - 8 }, { cx - 1, cy - 4 },
        { cx + 14, cy - 4 }, { cx + 14, cy + 10 }, { cx - 15, cy + 10 },
        { cx - 15, cy - 8 }
    };
    for (int i = 0; i < 6; i++) {
        MoveToEx(dc, f[i].x, f[i].y, NULL);
        LineTo(dc, f[i + 1].x, f[i + 1].y);
    }

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

static void gui_draw_icon_raw(HDC dc, int id, int cx, int cy, COLORREF col) {
    switch (id) {
        case IDC_GUI_FULL:     gui_draw_monitor_icon(dc, cx, cy, col); break;
        case IDC_GUI_REGION:   gui_draw_region_icon(dc, cx, cy, col); break;
        case IDC_GUI_SETTINGS: gui_draw_sliders_icon(dc, cx, cy, col); break;
        case IDC_GUI_EXPORT:   gui_draw_folder_icon(dc, cx, cy, col); break;
    }
}

// Draw the fixed-size glyph scaled around (cx, cy). Scale is 1.0 in the one
// UI the strip uses, so the icons never grow with the window.
static void gui_draw_icon(HDC dc, int id, int cx, int cy, COLORREF col, float scale) {
    if (scale < 0.05f) scale = 1.0f;

    int saved = SaveDC(dc);
    if (saved) {
        XFORM xf;
        xf.eM11 = scale;
        xf.eM12 = 0.0f;
        xf.eM21 = 0.0f;
        xf.eM22 = scale;
        xf.eDx = (FLOAT)cx * (1.0f - scale);
        xf.eDy = (FLOAT)cy * (1.0f - scale);

        SetGraphicsMode(dc, GM_ADVANCED);
        SetWorldTransform(dc, &xf);
        gui_draw_icon_raw(dc, id, cx, cy, col);
        RestoreDC(dc, saved);
    } else {
        gui_draw_icon_raw(dc, id, cx, cy, col);
    }
}

// Capture buttons show their hotkey underneath the label. A disabled hotkey is
// spelled out instead of the settings dialog's "Click to set" wording.
static void gui_hotkey_hint(wchar_t *out, size_t n, UINT vk, UINT mod) {
    if (vk == 0) {
        wcsncpy_s(out, n, L"No hotkey", _TRUNCATE);
        return;
    }
    format_hotkey_string(vk, mod, out, n);
}

static void gui_button_subtitle(int id, wchar_t *out, size_t n) {
    switch (id) {
        case IDC_GUI_FULL:
            gui_hotkey_hint(out, n, g_cfg.hk_full_vk, g_cfg.hk_full_mod);
            break;
        case IDC_GUI_REGION:
            gui_hotkey_hint(out, n, g_cfg.hk_region_vk, g_cfg.hk_region_mod);
            break;
        case IDC_GUI_SETTINGS:
            wcsncpy_s(out, n, L"Preferences", _TRUNCATE);
            break;
        case IDC_GUI_EXPORT:
            wcsncpy_s(out, n, L"Show exports", _TRUNCATE);
            break;
        default:
            out[0] = L'\0';
            break;
    }
}

static void gui_draw_button(LPDRAWITEMSTRUCT di) {
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    int id = (int)di->CtlID;
    BOOL hot = (g_guiHoverBtn == di->hwndItem);
    BOOL down = (di->itemState & ODS_SELECTED) != 0;

    GuiLayout L;
    gui_get_layout(GetParent(di->hwndItem), &L);

    float fit = L.iconScale / ((float)L.dpi / 96.0f);

    COLORREF bg = down ? GUI_BTN_DOWN : (hot ? GUI_BTN_HOT : GUI_BTN);
    COLORREF border = hot ? GUI_ACCENT : GUI_BORDER;

    gui_round_rect(dc, &rc, (int)(ui_scale(L.dpi, 8) * fit + 0.5f), bg, border);

    wchar_t label[128];
    GetWindowTextW(di->hwndItem, label, 128);

    SetBkMode(dc, TRANSPARENT);

    // Font created here grows with the buttons.
    int labelPx = (int)(ui_scale(L.dpi, 12) * fit + 0.5f);
    HFONT labelFont = CreateFontW(-labelPx, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    if (!labelFont) labelFont = g_guiCardFontSm;

    int smallPx = (int)(ui_scale(L.dpi, 9) * fit + 0.5f);
    HFONT smallFont = g_guiSmallFontSm;
    HFONT smallOwned = NULL;
    if (L.subtitle) {
        smallOwned = CreateFontW(-smallPx, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        if (smallOwned) smallFont = smallOwned;
    }

    HFONT old = (HFONT)SelectObject(dc, labelFont);
    TEXTMETRICW tmLabel;
    GetTextMetricsW(dc, &tmLabel);

    int iconPx = (int)(32.0f * L.iconScale + 0.5f);
    int subH = 0;
    if (L.subtitle) {
        HFONT prev = (HFONT)SelectObject(dc, smallFont);
        TEXTMETRICW tmSub;
        GetTextMetricsW(dc, &tmSub);
        subH = tmSub.tmHeight;
        SelectObject(dc, prev);
    }

    int gapIconText = (int)(ui_scale(L.dpi, 5) * fit + 0.5f);
    int gapLabelSub = ui_scale(L.dpi, 2);
    int blockH = iconPx + gapIconText + tmLabel.tmHeight
               + (L.subtitle ? gapLabelSub + subH : 0);

    int blockTop = rc.top + ((rc.bottom - rc.top) - blockH) / 2;
    if (blockTop < rc.top + ui_scale(L.dpi, 2)) blockTop = rc.top + ui_scale(L.dpi, 2);

    int cx = (rc.left + rc.right) / 2;
    int iconCy = blockTop + iconPx / 2;
    gui_draw_icon(dc, id, cx, iconCy, hot ? GUI_ACCENT : GUI_TEXT, L.iconScale);

    int padX = ui_scale(L.dpi, 4);
    RECT lr = { rc.left + padX, blockTop + iconPx + gapIconText,
                rc.right - padX, blockTop + iconPx + gapIconText + tmLabel.tmHeight };
    SetTextColor(dc, GUI_TEXT);
    DrawTextW(dc, label, -1, &lr,
              DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);

    if (L.subtitle) {
        wchar_t subtitle[128];
        gui_button_subtitle(id, subtitle, 128);

        SelectObject(dc, smallFont);
        RECT sr = { rc.left + padX, lr.bottom + gapLabelSub,
                    rc.right - padX, lr.bottom + gapLabelSub + subH };
        SetTextColor(dc, GUI_DIM);
        DrawTextW(dc, subtitle, -1, &sr,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
    }

    SelectObject(dc, old);
    if (labelFont != g_guiCardFontSm) DeleteObject(labelFont);
    if (smallOwned) DeleteObject(smallOwned);
}

// Hover tracking and Escape forwarding for the owner-drawn buttons.
static LRESULT CALLBACK GuiBtnSubclass(HWND h, UINT m, WPARAM w, LPARAM l,
                                      UINT_PTR id, DWORD_PTR ref) {
    (void)ref;
    switch (m) {
        case WM_MOUSEMOVE:
            if (g_guiHoverBtn != h) {
                g_guiHoverBtn = h;
                TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
                TrackMouseEvent(&t);
                InvalidateRect(h, NULL, FALSE);
            }
            break;

        case WM_MOUSELEAVE:
            if (g_guiHoverBtn == h) g_guiHoverBtn = NULL;
            InvalidateRect(h, NULL, FALSE);
            break;

        case WM_SETCURSOR:
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;

        case WM_KEYDOWN:
            // Buttons take focus when clicked; keep Escape working.
            if (w == VK_ESCAPE) {
                PostMessageW(GetParent(h), WM_KEYDOWN, VK_ESCAPE, 0);
                return 0;
            }
            break;

        case WM_NCDESTROY:
            RemoveWindowSubclass(h, GuiBtnSubclass, id);
            break;
    }
    return DefSubclassProc(h, m, w, l);
}



// Capture preview

// in a frameless, shadowless window
// The image itself is the drag surface.


static LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// The window is exactly the image (plus the white frame for full-screen shots).
static RECT preview_image_rect(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    InflateRect(&rc, -g_prevBorder, -g_prevBorder);
    return rc;
}

// Close button: flush in the top-right corner, scaled with the display DPI.
static RECT preview_x_rect(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int dpi = ui_dpi_for_window(hwnd);
    int w = ui_scale(dpi, PREVIEW_X_W);
    int h = ui_scale(dpi, PREVIEW_X_H);
    RECT r = { rc.right - w, 0, rc.right, h };
    if (r.left < 0) r.left = 0;
    return r;
}

// Linear scRGB -> 8-bit sRGB lookup. Used only for the clipboard DIB and the
// GDI fallback; the on-screen preview keeps the original FP16 values.
static const uint8_t *preview_srgb_lut(void) {
    static uint8_t lut[1025];
    static BOOL built = FALSE;
    if (!built) {
        for (int i = 0; i <= 1024; i++) {
            float c = (float)i / 1024.0f;
            float s = (c <= 0.0031308f)
                ? 12.92f * c
                : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
            int v = (int)(s * 255.0f + 0.5f);
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            lut[i] = (uint8_t)v;
        }
        built = TRUE;
    }
    return lut;
}

// GPU presentation for the preview window.

// GDI only understands 8-bit SDR,
// so drawing the captured frame through a DIB
// flattened an HDR (FP16 scRGB) capture to sRGB before it ever reached the screen

// The preview is therefore presented through D3D11 flip-model swap chain
// FP16 scRGB chain for HDR captures and a B8G8R8A8 sRGB chain for SDR captures.
// The GDI bitmap is still built as the clipboard source
// and as the fallback when D3D is unavailable.


typedef struct {
    float img[4];    // left, top, right, bottom of the image in client pixels
    float xbtn[4];   // close button rectangle (empty when it is hidden)
    float ui_gamma;  // 1.0 for the sRGB chain, 2.2 to lift UI into linear scRGB
    float pad[3];
} PreviewGpuCB;

typedef struct {
    ID3D11Device             *device;
    ID3D11DeviceContext      *ctx;
    IDXGISwapChain3          *swap;
    ID3D11RenderTargetView   *rtv;
    ID3D11Texture2D          *src_tex;
    ID3D11ShaderResourceView *src_srv;
    ID3D11Texture2D          *x_tex;
    ID3D11ShaderResourceView *x_srv;
    ID3D11VertexShader       *vs;
    ID3D11PixelShader        *ps;
    ID3D11Buffer             *cb;
    ID3D11SamplerState       *samp;
    int is_hdr;
    int swap_w, swap_h;
    int x_tex_w, x_tex_h;
    HDC x_dc;
    HBITMAP x_bmp;
    HGDIOBJ x_old;
    void *x_bits;
} PreviewGpu;

static PreviewGpu g_prevGpu;
static BOOL       g_prevGpuActive = FALSE;

static const char k_preview_shader[] =
    "cbuffer P : register(b0) {\n"
    "  float4 img; float4 xbtn; float ui_gamma; float3 pad;\n"
    "};\n"
    "Texture2D<float4> Src : register(t0);\n"
    "Texture2D<float4> Xbt : register(t1);\n"
    "SamplerState Samp : register(s0);\n"
    "struct V { float4 pos : SV_Position; };\n"
    "V VS(uint id : SV_VertexID) {\n"
    "  V o;\n"
    // Same clockwise full-viewport triangle as the region overlay: the default
    // rasterizer culls counter-clockwise faces and would leave the window black.
    "  o.pos = float4((id == 1) ? 3.0 : -1.0, (id == 2) ? -3.0 : 1.0, 0.0, 1.0);\n"
    "  return o;\n"
    "}\n"
    "float4 PS(V i) : SV_Target {\n"
    "  float2 p = i.pos.xy;\n"
    "  float3 c = float3(1.0, 1.0, 1.0);\n" // white frame / background
    "  if (img.z > img.x && img.w > img.y &&\n"
    "      p.x >= img.x && p.x < img.z && p.y >= img.y && p.y < img.w) {\n"
    "    float2 uv = (p - img.xy) / (img.zw - img.xy);\n"
    "    c = Src.SampleLevel(Samp, uv, 0.0).rgb;\n"
    "  }\n"
    "  if (xbtn.z > xbtn.x && p.x >= xbtn.x && p.x < xbtn.z &&\n"
    "      p.y >= xbtn.y && p.y < xbtn.w) {\n"
    "    float2 uv = (p - xbtn.xy) / (xbtn.zw - xbtn.xy);\n"
    "    float3 t = Xbt.SampleLevel(Samp, uv, 0.0).rgb;\n"
    // The button DIB is sRGB; the scRGB target is linear.
    "    c = pow(max(t, 0.0), ui_gamma);\n"
    "  }\n"
    "  return float4(c, 1.0);\n"
    "}\n";

static ID3DBlob *preview_gpu_compile(const char *entry, const char *target) {
    static HMODULE     compiler = NULL;
    static pD3DCompile compile_fn = NULL;
    static int         tried = 0;

    if (!tried) {
        tried = 1;
        compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        if (compiler) {
            compile_fn = (pD3DCompile)(void *)GetProcAddress(compiler, "D3DCompile");
        }
    }
    if (!compile_fn) {
        dbg("preview: D3DCompile unavailable, keeping GDI fallback");
        return NULL;
    }

    ID3DBlob *code = NULL;
    ID3DBlob *err = NULL;
    HRESULT hr = compile_fn(k_preview_shader, sizeof k_preview_shader - 1,
                            NULL, NULL, NULL, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        dbg("preview: shader %s compile failed (0x%08lX) %s", entry, hr,
            err ? (const char *)err->lpVtbl->GetBufferPointer(err) : "");
        if (err) err->lpVtbl->Release(err);
        if (code) code->lpVtbl->Release(code);
        return NULL;
    }
    if (err) err->lpVtbl->Release(err);
    return code;
}

static void preview_gpu_destroy(void) {
    g_prevGpuActive = FALSE;

    if (g_prevGpu.ctx) g_prevGpu.ctx->lpVtbl->ClearState(g_prevGpu.ctx);
    if (g_prevGpu.rtv)      g_prevGpu.rtv->lpVtbl->Release(g_prevGpu.rtv);
    if (g_prevGpu.src_srv)  g_prevGpu.src_srv->lpVtbl->Release(g_prevGpu.src_srv);
    if (g_prevGpu.src_tex)  g_prevGpu.src_tex->lpVtbl->Release(g_prevGpu.src_tex);
    if (g_prevGpu.x_srv)    g_prevGpu.x_srv->lpVtbl->Release(g_prevGpu.x_srv);
    if (g_prevGpu.x_tex)    g_prevGpu.x_tex->lpVtbl->Release(g_prevGpu.x_tex);
    if (g_prevGpu.samp)     g_prevGpu.samp->lpVtbl->Release(g_prevGpu.samp);
    if (g_prevGpu.cb)       g_prevGpu.cb->lpVtbl->Release(g_prevGpu.cb);
    if (g_prevGpu.vs)       g_prevGpu.vs->lpVtbl->Release(g_prevGpu.vs);
    if (g_prevGpu.ps)       g_prevGpu.ps->lpVtbl->Release(g_prevGpu.ps);
    if (g_prevGpu.swap)     g_prevGpu.swap->lpVtbl->Release(g_prevGpu.swap);
    if (g_prevGpu.ctx)      g_prevGpu.ctx->lpVtbl->Release(g_prevGpu.ctx);
    if (g_prevGpu.device)   g_prevGpu.device->lpVtbl->Release(g_prevGpu.device);

    if (g_prevGpu.x_dc && g_prevGpu.x_old)
        SelectObject(g_prevGpu.x_dc, g_prevGpu.x_old);
    if (g_prevGpu.x_dc)  { DeleteDC(g_prevGpu.x_dc); g_prevGpu.x_dc = NULL; }
    if (g_prevGpu.x_bmp) { DeleteObject(g_prevGpu.x_bmp); g_prevGpu.x_bmp = NULL; }

    ZeroMemory(&g_prevGpu, sizeof g_prevGpu);
}

// Upload one frame into the preview texture.
//
// `src` is packed 3-channel RGB: half floats in linear scRGB for HDR, sRGB
// bytes for SDR. The source may be larger than the stored preview bitmap, so it
// is point-sampled down to dw x dh and 6K/8K frames stay bounded. The new
// texture is only swapped in after every step succeeded: a failed decode or
// upload leaves the previous frame on screen instead of going black.
static int preview_gpu_upload(const void *src, int sw, int sh,
                              int dw, int dh, int is_hdr) {
    if (!g_prevGpu.device || !g_prevGpu.ctx) return 0;
    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return 0;
    if (is_hdr != g_prevGpu.is_hdr) {
        dbg("preview: upload colour space mismatch (chain %s, frame %s)",
            g_prevGpu.is_hdr ? "HDR" : "SDR", is_hdr ? "HDR" : "SDR");
        return 0;
    }

    D3D11_TEXTURE2D_DESC td;
    ZeroMemory(&td, sizeof td);
    td.Width = (UINT)dw;
    td.Height = (UINT)dh;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = is_hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT
                       : DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    ID3D11Texture2D *tex = NULL;
    ID3D11ShaderResourceView *srv = NULL;

    if (FAILED(g_prevGpu.device->lpVtbl->CreateTexture2D(
            g_prevGpu.device, &td, NULL, &tex))) {
        dbg("preview: CreateTexture2D(source) failed");
        return 0;
    }

    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g_prevGpu.ctx->lpVtbl->Map(
            g_prevGpu.ctx, (ID3D11Resource *)tex,
            0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        dbg("preview: Map(source) failed");
        tex->lpVtbl->Release(tex);
        return 0;
    }

    const int bpp = is_hdr ? 6 : 3;
    const uint8_t *base = (const uint8_t *)src;

    for (int y = 0; y < dh; y++) {
        int sy = (dw == sw && dh == sh) ? y : (int)((long long)y * sh / dh);
        if (sy >= sh) sy = sh - 1;
        const uint8_t *srow = base + (size_t)sy * sw * bpp;
        uint8_t *drow = (uint8_t *)map.pData + (size_t)y * map.RowPitch;

        for (int x = 0; x < dw; x++) {
            int sx = (dw == sw && dh == sh) ? x : (int)((long long)x * sw / dw);
            if (sx >= sw) sx = sw - 1;
            const uint8_t *sp = srow + (size_t)sx * bpp;
            uint8_t *dp = drow + (size_t)x * 4;

            if (is_hdr) {
                dp[0] = sp[0]; dp[1] = sp[1];   // R (half)
                dp[2] = sp[2]; dp[3] = sp[3];   // G
                dp[4] = sp[4]; dp[5] = sp[5];   // B
                dp[6] = 0x00;  dp[7] = 0x3C;    // A = 1.0h
            } else {
                dp[0] = sp[2]; // B
                dp[1] = sp[1]; // G
                dp[2] = sp[0]; // R
                dp[3] = 255;
            }
        }
    }

    g_prevGpu.ctx->lpVtbl->Unmap(g_prevGpu.ctx, (ID3D11Resource *)tex, 0);

    D3D11_SHADER_RESOURCE_VIEW_DESC svd;
    ZeroMemory(&svd, sizeof svd);
    svd.Format = td.Format;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    if (FAILED(g_prevGpu.device->lpVtbl->CreateShaderResourceView(
            g_prevGpu.device, (ID3D11Resource *)tex, &svd, &srv))) {
        dbg("preview: CreateShaderResourceView(source) failed");
        tex->lpVtbl->Release(tex);
        return 0;
    }

    // Success: replace the previous frame.
    if (g_prevGpu.src_srv) {
        g_prevGpu.src_srv->lpVtbl->Release(g_prevGpu.src_srv);
        g_prevGpu.src_srv = NULL;
    }
    if (g_prevGpu.src_tex) {
        g_prevGpu.src_tex->lpVtbl->Release(g_prevGpu.src_tex);
        g_prevGpu.src_tex = NULL;
    }
    g_prevGpu.src_tex = tex;
    g_prevGpu.src_srv = srv;
    return 1;
}

// Upload the captured frame. The grab is packed 3-channel RGB: half floats in
// linear scRGB for HDR, sRGB bytes for SDR. preview_make_bitmap() has already
// sized the on-screen texture (g_prevW/g_prevH), so those bounds are reused.
static int preview_gpu_upload_source(const Grab *g) {
    if (!g || !g->bits) return 0;
    return preview_gpu_upload(g->bits, g->w, g->h, g_prevW, g_prevH, g->is_hdr);
}

// Redraw the close button into its DIB and upload it. Called each time the
// button's visibility or hover state can have changed; the DIB is tiny.
static int preview_gpu_update_x_texture(void) {
    if (!g_prevGpu.ctx || !g_prevGpu.x_tex) return 0;

    int xw = g_prevGpu.x_tex_w, xh = g_prevGpu.x_tex_h;
    if (xw <= 0 || xh <= 0) return 0;

    if (!g_prevGpu.x_dc || !g_prevGpu.x_bmp) {
        HDC screen = GetDC(NULL);
        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof bi);
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = xw;
        bi.bmiHeader.biHeight = -xh; // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void *bits = NULL;
        g_prevGpu.x_bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        g_prevGpu.x_dc = CreateCompatibleDC(screen);
        ReleaseDC(NULL, screen);

        if (!g_prevGpu.x_dc || !g_prevGpu.x_bmp || !bits) {
            dbg("preview: close button DIB creation failed");
            return 0;
        }
        g_prevGpu.x_bits = bits;
        g_prevGpu.x_old = SelectObject(g_prevGpu.x_dc, g_prevGpu.x_bmp);
    }

    // The texture is already scaled to the monitor DPI,
    // so the glyph extent is scaled by the same ratio as the button.
    int glyph = MulDiv(GUI_CLOSE_GLYPH, xw, PREVIEW_X_W);
    if (glyph < 1) glyph = 1;

    RECT r = { 0, 0, xw, xh };
    gui_draw_close_button(g_prevGpu.x_dc, &r, g_prevXHot, TRUE,
                          RGB(32, 32, 32), RGB(255, 255, 255), glyph);

    // GDI leaves the alpha bytes alone; the shader uses the RGB of an opaque
    // button, so force every pixel opaque.
    uint8_t *px = (uint8_t *)g_prevGpu.x_bits;
    for (size_t i = 0, n = (size_t)xw * xh; i < n; i++, px += 4)
        px[3] = 255;

    g_prevGpu.ctx->lpVtbl->UpdateSubresource(
        g_prevGpu.ctx, (ID3D11Resource *)g_prevGpu.x_tex, 0, NULL,
        g_prevGpu.x_bits, (UINT)xw * 4, 0);
    return 1;
}

static void preview_gpu_present(HWND hwnd) {
    if (!g_prevGpuActive || !g_prevGpu.ctx || !g_prevGpu.swap) return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    RECT img = preview_image_rect(hwnd);

    PreviewGpuCB cb;
    ZeroMemory(&cb, sizeof cb);
    cb.img[0] = (float)img.left;
    cb.img[1] = (float)img.top;
    cb.img[2] = (float)img.right;
    cb.img[3] = (float)img.bottom;
    cb.ui_gamma = g_prevGpu.is_hdr ? 2.2f : 1.0f;

    if (g_prevXShown) {
        RECT xr = preview_x_rect(hwnd);
        cb.xbtn[0] = (float)xr.left;
        cb.xbtn[1] = (float)xr.top;
        cb.xbtn[2] = (float)xr.right;
        cb.xbtn[3] = (float)xr.bottom;
        preview_gpu_update_x_texture();
    }

    D3D11_MAPPED_SUBRESOURCE cbmap;
    if (SUCCEEDED(g_prevGpu.ctx->lpVtbl->Map(
            g_prevGpu.ctx, (ID3D11Resource *)g_prevGpu.cb,
            0, D3D11_MAP_WRITE_DISCARD, 0, &cbmap))) {
        memcpy(cbmap.pData, &cb, sizeof cb);
        g_prevGpu.ctx->lpVtbl->Unmap(g_prevGpu.ctx, (ID3D11Resource *)g_prevGpu.cb, 0);
    }

    D3D11_VIEWPORT vp;
    ZeroMemory(&vp, sizeof vp);
    vp.Width = (float)(rc.right - rc.left);
    vp.Height = (float)(rc.bottom - rc.top);
    vp.MaxDepth = 1.0f;

    ID3D11ShaderResourceView *srvs[2] = { g_prevGpu.src_srv, g_prevGpu.x_srv };
    g_prevGpu.ctx->lpVtbl->OMSetRenderTargets(g_prevGpu.ctx, 1, &g_prevGpu.rtv, NULL);
    g_prevGpu.ctx->lpVtbl->RSSetViewports(g_prevGpu.ctx, 1, &vp);
    g_prevGpu.ctx->lpVtbl->IASetPrimitiveTopology(
        g_prevGpu.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_prevGpu.ctx->lpVtbl->VSSetShader(g_prevGpu.ctx, g_prevGpu.vs, NULL, 0);
    g_prevGpu.ctx->lpVtbl->PSSetShader(g_prevGpu.ctx, g_prevGpu.ps, NULL, 0);
    g_prevGpu.ctx->lpVtbl->PSSetConstantBuffers(g_prevGpu.ctx, 0, 1, &g_prevGpu.cb);
    g_prevGpu.ctx->lpVtbl->PSSetShaderResources(g_prevGpu.ctx, 0, 2, srvs);
    g_prevGpu.ctx->lpVtbl->PSSetSamplers(g_prevGpu.ctx, 0, 1, &g_prevGpu.samp);
    g_prevGpu.ctx->lpVtbl->Draw(g_prevGpu.ctx, 3, 0);

    HRESULT hr = g_prevGpu.swap->lpVtbl->Present(g_prevGpu.swap, 1, 0);
    if (FAILED(hr)) dbg("preview: Present failed hr=0x%08lX", hr);
}

// Keep the swap chain matching the window if the frame size changes
// (DPI change, or a new capture reusing an open preview window).
static void preview_gpu_resize(HWND hwnd) {
    if (!g_prevGpuActive || !g_prevGpu.swap || !g_prevGpu.device) return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    UINT w = (UINT)(rc.right - rc.left);
    UINT h = (UINT)(rc.bottom - rc.top);
    if (w < 1 || h < 1) return;
    if ((int)w == g_prevGpu.swap_w && (int)h == g_prevGpu.swap_h) return;

    if (g_prevGpu.rtv) {
        g_prevGpu.rtv->lpVtbl->Release(g_prevGpu.rtv);
        g_prevGpu.rtv = NULL;
    }

    DXGI_FORMAT fmt = g_prevGpu.is_hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                       : DXGI_FORMAT_B8G8R8A8_UNORM;
    if (FAILED(g_prevGpu.swap->lpVtbl->ResizeBuffers(g_prevGpu.swap, 2, w, h, fmt, 0))) {
        dbg("preview: ResizeBuffers failed, falling back to GDI");
        preview_gpu_destroy();
        return;
    }

    ID3D11Texture2D *backbuffer = NULL;
    if (FAILED(g_prevGpu.swap->lpVtbl->GetBuffer(
            g_prevGpu.swap, 0, &IID_ID3D11Texture2D, (void **)&backbuffer))) {
        dbg("preview: GetBuffer failed after resize");
        preview_gpu_destroy();
        return;
    }
    HRESULT hr = g_prevGpu.device->lpVtbl->CreateRenderTargetView(
        g_prevGpu.device, (ID3D11Resource *)backbuffer, NULL, &g_prevGpu.rtv);
    backbuffer->lpVtbl->Release(backbuffer);
    if (FAILED(hr)) {
        dbg("preview: CreateRenderTargetView failed after resize");
        preview_gpu_destroy();
        return;
    }

    g_prevGpu.swap_w = (int)w;
    g_prevGpu.swap_h = (int)h;
}

static int preview_gpu_init(HWND hwnd, const Grab *g, int is_hdr) {
    preview_gpu_destroy();
    g_prevGpu.is_hdr = is_hdr;

    IDXGIFactory1 *factory1 = NULL;
    IDXGIFactory2 *factory2 = NULL;
    IDXGIAdapter1 *adapter = NULL;
    ID3DBlob *vs_blob = NULL;
    ID3DBlob *ps_blob = NULL;
    IDXGISwapChain1 *swap1 = NULL;
    ID3D11Texture2D *backbuffer = NULL;
    int ok = 0;

    RECT rc;
    GetClientRect(hwnd, &rc);
    UINT w = (UINT)(rc.right - rc.left);
    UINT h = (UINT)(rc.bottom - rc.top);
    if (w < 1 || h < 1) return 0;

    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

    // Find the adapter that drives this monitor. CreateDXGIFactory2 is
    // Windows 8.1+, so QI for it and fall back to GDI on older systems.
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory1))) {
        dbg("preview: CreateDXGIFactory1 failed");
        goto done;
    }
    if (FAILED(factory1->lpVtbl->QueryInterface(
            factory1, &IID_IDXGIFactory2, (void **)&factory2))) {
        dbg("preview: IDXGIFactory2 unavailable");
        goto done;
    }

    for (UINT ai = 0; !adapter; ai++) {
        IDXGIAdapter1 *cand = NULL;
        if (factory1->lpVtbl->EnumAdapters1(factory1, ai, &cand) == DXGI_ERROR_NOT_FOUND)
            break;
        if (!cand) continue;

        for (UINT oi = 0; ; oi++) {
            IDXGIOutput *out = NULL;
            if (cand->lpVtbl->EnumOutputs(cand, oi, &out) == DXGI_ERROR_NOT_FOUND)
                break;
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
        dbg("preview: no adapter for monitor");
        goto done;
    }

    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL got;
    if (FAILED(D3D11CreateDevice((IDXGIAdapter *)adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3,
                                 D3D11_SDK_VERSION, &g_prevGpu.device, &got,
                                 &g_prevGpu.ctx))) {
        dbg("preview: D3D11CreateDevice failed");
        goto done;
    }

    vs_blob = preview_gpu_compile("VS", "vs_4_0");
    ps_blob = preview_gpu_compile("PS", "ps_4_0");
    if (!vs_blob || !ps_blob) goto done;
    if (FAILED(g_prevGpu.device->lpVtbl->CreateVertexShader(
            g_prevGpu.device, vs_blob->lpVtbl->GetBufferPointer(vs_blob),
            vs_blob->lpVtbl->GetBufferSize(vs_blob), NULL, &g_prevGpu.vs))) {
        dbg("preview: CreateVertexShader failed");
        goto done;
    }
    if (FAILED(g_prevGpu.device->lpVtbl->CreatePixelShader(
            g_prevGpu.device, ps_blob->lpVtbl->GetBufferPointer(ps_blob),
            ps_blob->lpVtbl->GetBufferSize(ps_blob), NULL, &g_prevGpu.ps))) {
        dbg("preview: CreatePixelShader failed");
        goto done;
    }
    vs_blob->lpVtbl->Release(vs_blob); vs_blob = NULL;
    ps_blob->lpVtbl->Release(ps_blob); ps_blob = NULL;

    DXGI_SWAP_CHAIN_DESC1 sd;
    ZeroMemory(&sd, sizeof sd);
    sd.Width = w;
    sd.Height = h;
    sd.Format = is_hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    if (FAILED(factory2->lpVtbl->CreateSwapChainForHwnd(
            factory2, (IUnknown *)g_prevGpu.device, hwnd, &sd, NULL, NULL, &swap1))) {
        dbg("preview: CreateSwapChainForHwnd failed");
        goto done;
    }
    if (FAILED(swap1->lpVtbl->QueryInterface(
            swap1, &IID_IDXGISwapChain3, (void **)&g_prevGpu.swap))) {
        dbg("preview: IDXGISwapChain3 unavailable");
        goto done;
    }
    factory2->lpVtbl->MakeWindowAssociation(factory2, hwnd, DXGI_MWA_NO_ALT_ENTER);

    if (is_hdr) {
        // The captured frame is linear scRGB (1.0 = SDR reference white). Pass
        // it through unchanged so values above 1.0 stay HDR highlights instead
        // of being clipped to white by an 8-bit sRGB DIB.
        if (FAILED(g_prevGpu.swap->lpVtbl->SetColorSpace1(
                g_prevGpu.swap, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709))) {
            dbg("preview: SetColorSpace1(scRGB) failed");
            goto done;
        }
    } else {
        // sRGB is the default for a B8G8R8A8 flip chain; request it explicitly
        // but do not abort if the driver refuses an otherwise working chain.
        g_prevGpu.swap->lpVtbl->SetColorSpace1(
            g_prevGpu.swap, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    }

    if (FAILED(g_prevGpu.swap->lpVtbl->GetBuffer(
            g_prevGpu.swap, 0, &IID_ID3D11Texture2D, (void **)&backbuffer))) {
        dbg("preview: GetBuffer failed");
        goto done;
    }
    if (FAILED(g_prevGpu.device->lpVtbl->CreateRenderTargetView(
            g_prevGpu.device, (ID3D11Resource *)backbuffer, NULL, &g_prevGpu.rtv))) {
        dbg("preview: CreateRenderTargetView failed");
        goto done;
    }

    D3D11_BUFFER_DESC bd;
    ZeroMemory(&bd, sizeof bd);
    bd.ByteWidth = sizeof(PreviewGpuCB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_prevGpu.device->lpVtbl->CreateBuffer(
            g_prevGpu.device, &bd, NULL, &g_prevGpu.cb))) {
        dbg("preview: CreateBuffer(cb) failed");
        goto done;
    }

    D3D11_SAMPLER_DESC smd;
    ZeroMemory(&smd, sizeof smd);
    smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    smd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(g_prevGpu.device->lpVtbl->CreateSamplerState(
            g_prevGpu.device, &smd, &g_prevGpu.samp))) {
        dbg("preview: CreateSamplerState failed");
        goto done;
    }

    if (!preview_gpu_upload_source(g)) goto done;

    // The close button texture is filled in before it is first sampled. Its
    // size is scaled with the monitor DPI so the glyph stays crisp.
    {
        int xw = ui_scale(ui_dpi_for_window(hwnd), PREVIEW_X_W);
        int xh = ui_scale(ui_dpi_for_window(hwnd), PREVIEW_X_H);
        if (xw < 1) xw = 1;
        if (xh < 1) xh = 1;

        D3D11_TEXTURE2D_DESC xtd;
        ZeroMemory(&xtd, sizeof xtd);
        xtd.Width = (UINT)xw;
        xtd.Height = (UINT)xh;
        xtd.MipLevels = 1;
        xtd.ArraySize = 1;
        xtd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        xtd.SampleDesc.Count = 1;
        xtd.Usage = D3D11_USAGE_DEFAULT;
        xtd.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        // Safe default: an opaque dark button, so the texture is never
        // undefined even if the first GDI render fails.
        uint32_t *xinit = (uint32_t *)malloc((size_t)xw * xh * sizeof(uint32_t));
        if (!xinit) {
            dbg("preview: close button init allocation failed");
            goto done;
        }
        for (int i = 0; i < xw * xh; i++) xinit[i] = 0xFF202020u;

        D3D11_SUBRESOURCE_DATA xdata;
        ZeroMemory(&xdata, sizeof xdata);
        xdata.pSysMem = xinit;
        xdata.SysMemPitch = (UINT)xw * 4;

        HRESULT xhr = g_prevGpu.device->lpVtbl->CreateTexture2D(
                g_prevGpu.device, &xtd, &xdata, &g_prevGpu.x_tex);
        free(xinit);

        if (FAILED(xhr)) {
            dbg("preview: CreateTexture2D(close button) failed");
            goto done;
        }

        g_prevGpu.x_tex_w = xw;
        g_prevGpu.x_tex_h = xh;

        D3D11_SHADER_RESOURCE_VIEW_DESC xsvd;
        ZeroMemory(&xsvd, sizeof xsvd);
        xsvd.Format = xtd.Format;
        xsvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        xsvd.Texture2D.MipLevels = 1;
        if (FAILED(g_prevGpu.device->lpVtbl->CreateShaderResourceView(
                g_prevGpu.device, (ID3D11Resource *)g_prevGpu.x_tex, &xsvd,
                &g_prevGpu.x_srv))) {
            dbg("preview: CreateShaderResourceView(close button) failed");
            goto done;
        }
    }

    g_prevGpu.swap_w = (int)w;
    g_prevGpu.swap_h = (int)h;
    g_prevGpuActive = TRUE;
    ok = 1;

done:
    if (backbuffer) backbuffer->lpVtbl->Release(backbuffer);
    if (swap1) swap1->lpVtbl->Release(swap1);
    if (vs_blob) vs_blob->lpVtbl->Release(vs_blob);
    if (ps_blob) ps_blob->lpVtbl->Release(ps_blob);
    if (factory2) factory2->lpVtbl->Release(factory2);
    if (factory1) factory1->lpVtbl->Release(factory1);
    if (adapter) adapter->lpVtbl->Release(adapter);

    if (!ok) preview_gpu_destroy();
    return ok;
}

// Convert the captured Grab into a 32-bpp BGRA DIB. This is the clipboard
// source and the GDI fallback; the on-screen preview goes through
// preview_gpu_present() when D3D is available. SDR is a direct channel swizzle;
// HDR takes the FP16 scRGB half floats through the sRGB LUT. Very large frames
// are point-sampled down to PREVIEW_MAX_TEX so the bitmap stays bounded even on
// 6K/8K displays.
static BOOL preview_make_bitmap(const Grab *g) {
    if (!g || !g->bits || g->w <= 0 || g->h <= 0) return FALSE;

    int sw = g->w, sh = g->h;
    int dw = sw, dh = sh;
    if (sw > PREVIEW_MAX_TEX || sh > PREVIEW_MAX_TEX) {
        int big = (sw > sh) ? sw : sh;
        double sc = (double)PREVIEW_MAX_TEX / (double)big;
        dw = (int)(sw * sc);
        dh = (int)(sh * sc);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = dw;
    bi.bmiHeader.biHeight = -dh; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        return FALSE;
    }

    size_t bpp = g->is_hdr ? 6 : 3;
    const uint8_t *lut = g->is_hdr ? preview_srgb_lut() : NULL;
    const uint8_t *src = g->bits;
    uint8_t *dst = (uint8_t *)bits;

    for (int y = 0; y < dh; y++) {
        int sy = (dw == sw && dh == sh) ? y : (int)((long long)y * sh / dh);
        if (sy >= sh) sy = sh - 1;

        const uint8_t *srow = src + (size_t)sy * sw * bpp;
        uint8_t *drow = dst + (size_t)y * dw * 4;

        for (int x = 0; x < dw; x++) {
            int sx = (dw == sw && dh == sh) ? x : (int)((long long)x * sw / dw);
            if (sx >= sw) sx = sw - 1;

            const uint8_t *sp = srow + (size_t)sx * bpp;
            uint8_t *dp = drow + (size_t)x * 4;

            if (g->is_hdr) {
                float r = half_to_float((uint16_t)(sp[0] | (sp[1] << 8)));
                float gg = half_to_float((uint16_t)(sp[2] | (sp[3] << 8)));
                float b = half_to_float((uint16_t)(sp[4] | (sp[5] << 8)));

                int ir = !(r > 0.0f) ? 0 : (r >= 1.0f ? 1024 : (int)(r * 1024.0f));
                int ig = !(gg > 0.0f) ? 0 : (gg >= 1.0f ? 1024 : (int)(gg * 1024.0f));
                int ib = !(b > 0.0f) ? 0 : (b >= 1.0f ? 1024 : (int)(b * 1024.0f));

                dp[0] = lut[ib]; // B
                dp[1] = lut[ig]; // G
                dp[2] = lut[ir]; // R
                dp[3] = 255;
            } else {
                dp[0] = sp[2]; // B
                dp[1] = sp[1]; // G
                dp[2] = sp[0]; // R
                dp[3] = 255;
            }
        }
    }

    if (g_prevBmp) DeleteObject(g_prevBmp);
    g_prevBmp = bmp;
    g_prevBits = bits;
    g_prevW = dw;
    g_prevH = dh;
    g_prevBmi = bi;
    return TRUE;
}

static void preview_copy_to_clipboard(void) {
    if (!g_prevBits || g_prevW <= 0 || g_prevH <= 0) return;

    DWORD pixels = (DWORD)g_prevW * (DWORD)g_prevH * 4;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixels);
    if (!mem) return;

    BITMAPINFOHEADER *bi = (BITMAPINFOHEADER *)GlobalLock(mem);
    if (!bi) {
        GlobalFree(mem);
        return;
    }

    bi->biSize = sizeof(BITMAPINFOHEADER);
    bi->biWidth = g_prevW;
    bi->biHeight = g_prevH; // clipboard DIBs are bottom-up
    bi->biPlanes = 1;
    bi->biBitCount = 32;
    bi->biCompression = BI_RGB;
    bi->biSizeImage = pixels;

    uint8_t *dst = (uint8_t *)(bi + 1);
    const uint8_t *src = (const uint8_t *)g_prevBits;
    for (int y = 0; y < g_prevH; y++) {
        memcpy(dst + (size_t)y * g_prevW * 4,
               src + (size_t)(g_prevH - 1 - y) * g_prevW * 4,
               (size_t)g_prevW * 4);
    }
    GlobalUnlock(mem);

    if (OpenClipboard(g_hwndPreview)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_DIB, mem)) GlobalFree(mem);
        CloseClipboard();
    } else {
        GlobalFree(mem);
    }
}

static void preview_open_folder(void) {
    if (g_prevPath[0] && GetFileAttributesW(g_prevPath) != INVALID_FILE_ATTRIBUTES) {
        wchar_t args[MAX_PATH + 32];
        _snwprintf(args, MAX_PATH + 32, L"/select,\"%s\"", g_prevPath);
        args[MAX_PATH + 31] = L'\0';
        ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
    } else {
        // Encoding may still be running; the export folder is the next best
        // destination until the file exists.
        execute_open_export_folder();
    }
}

// Win10/11 "Open with" chooser. SHOpenWithDialog is the real API behind it
// (the "openas" verb and OpenAs_RunDLL are unreliable on current Windows), so
// it is called first. It is looked up at runtime so no extra header or import
// library is needed.
typedef struct {
    LPCWSTR pcszFile;
    LPCWSTR pcszClass;
    DWORD   oaifInFlags;
} JxlOpenAsInfo;

static void preview_open_with(void) {
    if (!g_prevPath[0]) return;
    if (GetFileAttributesW(g_prevPath) == INVALID_FILE_ATTRIBUTES) return;

    HWND owner = g_hwndPreview;
    wchar_t file[MAX_PATH];
    wcsncpy_s(file, MAX_PATH, g_prevPath, _TRUNCATE);   // preview may close while the dialog is up

    // The preview is TOPMOST; drop that so the chooser is not hidden behind it.
    if (owner) {
        SetWindowPos(owner, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetForegroundWindow(owner);
    }

    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    BOOL done = FALSE;
    typedef HRESULT (WINAPI *SHOpenWithDialog_t)(HWND, const JxlOpenAsInfo *);
    HMODULE sh = LoadLibraryW(L"shell32.dll");
    SHOpenWithDialog_t fn = sh ? (SHOpenWithDialog_t)GetProcAddress(sh, "SHOpenWithDialog") : NULL;
    if (fn) {
        JxlOpenAsInfo oi;
        oi.pcszFile = file;
        oi.pcszClass = NULL;
        oi.oaifInFlags = 0x1 /*OAIF_ALLOW_REGISTRATION*/ | 0x4 /*OAIF_EXEC*/;
        HRESULT hr = fn(owner, &oi);
        // Success, or the user cancelled: either way nothing more to do.
        if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) done = TRUE;
    }
    if (sh) FreeLibrary(sh);
    if (SUCCEEDED(com)) CoUninitialize();
    if (done) return;

    // Fallbacks for odd systems.
    wchar_t args[MAX_PATH + 64];
    _snwprintf(args, MAX_PATH + 64, L"shell32.dll,OpenAs_RunDLL %s", file);
    args[MAX_PATH + 63] = L'\0';
    HINSTANCE r = ShellExecuteW(NULL, L"open", L"rundll32.exe", args, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32)
        ShellExecuteW(NULL, L"openas", file, NULL, NULL, SW_SHOWNORMAL);
}

// EncodeWorker side: remember the finished file and poke the preview. No GDI
// or D3D calls here; those objects belong to the window thread.
static void preview_on_saved(const wchar_t *path) {
    if (!path || !path[0]) return;

    // Ignore saves that do not belong to the capture this preview is showing
    // (multimonitor mode saves several files; only the first one is previewed).
    wchar_t current[MAX_PATH];
    preview_get_path(current);
    if (_wcsicmp(current, path) != 0) return;

    AcquireSRWLockExclusive(&g_prevReadyLock);
    wcsncpy_s(g_prevReadyPath, MAX_PATH, path, _TRUNCATE);
    ReleaseSRWLockExclusive(&g_prevReadyLock);

    HWND hwnd = g_hwndPreview;
    if (hwnd && IsWindow(hwnd)) {
        PostMessageW(hwnd, WM_PREVIEW_FILE_READY, 0, 0);
    }
}

// Swap the preview over to the decoded .jxl once the encoder has closed the
// file. HDR stays FP16 linear scRGB (libjxl converts the PQ / Rec.2020 data),
// SDR stays 8-bit sRGB, so the existing passthrough shader works unchanged.
static void preview_reload_from_file(HWND hwnd, const wchar_t *path) {
    if (!g_prevGpuActive || !hwnd || !path || !path[0]) return;

    int is_hdr = g_prevGpu.is_hdr;
    uint8_t *bits = NULL;
    int w = 0, h = 0;

    if (!decode_jxl_for_preview(path, is_hdr, &bits, &w, &h)) return;
    if (w <= 0 || h <= 0 || !bits) { free(bits); return; }

    if (preview_gpu_upload(bits, w, h, g_prevW, g_prevH, is_hdr)) {
        g_prevShowingDecoded = TRUE;
        dbg("preview: showing decoded %s frame %dx%d",
            is_hdr ? "HDR" : "SDR", w, h);
        InvalidateRect(hwnd, NULL, FALSE);
    }
    free(bits);
}

// Right-click menu, using the same hidden owner window as the tray menu so it
// picks up the Dark/Light theme.
static void preview_show_menu(HWND hwnd, POINT pt) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    BOOL exists = g_prevPath[0] &&
                  GetFileAttributesW(g_prevPath) != INVALID_FILE_ATTRIBUTES;

    AppendMenuW(menu, MF_STRING, IDC_PREV_FOLDER, L"Open File in Folder");
    AppendMenuW(menu, MF_STRING, IDC_PREV_COPY,   L"Copy Image");
    AppendMenuW(menu, MF_STRING | (exists ? 0 : MF_GRAYED),
                IDC_PREV_OPENWITH, L"Open With...");
    AppendMenuW(menu, MF_STRING | (exists ? 0 : MF_GRAYED) |
                      (g_prevShowingDecoded ? MF_CHECKED : 0),
                IDC_PREV_RELOAD, L"View Encoded JXL");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDC_PREV_CLOSE, L"Close Preview");

    HWND owner = g_hwndMenuOwner ? g_hwndMenuOwner : hwnd;
    HWND prevFg = GetForegroundWindow();
    if (g_hwndMenuOwner) {
        ApplyDarkMode(g_hwndMenuOwner);
        SetForegroundWindow(g_hwndMenuOwner);
    }

    UINT cmd = (UINT)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                                    pt.x, pt.y, 0, owner, NULL);
    DestroyMenu(menu);

    if (prevFg && prevFg != owner && IsWindow(prevFg)) SetForegroundWindow(prevFg);

    if (cmd) PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
}

// Only keeps the window on its monitor while dragging; never changes the size.
static void preview_clamp_to_monitor(RECT *r) {
    HMONITOR mon = MonitorFromRect(r, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;

    RECT m = mi.rcMonitor;
    int w = r->right - r->left;
    int h = r->bottom - r->top;
    int x = r->left, y = r->top;

    if (w <= m.right - m.left) {
        if (x + w > m.right) x = m.right - w;
        if (x < m.left) x = m.left;
    }
    if (h <= m.bottom - m.top) {
        if (y + h > m.bottom) y = m.bottom - h;
        if (y < m.top) y = m.top;
    }
    r->left = x; r->top = y; r->right = x + w; r->bottom = y + h;
}

static void preview_register_class(void) {
    static BOOL registered = FALSE;
    if (registered) return;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = 0;                       // no CS_DROPSHADOW
    wc.lpfnWndProc = PreviewWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"JxlShotPreviewClass";

    if (RegisterClassExW(&wc)) registered = TRUE;
}

// Remove every DWM decoration: shadow, border line, Win11 rounded corners.
static void preview_flatten_frame(HWND hwnd) {
    typedef HRESULT (WINAPI *DwmSetWindowAttribute_t)(HWND, DWORD, LPCVOID, DWORD);

    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;

    DwmSetWindowAttribute_t fn =
        (DwmSetWindowAttribute_t)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (fn) {
        int policy = 1;                 // DWMNCRP_DISABLED -> no NC rendering / shadow
        fn(hwnd, 2 /*DWMWA_NCRENDERING_POLICY*/, &policy, sizeof(policy));

        int corner = 1;                 // DWMWCP_DONOTROUND
        fn(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));

        DWORD none = 0xFFFFFFFE;        // DWMWA_COLOR_NONE
        fn(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &none, sizeof(none));
    }
    FreeLibrary(dwm);
}

// Close button visibility helpers. The button follows mouse movement and is
// hidden after a short idle period or when the cursor leaves the window.
static void preview_reset_idle(HWND hwnd) {
    KillTimer(hwnd, PREVIEW_TIMER_X);
    SetTimer(hwnd, PREVIEW_TIMER_X, PREVIEW_IDLE_MS, NULL);
}

static void preview_show_x(HWND hwnd) {
    if (!g_prevXShown) {
        g_prevXShown = TRUE;
        RECT xr = preview_x_rect(hwnd);
        InvalidateRect(hwnd, &xr, FALSE);
    }
    preview_reset_idle(hwnd);
}

static void preview_hide_x(HWND hwnd) {
    if (!g_prevXShown && !g_prevXHot) return;
    g_prevXShown = FALSE;
    g_prevXHot = FALSE;
    KillTimer(hwnd, PREVIEW_TIMER_X);
    RECT xr = preview_x_rect(hwnd);
    InvalidateRect(hwnd, &xr, FALSE);
}

// Mouse left the client (X) or the whole non-client image. Staying inside just
// clears the hover state; the idle timer then hides the X as usual.
static void preview_on_mouse_leave(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    RECT wr;
    GetWindowRect(hwnd, &wr);

    if (PtInRect(&wr, pt)) {
        if (g_prevXHot) {
            g_prevXHot = FALSE;
            RECT xr = preview_x_rect(hwnd);
            InvalidateRect(hwnd, &xr, FALSE);
        }
        return;
    }
    preview_hide_x(hwnd);
}

static void preview_paint(HWND hwnd, HDC dc) {
    RECT rc;
    GetClientRect(hwnd, &rc);

    if (g_prevBorder > 0)
        FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));   // frame

    RECT img = preview_image_rect(hwnd);
    int iw = img.right - img.left;
    int ih = img.bottom - img.top;

    if (g_prevBmp && g_prevBits && g_prevW > 0 && g_prevH > 0) {
        if (g_prevW == iw && g_prevH == ih) {
            SetDIBitsToDevice(dc, img.left, img.top, g_prevW, g_prevH, 0, 0, 0, g_prevH,
                              g_prevBits, &g_prevBmi, DIB_RGB_COLORS);   // 1:1, no resampling
        } else {
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, NULL);
            StretchDIBits(dc, img.left, img.top, iw, ih,
                          0, 0, g_prevW, g_prevH,
                          g_prevBits, &g_prevBmi, DIB_RGB_COLORS, SRCCOPY);
        }
    } else if (g_prevBorder == 0) {
        HBRUSH bg = CreateSolidBrush(RGB(18, 18, 18));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
    }

    if (g_prevXShown) {
        RECT xr = preview_x_rect(hwnd);
        int glyph = ui_scale(ui_dpi_for_window(hwnd), GUI_CLOSE_GLYPH);
        gui_draw_close_button(dc, &xr, g_prevXHot, TRUE,
                              RGB(32, 32, 32), RGB(255, 255, 255), glyph);
    }
}

// Public entry point used by the capture paths. The preview opens centred on
// the monitor under the cursor, at exactly the capture's size. A capture that
// fills the monitor (full screen) would cover everything and look like nothing
// happened, so it is shown smaller, still centred, inside a white frame.
// Safe to call repeatedly: an open preview gets the new frame and is resized.
static void gui_preview_show(const Grab *g, const wchar_t *path) {
    if (!g_cfg.show_preview) return;
    if (!g || !g->bits || g->w <= 0 || g->h <= 0) return;
    if (!preview_make_bitmap(g)) return;

    if (path && path[0]) preview_set_path(path);
    else preview_set_path(NULL);

    if (!g_guiBgBrush) gui_update_theme();
    preview_register_class();

    POINT cur;
    GetCursorPos(&cur);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST), &mi);
    RECT m = mi.rcMonitor;
    int availW = m.right - m.left;
    int availH = m.bottom - m.top;

    int w = g->w, h = g->h;
    int border = 0;
    int preview_dpi = ui_dpi_for_monitor(MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST));

    if (availW > 0 && availH > 0 &&
        (g->w >= availW * 95 / 100 || g->h >= availH * 95 / 100)) {
        border = ui_scale(preview_dpi, PREVIEW_FULL_BORDER);
        double s1 = (double)availW * 0.85 / g->w;
        double s2 = (double)availH * 0.85 / g->h;
        double sc = (s1 < s2) ? s1 : s2;
        if (sc > 1.0) sc = 1.0;
        w = (int)(g->w * sc) + 2 * border;
        h = (int)(g->h * sc) + 2 * border;
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    int x = m.left + (availW - w) / 2;
    int y = m.top + (availH - h) / 2;

    g_prevBorder = border;
    g_prevXShown = FALSE;
    g_prevXHot = FALSE;
    g_prevShowingDecoded = FALSE;

    if (!g_hwndPreview || !IsWindow(g_hwndPreview)) {
        g_hwndPreview = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            L"JxlShotPreviewClass", L"",
            WS_POPUP,                       // no frame, no thick border
            x, y, w, h,
            NULL, NULL, GetModuleHandleW(NULL), NULL);
        if (!g_hwndPreview) return;
        preview_flatten_frame(g_hwndPreview);
    } else {
        SetWindowPos(g_hwndPreview, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
        InvalidateRect(g_hwndPreview, NULL, FALSE);
    }

    // Present the frame in its own colour space: FP16 scRGB for HDR captures,
    // B8G8R8A8 sRGB for SDR. The GDI bitmap is only the fallback (and the
    // clipboard source) if D3D setup fails.
    if (!preview_gpu_init(g_hwndPreview, g, g->is_hdr)) {
        dbg("preview: GPU presentation unavailable, using GDI fallback");
    }

    ShowWindow(g_hwndPreview, SW_SHOW);
    AllowSetForegroundWindow(GetCurrentProcessId());
    SetForegroundWindow(g_hwndPreview);

    // Flash the X once so it is discoverable, then let the idle timer hide it.
    preview_show_x(g_hwndPreview);
}

static BOOL gui_preview_is_visible(void) {
    return (g_hwndPreview && IsWindowVisible(g_hwndPreview)) ? TRUE : FALSE;
}

// Used by the tray menu item and by the Settings toggle when previews are
// switched off. The normal WM_DESTROY path restores the strip if needed.
static void gui_preview_close(void) {
    if (g_hwndPreview && IsWindow(g_hwndPreview)) {
        DestroyWindow(g_hwndPreview);
    }
}

// A new capture must not include the previous preview. Close it first; the
// close handler is told not to bring the strip back, because every caller is
// either already hiding the strip or about to grab the screen. If the strip
// was waiting for this preview, that wait is handed to the next preview.
static BOOL preview_close_for_capture(void) {
    if (g_hwndPreview && IsWindow(g_hwndPreview)) {
        BOOL wasPending = g_guiRestorePending;
        g_guiRestorePending = FALSE;
        g_prevClosingForCapture = TRUE;
        DestroyWindow(g_hwndPreview);
        g_prevClosingForCapture = FALSE;
        g_guiRestorePending = wasPending;
        return TRUE;
    }
    return FALSE;
}

// The image is theme independent, so a Dark/Light flip only has to repaint the
// overlay button.
static void preview_refresh_theme(void) {
    if (!g_hwndPreview) return;
    InvalidateRect(g_hwndPreview, NULL, TRUE);
}

static LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOVING:
            preview_clamp_to_monitor((RECT *)lp);
            return TRUE;

        case WM_PREVIEW_FILE_READY: {
            // The encoder finished writing the file. Only refresh if it is the
            // capture this preview is showing (a stale message from an earlier
            // capture is ignored).
            wchar_t path[MAX_PATH];
            AcquireSRWLockShared(&g_prevReadyLock);
            wcsncpy_s(path, MAX_PATH, g_prevReadyPath, _TRUNCATE);
            ReleaseSRWLockShared(&g_prevReadyLock);

            if (path[0] && _wcsicmp(path, g_prevPath) == 0) {
                preview_reload_from_file(hwnd, path);
            }
            return 0;
        }

        case WM_NCLBUTTONDBLCLK:
            return 0; // no maximize on double click

        case WM_SIZE:
            preview_gpu_resize(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            // HDR/SDR-aware GPU presentation; the GDI painter is only the
            // fallback when D3D setup is unavailable.
            if (g_prevGpuActive) preview_gpu_present(hwnd);
            else                 preview_paint(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_NCHITTEST: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);

            RECT xr = preview_x_rect(hwnd);
            if (g_prevXShown && PtInRect(&xr, pt)) return HTCLIENT;

            return HTCAPTION;   // the whole image is the drag surface; no resize borders
        }

        case WM_NCMOUSEMOVE: {
            preview_show_x(hwnd);

            // The X may have just appeared under a stationary cursor. Track
            // that here as well, because the hit test only switches this spot
            // to HTCLIENT on the next mouse message.
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);
            RECT xr = preview_x_rect(hwnd);
            BOOL hot = PtInRect(&xr, pt);
            if (hot != g_prevXHot) {
                g_prevXHot = hot;
                InvalidateRect(hwnd, &xr, FALSE);
            }

            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE | TME_NONCLIENT, hwnd, 0 };
            TrackMouseEvent(&t);
            return 0;
        }

        case WM_NCMOUSELEAVE:
            preview_on_mouse_leave(hwnd);
            return 0;

        case WM_MOUSEMOVE: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT xr = preview_x_rect(hwnd);
            BOOL hot = PtInRect(&xr, pt);

            if (hot != g_prevXHot) {
                g_prevXHot = hot;
                InvalidateRect(hwnd, &xr, FALSE);
            }

            preview_show_x(hwnd);
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&t);
            return 0;
        }

        case WM_MOUSELEAVE:
            preview_on_mouse_leave(hwnd);
            return 0;

        case WM_TIMER:
            if (wp == PREVIEW_TIMER_X) {
                // The X only disappears when the pointer has genuinely left
                // it; parking the cursor on the button keeps it usable.
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                RECT xr = preview_x_rect(hwnd);

                if (PtInRect(&xr, pt)) {
                    if (!g_prevXHot) {
                        g_prevXHot = TRUE;
                        InvalidateRect(hwnd, &xr, FALSE);
                    }
                    return 0;
                }

                preview_hide_x(hwnd);
                return 0;
            }
            break;

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                RECT xr = preview_x_rect(hwnd);
                if (g_prevXShown && PtInRect(&xr, pt)) {
                    SetCursor(LoadCursorW(NULL, IDC_HAND));
                    return TRUE;
                }
            }
            break;

        case WM_RBUTTONUP: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(hwnd, &pt);
            preview_show_menu(hwnd, pt);
            return 0;
        }

        case WM_NCRBUTTONUP: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            preview_show_menu(hwnd, pt);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT xr = preview_x_rect(hwnd);
            if (g_prevXShown && PtInRect(&xr, pt)) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_PREV_COPY:     preview_copy_to_clipboard(); break;
                case IDC_PREV_FOLDER:   preview_open_folder(); break;
                case IDC_PREV_OPENWITH: preview_open_with(); break;
                case IDC_PREV_RELOAD:   preview_reload_from_file(hwnd, g_prevPath); break;
                case IDC_PREV_CLOSE:    DestroyWindow(hwnd); break;
            }
            return 0;

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            if (wp == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                preview_copy_to_clipboard();
                return 0;
            }
            break;

        case WM_DESTROY: {
            KillTimer(hwnd, PREVIEW_TIMER_X);
            preview_gpu_destroy();
            g_hwndPreview = NULL;
            g_prevXShown = FALSE;
            g_prevXHot = FALSE;
            preview_set_path(NULL);
            g_prevBorder = 0;
            g_prevShowingDecoded = FALSE;

            if (g_prevBmp) {
                DeleteObject(g_prevBmp);
                g_prevBmp = NULL;
            }
            g_prevBits = NULL;
            g_prevW = g_prevH = 0;

            // If the strip hid itself for this capture, bring it back now that
            // the preview is gone.
            if (g_guiRestorePending) {
                g_guiRestorePending = FALSE;
                if (g_hwndGui && IsWindow(g_hwndGui)) {
                    ShowWindow(g_hwndGui, SW_SHOW);
                    SetForegroundWindow(g_hwndGui);
                    SetFocus(g_hwndGui);
                }
            }

            // The Settings window also hid for the capture. Skip this when the
            // preview is being destroyed only because a new capture started:
            // the caller hides it again for the new grab.
            if (!g_prevClosingForCapture)
                settings_restore_after_capture();
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Called by the settings/hotkey code whenever g_cfg changes. This window has
// no child text controls, so a repaint is all it takes for the status line and
// the capture-card hotkey hints to show the new values.
static void gui_refresh(void) {
    if (g_hwndGui) InvalidateRect(g_hwndGui, NULL, FALSE);
}

// Hide the window first: a screenshot must not contain the window that asked
// for it (or the Settings window it opened). A short timer gives the
// compositor time to repaint the desktop.
static void gui_start_capture(int kind) {
    if (!g_hwndGui || g_guiPendingCapture) return;

    // A previous preview must not be part of the new capture.
    preview_close_for_capture();
    settings_hide_for_capture();

    g_guiPendingCapture = kind;
    g_guiRestorePending = TRUE; // the strip returns after the capture/preview
    ShowWindow(g_hwndGui, SW_HIDE);
    SetTimer(g_hwndGui, TIMER_GUI_CAPTURE, GUI_CAPTURE_DELAY_MS, NULL);
}


// Window procedure


static void gui_create_fonts(int dpi) {
    if (g_guiTitleFont)    { DeleteObject(g_guiTitleFont); g_guiTitleFont = NULL; }
    if (g_guiCardFont)     { DeleteObject(g_guiCardFont); g_guiCardFont = NULL; }
    if (g_guiSmallFont)    { DeleteObject(g_guiSmallFont); g_guiSmallFont = NULL; }
    if (g_guiXFont)        { DeleteObject(g_guiXFont); g_guiXFont = NULL; }
    if (g_guiTitleFontSm)  { DeleteObject(g_guiTitleFontSm); g_guiTitleFontSm = NULL; }
    if (g_guiCardFontSm)   { DeleteObject(g_guiCardFontSm); g_guiCardFontSm = NULL; }
    if (g_guiSmallFontSm)  { DeleteObject(g_guiSmallFontSm); g_guiSmallFontSm = NULL; }
    if (g_guiXFontSm)      { DeleteObject(g_guiXFontSm); g_guiXFontSm = NULL; }

    g_guiTitleFont = CreateFontW(-ui_scale(dpi, 22), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI Variable Display");
    g_guiCardFont = CreateFontW(-ui_scale(dpi, 14), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_guiSmallFont = CreateFontW(-ui_scale(dpi, 11), 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_guiXFont = CreateFontW(-ui_scale(dpi, 18), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    g_guiTitleFontSm = CreateFontW(-ui_scale(dpi, 14), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI Variable Display");
    g_guiCardFontSm = CreateFontW(-ui_scale(dpi, 12), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_guiSmallFontSm = CreateFontW(-ui_scale(dpi, 9), 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_guiXFontSm = CreateFontW(-ui_scale(dpi, 13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
}

static LRESULT CALLBACK GuiWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            gui_update_theme();
            ApplyDarkMode(hwnd);

            gui_create_fonts(ui_dpi_for_window(hwnd));

            // Buttons are created in left-to-right order: Full Screen, Region,
            // Settings, Open Folder. Export path and quality live in the
            // Settings window, so the strip stays at four buttons.
            HINSTANCE hi = GetModuleHandleW(NULL);
            for (int i = 0; i < GUI_BTN_COUNT; i++) {
                HWND hb = CreateWindowExW(0, L"BUTTON", kGuiButtons[i].longText,
                    WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                    0, 0, 10, 10,
                    hwnd, (HMENU)(INT_PTR)kGuiButtons[i].id, hi, NULL);
                if (hb) SetWindowSubclass(hb, GuiBtnSubclass, (UINT_PTR)kGuiButtons[i].id, 0);
            }

            gui_layout_children(hwnd);
            SetFocus(hwnd); // never leave a button focused on open (Space would re-capture)
            return 0;
        }

        case WM_DPICHANGED: {
            RECT *pr = (RECT *)lp;
            if (pr) {
                SetWindowPos(hwnd, NULL, pr->left, pr->top,
                            pr->right - pr->left, pr->bottom - pr->top,
                            SWP_NOZORDER | SWP_NOACTIVATE);
            }
            gui_create_fonts(ui_dpi_for_window(hwnd));   // was LOWORD(wp)
            gui_layout_children(hwnd);
            if (g_hwnd_catcher) position_settings_window(g_hwnd_catcher);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO *mmi = (MINMAXINFO *)lp;
            int dpi = ui_dpi_for_window(hwnd);
            mmi->ptMinTrackSize.x = ui_scale(dpi, GUI_MIN_W);
            mmi->ptMinTrackSize.y = ui_scale(dpi, GUI_MIN_H);
            mmi->ptMaxTrackSize.x = ui_scale(dpi, GUI_MAX_W);
            mmi->ptMaxTrackSize.y = ui_scale(dpi, GUI_MAX_H);
            return 0;
        }

        // Catch every programmatic move/size as well (snap, maximize, DPI
        // change), not just mouse dragging.
        case WM_WINDOWPOSCHANGING: {
            WINDOWPOS *p = (WINDOWPOS *)lp;
            RECT cur;
            GetWindowRect(hwnd, &cur);

            int w = (p->flags & SWP_NOSIZE) ? (cur.right - cur.left) : p->cx;
            int h = (p->flags & SWP_NOSIZE) ? (cur.bottom - cur.top) : p->cy;
            int x = (p->flags & SWP_NOMOVE) ? cur.left : p->x;
            int y = (p->flags & SWP_NOMOVE) ? cur.top  : p->y;

            RECT r = { x, y, x + w, y + h };
            gui_clamp_to_monitor(&r);

            p->x  = r.left;
            p->y  = r.top;
            p->cx = r.right - r.left;
            p->cy = r.bottom - r.top;
            p->flags &= ~(SWP_NOSIZE | SWP_NOMOVE);
            return 0;
        }

        case WM_MOVING:
            gui_clamp_to_monitor((RECT *)lp);
            return TRUE;

        case WM_SIZING:
            gui_clamp_to_monitor((RECT *)lp);
            return TRUE;

        case WM_SIZE:
            gui_layout_children(hwnd);
            if (g_hwnd_catcher) position_settings_window(g_hwnd_catcher);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_MOVE:
            // The Settings window is attached to the strip: it follows while the
            // strip is dragged or resized, and is destroyed with it below.
            if (g_hwnd_catcher) position_settings_window(g_hwnd_catcher);
            return 0;

        case WM_ERASEBKGND:
            return 1; // WM_PAINT fills the whole client area

        case WM_SHOWWINDOW:
            if (wp) InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);

            GuiLayout L;
            gui_get_layout(hwnd, &L);

            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, g_guiBgBrush);

            SetBkMode(dc, TRANSPARENT);

            RECT closeRc = gui_close_rect(&L);
            RECT themeRc = gui_theme_rect(&L, &closeRc);

            // Title bar.
            RECT tr = { L.margin, 0, themeRc.left - 8, L.titleH };
            HFONT oldFont = (HFONT)SelectObject(dc,
                L.compact ? g_guiTitleFontSm : g_guiTitleFont);
            SetTextColor(dc, GUI_TEXT);
            DrawTextW(dc, L"JXL Shot", -1, &tr,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT);

            // Theme selection bar: flat Dark | Light segments. The active one
            // is bright with an accent bar underneath, the other is dimmed;
            // the hovered segment gets a light fill.
            {
                int themeMid = (themeRc.left + themeRc.right) / 2;
                RECT dl = { themeRc.left, themeRc.top, themeMid, themeRc.bottom };
                RECT dr = { themeMid, themeRc.top, themeRc.right, themeRc.bottom };

                if (g_guiThemeHot) {
                    HBRUSH hb = CreateSolidBrush(GUI_BTN_HOT);
                    FillRect(dc, (g_guiThemeHot == 1) ? &dl : &dr, hb);
                    DeleteObject(hb);
                }

                RECT bar = g_cfg.dark_mode ? dl : dr;
                bar.top = bar.bottom - ui_scale(L.dpi, 3);
                bar.left += ui_scale(L.dpi, 8);
                bar.right -= ui_scale(L.dpi, 8);
                HBRUSH ab = CreateSolidBrush(GUI_ACCENT);
                FillRect(dc, &bar, ab);
                DeleteObject(ab);

                HFONT oldThemeFont = (HFONT)SelectObject(dc,
                    L.compact ? g_guiCardFontSm : g_guiCardFont);
                SetTextColor(dc, g_cfg.dark_mode ? GUI_TEXT : GUI_DIM);
                DrawTextW(dc, L"Dark", -1, &dl, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
                SetTextColor(dc, g_cfg.dark_mode ? GUI_DIM : GUI_TEXT);
                DrawTextW(dc, L"Light", -1, &dr, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
                SelectObject(dc, oldThemeFont);
            }

            // Close button: same flat red rectangle as the preview's.
            gui_draw_close_button(dc, &closeRc, g_guiCloseHot, FALSE,
                                  0, GUI_TEXT, ui_scale(L.dpi, GUI_CLOSE_GLYPH));

            SelectObject(dc, oldFont);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DRAWITEM:
            gui_draw_button((LPDRAWITEMSTRUCT)lp);
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_GUI_FULL:     gui_start_capture(1); break;
                case IDC_GUI_REGION:   gui_start_capture(2); break;
                case IDC_GUI_SETTINGS: execute_hotkey_settings(NULL); break;
                case IDC_GUI_EXPORT:   execute_open_export_folder(); break;
                case IDC_GUI_ABOUT:    execute_about(); break;
                case IDC_GUI_CLOSE:    DestroyWindow(hwnd); break;
            }
            return 0;

        case WM_TIMER:
            if (wp == TIMER_GUI_CAPTURE) {
                KillTimer(hwnd, TIMER_GUI_CAPTURE);

                int kind = g_guiPendingCapture;
                g_guiPendingCapture = 0;

                if (kind == 1) {
                    execute_full_capture();
                    if (gui_preview_is_visible()) {
                        // Keep the strip hidden: the preview is the window the
                        // user is looking at now. It restores the strip when
                        // it closes.
                        g_guiRestorePending = TRUE;
                    } else {
                        g_guiRestorePending = FALSE;
                        ShowWindow(hwnd, SW_SHOW);
                        SetForegroundWindow(hwnd);
                    }
                } else if (kind == 2) {
                    start_region_capture();
                    // The overlay is topmost and owns the screen; wait until it
                    // is gone before bringing this window back, so it cannot be
                    // raised above the frozen frame being selected.
                    SetTimer(hwnd, TIMER_GUI_REGION_DONE, GUI_REGION_POLL_MS, NULL);
                }
                return 0;
            }
            if (wp == TIMER_GUI_REGION_DONE) {
                if (g_hwndRegion) return 0; // selection still in progress
                KillTimer(hwnd, TIMER_GUI_REGION_DONE);
                if (gui_preview_is_visible()) {
                    // A crop was made and the preview opened; let it close
                    // first, then show the strip.
                    g_guiRestorePending = TRUE;
                } else {
                    g_guiRestorePending = FALSE;
                    ShowWindow(hwnd, SW_SHOW);
                    SetForegroundWindow(hwnd);
                    SetFocus(hwnd);
                }
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);

        case WM_NCHITTEST: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);

            GuiLayout L;
            gui_get_layout(hwnd, &L);

            // The close button sits in the corner, so test it (and the theme
            // toggle) before the resize borders.
            RECT closeRc = gui_close_rect(&L);
            RECT themeRc = gui_theme_rect(&L, &closeRc);
            if (PtInRect(&closeRc, pt) || PtInRect(&themeRc, pt)) return HTCLIENT;

            const int border = ui_scale(L.dpi, 6);
            BOOL left   = pt.x < border;
            BOOL right  = pt.x >= L.width - border;
            BOOL top    = pt.y < border;
            BOOL bottom = pt.y >= L.height - border;

            if (left)  return top ? HTTOPLEFT    : (bottom ? HTBOTTOMLEFT  : HTLEFT);
            if (right) return top ? HTTOPRIGHT   : (bottom ? HTBOTTOMRIGHT : HTRIGHT);
            if (bottom) return HTBOTTOM;
            if (top && pt.y >= 0) return HTTOP;
            if (pt.y >= 0 && pt.y < L.titleH) return HTCAPTION;
            return HTCLIENT;
        }

        case WM_MOUSEMOVE: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };

            GuiLayout L;
            gui_get_layout(hwnd, &L);
            RECT closeRc = gui_close_rect(&L);
            RECT themeRc = gui_theme_rect(&L, &closeRc);
            BOOL overClose = PtInRect(&closeRc, pt);
            BOOL overTheme = PtInRect(&themeRc, pt);

            if (overClose || overTheme) {
                SetCursor(LoadCursorW(NULL, IDC_HAND));
                TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&t);
            } else {
                SetCursor(LoadCursorW(NULL, IDC_ARROW));
            }

            if (g_guiCloseHot != overClose) {
                g_guiCloseHot = overClose;
                InvalidateRect(hwnd, &closeRc, FALSE);
            }
            int themeSeg = !overTheme ? 0
                         : (pt.x < (themeRc.left + themeRc.right) / 2 ? 1 : 2);
            if (g_guiThemeHot != themeSeg) {
                g_guiThemeHot = themeSeg;
                InvalidateRect(hwnd, &themeRc, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            SetCursor(LoadCursorW(NULL, IDC_ARROW));
            if (g_guiCloseHot || g_guiThemeHot) {
                GuiLayout L;
                gui_get_layout(hwnd, &L);
                RECT closeRc = gui_close_rect(&L);
                RECT themeRc = gui_theme_rect(&L, &closeRc);
                g_guiCloseHot = FALSE;
                g_guiThemeHot = 0;
                InvalidateRect(hwnd, &closeRc, FALSE);
                InvalidateRect(hwnd, &themeRc, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            GuiLayout L;
            gui_get_layout(hwnd, &L);
            RECT closeRc = gui_close_rect(&L);
            RECT themeRc = gui_theme_rect(&L, &closeRc);

            if (PtInRect(&closeRc, pt)) {
                DestroyWindow(hwnd);
                return 0;
            }
            if (PtInRect(&themeRc, pt)) {
                int mid = (themeRc.left + themeRc.right) / 2;
                BOOL wantDark = (pt.x < mid) ? TRUE : FALSE;
                if (g_cfg.dark_mode == wantDark) return 0;

                g_cfg.dark_mode = wantDark;
                gui_update_theme();
                ApplyDarkMode(hwnd);
                // The settings window is a separate top-level popup, so it
                // needs the new palette as well as the new DWM theme.
                if (g_hwnd_catcher) {
                    hk_update_theme();
                    ApplyDarkMode(g_hwnd_catcher);
                    InvalidateRect(g_hwnd_catcher, NULL, TRUE);
                }
                gui_save_theme();
                for (int i = 0; i < GUI_BTN_COUNT; i++) {
                    HWND hb = GetDlgItem(hwnd, kGuiButtons[i].id);
                    if (hb) InvalidateRect(hb, NULL, TRUE);
                }
                preview_refresh_theme();
                InvalidateRect(hwnd, NULL, TRUE);
                return 0;
            }
            return 0;
        }

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY: {
            // Save position and size before destroying
            RECT r;
            GetWindowRect(hwnd, &r);
            g_cfg.gui_x = r.left;
            g_cfg.gui_y = r.top;
            g_cfg.gui_w = r.right - r.left;
            g_cfg.gui_h = r.bottom - r.top;
            gui_save_window_state();

            // Never PostQuitMessage here: the tray application owns this
            // window and must keep running after it is closed.
            KillTimer(hwnd, TIMER_GUI_CAPTURE);
            KillTimer(hwnd, TIMER_GUI_REGION_DONE);
            g_guiPendingCapture = 0;
            g_guiRestorePending = FALSE;
            g_guiHoverBtn = NULL;
            g_guiCloseHot = FALSE;
            g_guiThemeHot = 0;
            g_guiShortLabelsSet = FALSE;

            if (g_guiBgBrush)   { DeleteObject(g_guiBgBrush);   g_guiBgBrush = NULL; }
            if (g_guiTitleFont) { DeleteObject(g_guiTitleFont); g_guiTitleFont = NULL; }
            if (g_guiCardFont)  { DeleteObject(g_guiCardFont);  g_guiCardFont = NULL; }
            if (g_guiSmallFont) { DeleteObject(g_guiSmallFont); g_guiSmallFont = NULL; }
            if (g_guiXFont)     { DeleteObject(g_guiXFont);     g_guiXFont = NULL; }
            if (g_guiTitleFontSm) { DeleteObject(g_guiTitleFontSm); g_guiTitleFontSm = NULL; }
            if (g_guiCardFontSm)  { DeleteObject(g_guiCardFontSm);  g_guiCardFontSm = NULL; }
            if (g_guiSmallFontSm) { DeleteObject(g_guiSmallFontSm); g_guiSmallFontSm = NULL; }
            if (g_guiXFontSm)     { DeleteObject(g_guiXFontSm);     g_guiXFontSm = NULL; }

            // Closing the strip closes the attached Settings window too.
            if (g_hwnd_catcher && IsWindow(g_hwnd_catcher)) {
                DestroyWindow(g_hwnd_catcher);
            }

            g_hwndGui = NULL;
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}


// Entry point (called from the tray menu / tray icon)



static HWND gui_hwnd(void) {
    return g_hwndGui;
}

// True when the strip is on screen. Captures started from the tray menu or a
// hotkey use this to route through gui_start_capture(), which hides the window
// for the shot and brings it back afterwards.
static BOOL gui_is_visible(void) {
    return (g_hwndGui && IsWindowVisible(g_hwndGui)) ? TRUE : FALSE;
}

static void execute_gui(void) {
    // Single instance: focus the existing window instead of creating another.
    if (g_hwndGui) {
        if (!IsWindowVisible(g_hwndGui)) ShowWindow(g_hwndGui, SW_SHOW);
        SetForegroundWindow(g_hwndGui);
        SetFocus(g_hwndGui);
        return;
    }

    AllowSetForegroundWindow(GetCurrentProcessId());

    static HICON icon = NULL;
    if (!icon) {
        icon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON),
                                 IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
        if (!icon) {
            icon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON),
                                     IMAGE_ICON, 48, 48, LR_DEFAULTCOLOR);
        }
        if (!icon) icon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON));
    }

    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = GuiWindowProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = L"JxlShotGuiClass";
        wc.hIcon = icon;
        wc.hIconSm = icon;
        RegisterClassExW(&wc);
        registered = TRUE;
    }

    // Pick the monitor from the last known position (or the cursor when there
    // is no saved geometry) and clamp the strip inside its physical bounds.
    POINT anchor;
    if (g_cfg.gui_w > 0 && g_cfg.gui_h > 0) {
        anchor.x = g_cfg.gui_x;
        anchor.y = g_cfg.gui_y;
    } else {
        GetCursorPos(&anchor);
    }

    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcMonitor; // physical monitor pixels
    int dpi = ui_dpi_for_monitor(mon);

    int x, y, w, h;
    if (g_cfg.gui_w > 0 && g_cfg.gui_h > 0) {
        w = g_cfg.gui_w;
        h = g_cfg.gui_h;
        x = g_cfg.gui_x;
        y = g_cfg.gui_y;
    } else {
        w = ui_scale(dpi, GUI_DEF_W);
        h = ui_scale(dpi, GUI_DEF_H);
        x = wa.left + (wa.right - wa.left - w) / 2;
        y = wa.top + (wa.bottom - wa.top - h) / 2;
    }

    RECT wr = { x, y, x + w, y + h };
    gui_clamp_to_monitor(&wr);
    x = wr.left;
    y = wr.top;
    w = wr.right - wr.left;
    h = wr.bottom - wr.top;

    g_hwndGui = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"JxlShotGuiClass",
        L"JXLShot",
        WS_POPUP | WS_CLIPCHILDREN,
        x, y, w, h,
        NULL, NULL, GetModuleHandleW(NULL), NULL);

    if (g_hwndGui) {
        ShowWindow(g_hwndGui, SW_SHOW);
        SetForegroundWindow(g_hwndGui);
        SetFocus(g_hwndGui);
    } else {
        MessageBoxW(NULL, L"Failed to create the jxlshot window.", L"jxlshot", MB_ICONERROR);
    }
}