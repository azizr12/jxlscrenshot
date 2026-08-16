/* jxlshot.c — tray-resident screenshot tool for Windows, saving JPEG XL via libjxl.
 *
 *  - Tray icon: left-click = capture region, right-click = menu
 *  - Menu: Capture region / Capture full screen (all monitors) / Open folder / Exit
 *  - Greenshot-style region selection: darkened overlay, drag rectangle, ESC or
 *    right-click cancels
 *  - Output: lossless .jxl saved to %USERPROFILE%\Pictures (falls back to Desktop)
 *
 * Build from a terminal with MSYS2 / MinGW-w64 (no Visual Studio):
 *   pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-libjxl
 *   gcc -O2 -mwindows -o jxlshot.exe jxlshot.c -ljxl -lgdi32 -luser32 -lshell32
 */

#define UNICODE
#define _UNICODE
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>

#include <jxl/encode.h>

#ifndef NOTIFYICON_VERSION_4
#define NOTIFYICON_VERSION_4 4
#endif

#define WM_APP_TRAY (WM_APP + 1)
#define TRAY_ID     1

enum { ID_REGION = 1001, ID_FULL, ID_OPEN, ID_EXIT };

/* ------------------------------------------------------------------ */
/* Globals                                                            */
/* ------------------------------------------------------------------ */

static HINSTANCE g_hinst;
static HWND      g_hwnd;      /* hidden owner window for the tray icon   */
static HWND      g_overlay;   /* region-selection overlay, if active     */
static HICON     g_icon;

typedef struct {
    HBITMAP  hbmp, hbmpDark;
    HDC      hdc,   hdcDark;
    uint8_t *bits;            /* BGRA capture                            */
    int      x, y, w, h;      /* virtual-screen origin and size          */
} Grab;

static Grab  g_grab;
static struct { int have; int x0, y0, x1, y1; } g_drag;

/* ------------------------------------------------------------------ */
/* JPEG XL encoding                                                   */
/* ------------------------------------------------------------------ */

static int encode_jxl_lossless(const uint8_t *rgba, int w, int h,
                               uint8_t **out_buf, size_t *out_size) {
    int ok = 0;
    uint8_t *buf = NULL;

    JxlEncoder *enc = JxlEncoderCreate(NULL);
    if (!enc) return 0;

    JxlEncoderSetCodestreamLevel(enc, -1);

    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize              = w;
    info.ysize              = h;
    info.bits_per_sample    = 8;
    info.exponent_bits_per_sample = 0;
    info.alpha_bits         = 8;
    info.num_color_channels = 3;
    if (JxlEncoderSetBasicInfo(enc, &info) != JXL_ENC_SUCCESS) goto done;

    JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    if (!fs) goto done;
    JxlEncoderSetFrameLossless(fs, JXL_TRUE);
    JxlEncoderSetFrameDistance(fs, 0.0f);

    JxlPixelFormat fmt = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    if (JxlEncoderAddImageFrame(fs, &fmt, rgba, (size_t)w * h * 4)
        != JXL_ENC_SUCCESS) goto done;
    JxlEncoderCloseInput(enc);

    size_t cap = 1 << 20;
    buf = (uint8_t *)malloc(cap);
    if (!buf) goto done;
    uint8_t *next = buf;
    size_t avail = cap;

    for (;;) {
        JxlEncoderStatus st = JxlEncoderProcessOutput(enc, &next, &avail);
        if (st == JXL_ENC_SUCCESS) break;
        if (st == JXL_ENC_NEED_MORE_OUTPUT) {
            size_t used = (size_t)(next - buf);
            cap *= 2;
            uint8_t *nb = (uint8_t *)realloc(buf, cap);
            if (!nb) { free(buf); buf = NULL; goto done; }
            buf  = nb;
            next = buf + used;
            avail = cap - used;
            continue;
        }
        free(buf); buf = NULL;
        goto done;
    }
    *out_buf  = buf;
    *out_size = cap - avail;
    buf = NULL;
    ok = 1;

done:
    free(buf);
    JxlEncoderDestroy(enc);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static void set_dpi_aware(void) {
    typedef BOOL (WINAPI *Fn)(HANDLE);
    Fn f = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                              "SetProcessDpiAwarenessContext");
    if (f) f((HANDLE)(LONG_PTR)-4);     /* PER_MONITOR_AWARE_V2 */
    else   SetProcessDPIAware();
}

static void convert_bgra_to_rgba(const uint8_t *src, uint8_t *dst, size_t n) {
    for (size_t i = 0; i < n; i++) {
        dst[4*i + 0] = src[4*i + 2];
        dst[4*i + 1] = src[4*i + 1];
        dst[4*i + 2] = src[4*i + 0];
        dst[4*i + 3] = 0xFF;
    }
}

static void get_output_dir(wchar_t *dir, int n) {
    wchar_t base[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", base, MAX_PATH) == 0)
        base[0] = L'\0';
    _snwprintf(dir, n, L"%s\\Pictures", base); dir[n-1] = 0;
    if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) {
        _snwprintf(dir, n, L"%s\\Desktop", base); dir[n-1] = 0;
    }
}

static void build_out_path(wchar_t *path, int n) {
    wchar_t dir[MAX_PATH];
    SYSTEMTIME st;
    get_output_dir(dir, MAX_PATH);
    GetLocalTime(&st);
    _snwprintf(path, n, L"%s\\jxlshot_%04d%02d%02d_%02d%02d%02d.jxl",
               dir, st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    path[n-1] = 0;
}

static int save_rgba_as_jxl(const uint8_t *rgba, int w, int h,
                            const wchar_t *path) {
    uint8_t *buf = NULL;
    size_t   size = 0;
    int ok = 0;
    if (!encode_jxl_lossless(rgba, w, h, &buf, &size)) return 0;
    FILE *f = _wfopen(path, L"wb");
    if (f) {
        ok = (fwrite(buf, 1, size, f) == size);
        fclose(f);
    }
    free(buf);
    return ok;
}

static void show_balloon(const wchar_t *title, const wchar_t *msg) {
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd   = g_hwnd;
    nid.uID    = TRAY_ID;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy(nid.szInfoTitle, title, 63);  nid.szInfoTitle[63] = 0;
    wcsncpy(nid.szInfo,      msg,  255);  nid.szInfo[255] = 0;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

/* ------------------------------------------------------------------ */
/* Screen capture                                                     */
/* ------------------------------------------------------------------ */

static int grab_screen(Grab *g) {
    ZeroMemory(g, sizeof *g);
    g->x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g->y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g->w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g->h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (g->w <= 0 || g->h <= 0) return 0;

    HDC sdc = GetDC(NULL);
    g->hdc  = CreateCompatibleDC(sdc);

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = g->w;
    bi.bmiHeader.biHeight      = -g->h;      /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    g->hbmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&g->bits, NULL, 0);
    if (!g->hbmp) { DeleteDC(g->hdc); ReleaseDC(NULL, sdc); return 0; }
    SelectObject(g->hdc, g->hbmp);

    if (!BitBlt(g->hdc, 0, 0, g->w, g->h, sdc, g->x, g->y, SRCCOPY)) {
        ReleaseDC(NULL, sdc);
        return 0;   /* caller frees via free_grab */
    }
    GdiFlush();
    ReleaseDC(NULL, sdc);

    /* Darkened copy for the overlay background (~45% brightness). */
    BITMAPINFO bd = bi;
    uint8_t *darkbits = NULL;
    g->hbmpDark = CreateDIBSection(sdc, &bd, DIB_RGB_COLORS, (void **)&darkbits, NULL, 0);
    if (g->hbmpDark) {
        g->hdcDark = CreateCompatibleDC(NULL);
        SelectObject(g->hdcDark, g->hbmpDark);
        size_t npix = (size_t)g->w * g->h;
        for (size_t i = 0; i < npix; i++) {
            darkbits[4*i + 0] = (uint8_t)(g->bits[4*i + 0] * 115 / 256);
            darkbits[4*i + 1] = (uint8_t)(g->bits[4*i + 1] * 115 / 256);
            darkbits[4*i + 2] = (uint8_t)(g->bits[4*i + 2] * 115 / 256);
            darkbits[4*i + 3] = 0xFF;
        }
    }
    return 1;
}

static void free_grab(Grab *g) {
    if (g->hdcDark) DeleteDC(g->hdcDark);
    if (g->hdc)     DeleteDC(g->hdc);
    if (g->hbmpDark) DeleteObject(g->hbmpDark);
    if (g->hbmp)     DeleteObject(g->hbmp);
    ZeroMemory(g, sizeof *g);
}

/* ------------------------------------------------------------------ */
/* Capture actions                                                    */
/* ------------------------------------------------------------------ */

static void do_full_capture(void) {
    Grab g;
    if (!grab_screen(&g)) { show_balloon(L"JXL Screenshot", L"Capture failed."); return; }

    uint8_t *rgba = (uint8_t *)malloc((size_t)g.w * g.h * 4);
    wchar_t  path[MAX_PATH];
    if (rgba) {
        convert_bgra_to_rgba(g.bits, rgba, (size_t)g.w * g.h);
        build_out_path(path, MAX_PATH);
        if (save_rgba_as_jxl(rgba, g.w, g.h, path))
            show_balloon(L"JXL Screenshot", path);
        else
            show_balloon(L"JXL Screenshot", L"Encoding or saving failed.");
        free(rgba);
    }
    free_grab(&g);
}

static void begin_region(void) {
    if (g_overlay) return;
    if (!grab_screen(&g_grab)) {
        show_balloon(L"JXL Screenshot", L"Capture failed.");
        return;
    }
    g_drag.have = 0;
    g_overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                L"jxlshot_overlay", L"", WS_POPUP,
                                g_grab.x, g_grab.y, g_grab.w, g_grab.h,
                                NULL, NULL, g_hinst, NULL);
    if (!g_overlay) { free_grab(&g_grab); return; }
    ShowWindow(g_overlay, SW_SHOW);
    UpdateWindow(g_overlay);
    SetForegroundWindow(g_overlay);   /* so ESC reaches us */
}

static void cancel_region(void) {
    if (g_overlay) { DestroyWindow(g_overlay); g_overlay = NULL; }
    free_grab(&g_grab);
    g_drag.have = 0;
}

static void finish_region(const RECT *r) {
    int w = r->right - r->left, h = r->bottom - r->top;
    uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (rgba) {
        for (int yy = 0; yy < h; yy++) {
            const uint8_t *srow = g_grab.bits
                + ((size_t)(r->top + yy) * g_grab.w + r->left) * 4;
            convert_bgra_to_rgba(srow, rgba + (size_t)yy * w * 4, (size_t)w);
        }
        wchar_t path[MAX_PATH];
        build_out_path(path, MAX_PATH);
        if (save_rgba_as_jxl(rgba, w, h, path))
            show_balloon(L"JXL Screenshot", path);
        else
            show_balloon(L"JXL Screenshot", L"Encoding or saving failed.");
        free(rgba);
    }
    cancel_region();
}

/* ------------------------------------------------------------------ */
/* Tray icon                                                          */
/* ------------------------------------------------------------------ */

static HICON make_icon(void) {
    const int S = 16;
    HDC dc = GetDC(NULL);

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = S;
    bi.bmiHeader.biHeight      = -S;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    uint32_t *px = NULL;
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);

    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            int inBody = (x >= 3 && x <= 12 && y >= 5 && y <= 11);
            int border = inBody && (x == 3 || x == 12 || y == 5 || y == 11);
            int lens   = (x - 8) * (x - 8) + (y - 8) * (y - 8) <= 4;
            int notch  = (x >= 6 && x <= 9 && y >= 3 && y < 5);
            uint32_t c;
            if (border || lens || notch)  c = 0xFF000000 | (255u<<16) | (255u<<8) | 255u;
            else if (inBody)              c = 0xFF000000 | (40u<<16)  | (60u<<8)  | 90u;
            else                          c = 0xFF000000 | (26u<<16)  | (34u<<8)  | 50u;
            px[y * S + x] = c;
        }
    }

    /* AND mask: all zeros => fully opaque icon. */
    BITMAPINFO bm = bi;
    bm.bmiHeader.biBitCount = 1;
    void *maskbits = NULL;
    HBITMAP mask = CreateDIBSection(dc, &bm, DIB_RGB_COLORS, &maskbits, NULL, 0);
    memset(maskbits, 0, ((S + 31) / 32) * 4 * S);

    ICONINFO ii;
    ii.fIcon    = TRUE;
    ii.xHotspot = 0;
    ii.yHotspot = 0;
    ii.hbmMask  = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(color);
    DeleteObject(mask);
    ReleaseDC(NULL, dc);
    return icon;
}

static void tray_add(void) {
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof nid);
    nid.cbSize          = sizeof nid;
    nid.hWnd            = g_hwnd;
    nid.uID             = TRAY_ID;
    nid.uFlags          = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon           = g_icon;
    wcsncpy(nid.szTip, L"JXL Screenshot", 127);
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

static void tray_remove(void) {
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd   = g_hwnd;
    nid.uID    = TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void show_menu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_REGION, L"Capture region (left-click tray icon)");
    AppendMenuW(m, MF_STRING, ID_FULL,   L"Capture full screen");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_OPEN,   L"Open output folder");
    AppendMenuW(m, MF_STRING, ID_EXIT,   L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* ------------------------------------------------------------------ */
/* Windows                                                            */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK overlay_wndproc(HWND hw, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);
        if (g_grab.hdcDark)
            BitBlt(dc, 0, 0, g_grab.w, g_grab.h, g_grab.hdcDark, 0, 0, SRCCOPY);

        if (!g_drag.have) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(235, 235, 235));
            RECT rc = {0, 0, g_grab.w, g_grab.h};
            DrawTextW(dc, L"Drag to select a region - ESC or right-click to cancel",
                      -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            int x0 = min(g_drag.x0, g_drag.x1), x1 = max(g_drag.x0, g_drag.x1);
            int y0 = min(g_drag.y0, g_drag.y1), y1 = max(g_drag.y0, g_drag.y1);
            if (x1 > x0 && y1 > y0 && g_grab.hdc) {
                BitBlt(dc, x0, y0, x1 - x0, y1 - y0, g_grab.hdc, x0, y0, SRCCOPY);
                RECT r = {x0, y0, x1, y1};
                HBRUSH br = CreateSolidBrush(RGB(0, 200, 255));
                FrameRect(dc, &r, br);
                DeleteObject(br);

                wchar_t label[48];
                _snwprintf(label, 48, L"%dx%d", x1 - x0, y1 - y0); label[47] = 0;
                SetBkColor(dc, RGB(20, 20, 20));
                SetTextColor(dc, RGB(255, 255, 255));
                TextOutW(dc, x0, (y1 + 18 < g_grab.h) ? y1 + 4 : y0 - 20,
                         label, (int)wcslen(label));
            }
        }
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        g_drag.x0 = g_drag.x1 = max(0, min(LOWORD(lp), g_grab.w - 1));
        g_drag.y0 = g_drag.y1 = max(0, min(HIWORD(lp), g_grab.h - 1));
        g_drag.have = 1;
        SetCapture(hw);
        InvalidateRect(hw, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (g_drag.have) {
            g_drag.x1 = max(0, min(LOWORD(lp), g_grab.w - 1));
            g_drag.y1 = max(0, min(HIWORD(lp), g_grab.h - 1));
            InvalidateRect(hw, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_drag.have) {
            ReleaseCapture();
            int x0 = min(g_drag.x0, g_drag.x1), x1 = max(g_drag.x0, g_drag.x1);
            int y0 = min(g_drag.y0, g_drag.y1), y1 = max(g_drag.y0, g_drag.y1);
            if (x1 - x0 >= 3 && y1 - y0 >= 3) {
                RECT r = {x0, y0, x1, y1};
                finish_region(&r);
            } else {
                g_drag.have = 0;
                InvalidateRect(hw, NULL, FALSE);
            }
        }
        return 0;
    case WM_RBUTTONDOWN:
        if (g_drag.have) { g_drag.have = 0; ReleaseCapture(); InvalidateRect(hw, NULL, FALSE); }
        else             cancel_region();
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { cancel_region(); return 0; }
        break;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

static LRESULT CALLBACK tray_wndproc(HWND hw, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_APP_TRAY:
        switch (HIWORD(lp)) {
        case WM_LBUTTONUP:   begin_region();             return 0;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: show_menu(hw);              return 0;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_REGION: begin_region();   break;
        case ID_FULL:   do_full_capture(); break;
        case ID_OPEN: {
            wchar_t dir[MAX_PATH];
            get_output_dir(dir, MAX_PATH);
            ShellExecuteW(NULL, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
            break;
        }
        case ID_EXIT:
            tray_remove();
            DestroyWindow(hw);
            break;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int nShow) {
    (void)hPrev; (void)cmd; (void)nShow;

    /* Single instance. */
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\jxlshot.single");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    set_dpi_aware();
    g_hinst = hInst;

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc   = tray_wndproc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"jxlshot_tray";
    RegisterClassW(&wc);

    WNDCLASSW oc;
    ZeroMemory(&oc, sizeof oc);
    oc.lpfnWndProc   = overlay_wndproc;
    oc.hInstance     = hInst;
    oc.hCursor       = LoadCursorW(NULL, IDC_CROSS);
    oc.lpszClassName = L"jxlshot_overlay";
    RegisterClassW(&oc);

    g_hwnd = CreateWindowExW(0, L"jxlshot_tray", L"", WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    g_icon = make_icon();
    tray_add();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    tray_remove();
    if (g_icon) DestroyIcon(g_icon);
    if (mtx) CloseHandle(mtx);
    return 0;
}
