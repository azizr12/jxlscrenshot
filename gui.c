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

// Close button shared by the strip and the settings window
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



// Default image

// Once the background encoder has closed the .jxl,
// it is opened with the Default image

// Path of the capture whose viewer should be launched. Written on the UI
// thread before the encode is queued, read by the EncodeWorker when the save
// finishes, so both sides go through the lock.
static SRWLOCK g_viewerLock = SRWLOCK_INIT;
static wchar_t  g_viewerPath[MAX_PATH];

// Called by the capture paths before the encode starts. The file does not
// exist yet, so the actual shell call is deferred until gui_capture_saved().
static void gui_open_capture_viewer(const Grab *g, const wchar_t *path) {
    (void)g;
    if (!g_cfg.show_preview || !path || !path[0]) return;

    AcquireSRWLockExclusive(&g_viewerLock);
    wcsncpy_s(g_viewerPath, MAX_PATH, path, _TRUNCATE);
    ReleaseSRWLockExclusive(&g_viewerLock);
}

// UI thread: launch the default image viewer for the finished capture.
static void gui_open_capture_now(void) {
    wchar_t path[MAX_PATH];

    AcquireSRWLockExclusive(&g_viewerLock);
    wcsncpy_s(path, MAX_PATH, g_viewerPath, _TRUNCATE);
    // A newer capture may have replaced the pending path while this message
    // was queued; only clear the one that actually finished saving.
    if (_wcsicmp(g_viewerPath, path) == 0) g_viewerPath[0] = L'\0';
    ReleaseSRWLockExclusive(&g_viewerLock);

    if (!path[0]) return;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;

    ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
}

// EncodeWorker side: the .jxl is complete. Only launch a viewer if this is the
// capture that is still waiting; a stale save must not pop up a window.
static void gui_capture_saved(const wchar_t *path) {
    if (!path || !path[0]) return;

    BOOL match = FALSE;
    AcquireSRWLockShared(&g_viewerLock);
    match = (g_viewerPath[0] && _wcsicmp(g_viewerPath, path) == 0);
    ReleaseSRWLockShared(&g_viewerLock);
    if (!match) return;

    if (g_hwndTray) PostMessageW(g_hwndTray, WM_GUI_OPEN_CAPTURE, 0, 0);
}

// Drop a pending hand-off, e.g. when the option is switched off.
static void gui_cancel_open_capture(void) {
    AcquireSRWLockExclusive(&g_viewerLock);
    g_viewerPath[0] = L'\0';
    ReleaseSRWLockExclusive(&g_viewerLock);
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

    // A capture started from here hides the strip first so it cannot appear in
    // its own screenshot.
    settings_hide_for_capture();

    g_guiPendingCapture = kind;
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

            // Close button: same flat red rectangle as the Settings window's.
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
                    // Stay hidden after the grab: the capture opens in the
                    // default image viewer and the tray icon is the only UI
                    // left.
                } else if (kind == 2) {
                    start_region_capture();
                    // The overlay is topmost and owns the screen; wait until it
                    // is gone before deciding what to do with this window.
                    SetTimer(hwnd, TIMER_GUI_REGION_DONE, GUI_REGION_POLL_MS, NULL);
                }
                return 0;
            }
            if (wp == TIMER_GUI_REGION_DONE) {
                if (g_hwndRegion) return 0; // selection still in progress
                KillTimer(hwnd, TIMER_GUI_REGION_DONE);
                // Keep the strip hidden so the tray icon is the only UI left
                // after a region capture too.
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
// so it cannot appear in its own screenshot.
static BOOL gui_is_visible(void) {
    return (g_hwndGui && IsWindowVisible(g_hwndGui)) ? TRUE : FALSE;
}

// True when a real position/size was saved.
static BOOL gui_has_saved_geometry(void) {
    if (g_cfg.gui_w <= 0 || g_cfg.gui_h <= 0) return FALSE;
    if (g_cfg.gui_x == 0 && g_cfg.gui_y == 0 &&
        g_cfg.gui_w == GUI_MIN_W && g_cfg.gui_h == GUI_MIN_H) return FALSE;
    return TRUE;
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
    BOOL hasGeometry = gui_has_saved_geometry();
    POINT anchor;
    if (hasGeometry) {
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
    if (hasGeometry) {
        w = g_cfg.gui_w;
        h = g_cfg.gui_h;
        x = g_cfg.gui_x;
        y = g_cfg.gui_y;
    } else {
        // On first run derive the size from the monitor instead of using a fixed
        // pixel size. 32% x 18% keeps the 600x190 design proportions on a
        // 1080p screen and scales sensibly to small laptop panels and 4K.
        int screenW = wa.right - wa.left;
        int screenH = wa.bottom - wa.top;

        w = MulDiv(screenW, 32, 100);
        h = MulDiv(screenH, 18, 100);

        int minW = ui_scale(dpi, GUI_MIN_W);
        int minH = ui_scale(dpi, GUI_MIN_H);
        int maxW = ui_scale(dpi, GUI_MAX_W);
        int maxH = ui_scale(dpi, GUI_MAX_H);
        if (w < minW) w = minW;
        if (h < minH) h = minH;
        if (w > maxW) w = maxW;
        if (h > maxH) h = maxH;
        if (w > screenW) w = screenW;
        if (h > screenH) h = screenH;

        // Centre inside the visible desktop so the first window is never
        // hidden under the taskbar or partly off-screen.
        RECT work = mi.rcWork;
        x = work.left + ((work.right - work.left) - w) / 2;
        y = work.top + ((work.bottom - work.top) - h) / 2;
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
