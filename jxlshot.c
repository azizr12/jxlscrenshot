/* jxlshot.c — tray screenshot tool for Windows, saving JPEG XL via libjxl.
 *
 * Tray mode (started without arguments):
 *   - Lives in the notification area (check the ^ overflow icons).
 *   - Clicking the icon opens a menu: Capture full screen / Capture region.
 *   - Output folder and quality come from jxlshot.ini (created next to the exe).
 *
 * Command-line mode (started with arguments):
 *   jxlshot.exe <output.jxl> [-l] [-d distance] [-a] [-w milliseconds]
 *     -l       lossless encoding
 *     -d       lossy distance 0.0-25.0, lower = better (default 1.0)
 *     -a       capture all monitors instead of the primary one
 *     -w       wait N milliseconds before capturing
 *
 * Debug log:
 *   Every run appends a step-by-step trace to "jxlshot_debug.log" next to
 *   the exe (created automatically). In CLI mode the same lines are echoed
 *   to the console. If a capture/save ever fails, check that file first —
 *   it records the return code of every capture/GDI/libjxl call, so you can
 *   see exactly which one failed instead of just "encoding or saving failed".
 *
 * Build (MSYS2 / MinGW-w64):
 *   gcc -O2 -mwindows -o jxlshot.exe jxlshot.c -ljxl -lgdi32 -luser32 -lshell32
 */

#define UNICODE
#define _UNICODE
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
#include <stdarg.h>
#include <errno.h>

#include <jxl/encode.h>

#ifndef NOTIFYICON_VERSION_4
#define NOTIFYICON_VERSION_4 4
#endif

#define WM_APP_TRAY (WM_APP + 1)
#define TRAY_ID     1

enum { ID_REGION = 1001, ID_FULL, ID_OPEN, ID_SETTINGS, ID_EXIT };

/* ------------------------------------------------------------------ */
/* Globals                                                            */
/* ------------------------------------------------------------------ */

static HINSTANCE g_hinst;
static HWND      g_hwnd;      /* hidden owner window for the tray icon */
static HWND      g_overlay;   /* region-selection overlay, if active   */
static HICON     g_icon;

typedef struct {
    wchar_t dir[MAX_PATH];    /* output folder                  */
    int     lossless;         /* 1 = lossless, 0 = lossy        */
    float   distance;         /* lossy distance, if lossless=0  */
} Settings;

static Settings g_set;
static wchar_t  g_exe_dir[MAX_PATH];
static wchar_t  g_ini_path[MAX_PATH];

typedef struct {
    HBITMAP  hbmp, hbmpDark;
    HDC      hdc,   hdcDark;
    uint8_t *bits;            /* BGRA capture */
    int      x, y, w, h;
} Grab;

static Grab  g_grab;
static struct { int have; int x0, y0, x1, y1; } g_drag;

/* ------------------------------------------------------------------ */
/* Debug logging                                                      */
/*                                                                     */
/* Writes to jxlshot_debug.log next to the exe on every run, and also */
/* echoes to the console when one is attached (CLI mode). Every       */
/* meaningful GDI/libjxl/file call in the capture pipeline logs its   */
/* return value here, so a failure can be traced to the exact step.   */
/* ------------------------------------------------------------------ */

static FILE *g_dbg = NULL;
static int   g_console_active = 0;

static void dbg(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = 0;

    if (g_dbg) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_dbg, "[%02d:%02d:%02d.%03d] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fflush(g_dbg);
    }
    if (g_console_active) {
        fprintf(stderr, "[jxlshot] %s\n", buf);
    }
}

static void dbg_init(void) {
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%s\\jxlshot_debug.log", g_exe_dir);
    path[MAX_PATH - 1] = 0;

    g_dbg = _wfopen(path, L"a");
    if (!g_dbg) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_dbg, "\n===== jxlshot run started %04d-%02d-%02d %02d:%02d:%02d =====\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fflush(g_dbg);
}

/* Human-readable name for a JxlEncoderError code, for the log. */
static const char *jxl_enc_err_name(JxlEncoderError e) {
    switch (e) {
        case JXL_ENC_ERR_OK:            return "OK";
        case JXL_ENC_ERR_GENERIC:       return "GENERIC";
        case JXL_ENC_ERR_OOM:           return "OUT_OF_MEMORY";
        case JXL_ENC_ERR_JBRD:          return "JPEG_BITSTREAM_RECONSTRUCTION_DATA";
        case JXL_ENC_ERR_BAD_INPUT:     return "BAD_INPUT";
        case JXL_ENC_ERR_NOT_SUPPORTED: return "NOT_SUPPORTED";
        case JXL_ENC_ERR_API_USAGE:     return "API_USAGE";
        default:                        return "UNKNOWN";
    }
}

/* ------------------------------------------------------------------ */
/* Settings (jxlshot.ini)                                             */
/* ------------------------------------------------------------------ */

static void init_paths(void) {
    wchar_t tmp[MAX_PATH];
    GetModuleFileNameW(NULL, tmp, MAX_PATH);
    wchar_t *slash = wcsrchr(tmp, L'\\');
    if (slash) *slash = 0;
    wcsncpy(g_exe_dir, tmp, MAX_PATH - 1);
    g_exe_dir[MAX_PATH - 1] = 0;
    _snwprintf(g_ini_path, MAX_PATH, L"%s\\jxlshot.ini", g_exe_dir);
    g_ini_path[MAX_PATH - 1] = 0;
}

static void default_output_dir(wchar_t *dir, int n) {
    wchar_t base[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", base, MAX_PATH) == 0)
        base[0] = 0;
    _snwprintf(dir, n, L"%s\\Pictures", base); dir[n-1] = 0;
    if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) {
        _snwprintf(dir, n, L"%s\\Desktop", base); dir[n-1] = 0;
    }
}

static void write_default_ini(void) {
    static const char tpl[] =
        "; jxlshot settings - restart jxlshot after editing\r\n"
        "[Output]\r\n"
        "; Folder where screenshots are saved.\r\n"
        "; Leave empty to use %%USERPROFILE%%\\Pictures\r\n"
        "Directory=\r\n"
        "; 1 = lossless (recommended for screenshots), 0 = lossy\r\n"
        "Lossless=1\r\n"
        "; Lossy quality distance 0.0-25.0, lower = better (ignored when Lossless=1)\r\n"
        "Distance=1.0\r\n";

    HANDLE f = CreateFileW(g_ini_path, GENERIC_WRITE, 0, NULL,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr;
    WriteFile(f, tpl, (DWORD)strlen(tpl), &wr, NULL);
    CloseHandle(f);
}

static void load_settings(void) {
    if (GetFileAttributesW(g_ini_path) == INVALID_FILE_ATTRIBUTES)
        write_default_ini();

    wchar_t buf[MAX_PATH];
    GetPrivateProfileStringW(L"Output", L"Directory", L"", buf, MAX_PATH, g_ini_path);
    if (buf[0]) {
        wcsncpy(g_set.dir, buf, MAX_PATH - 1);
        g_set.dir[MAX_PATH - 1] = 0;
    } else {
        default_output_dir(g_set.dir, MAX_PATH);
    }

    g_set.lossless = GetPrivateProfileIntW(L"Output", L"Lossless", 1, g_ini_path);

    wchar_t dstr[32];
    GetPrivateProfileStringW(L"Output", L"Distance", L"1.0", dstr, 32, g_ini_path);
    g_set.distance = (float)wcstod(dstr, NULL);
    if (g_set.distance < 0.0f || g_set.distance > 25.0f) {
        dbg("load_settings: Distance=%.3f out of range, clamping to 1.0", g_set.distance);
        g_set.distance = 1.0f;
    }

    /* Create the folder if needed; fall back if impossible. */
    if (GetFileAttributesW(g_set.dir) == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryW(g_set.dir, NULL))
            default_output_dir(g_set.dir, MAX_PATH);
    }

    dbg("load_settings: dir=%S lossless=%d distance=%.3f",
        g_set.dir, g_set.lossless, g_set.distance);
}

/* ------------------------------------------------------------------ */
/* JPEG XL encoding                                                   */
/* ------------------------------------------------------------------ */

static int encode_jxl(const uint8_t *rgba, int w, int h,
                      int lossless, float distance,
                      uint8_t **out_buf, size_t *out_size) {
    int ok = 0;
    uint8_t *buf = NULL;
    JxlEncoderStatus st;

    dbg("encode_jxl: start w=%d h=%d lossless=%d distance=%.3f",
        w, h, lossless, distance);

    JxlEncoder *enc = JxlEncoderCreate(NULL);
    if (!enc) { dbg("encode_jxl: JxlEncoderCreate returned NULL"); return 0; }

    /* Temporarily disabled for API_USAGE diagnosis.
       Some libjxl versions reject this call before SetBasicInfo.
       JxlEncoderSetCodestreamLevel(enc, -1);
    */

    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize              = w;
    info.ysize              = h;
    info.bits_per_sample    = 8;
    info.exponent_bits_per_sample = 0;
    info.alpha_bits         = 8;
    info.num_color_channels = 3;
    /* Bit-exact lossless requires the encoder to skip the XYB colour
     * transform. Without this, "lossless" screenshots are not actually
     * pixel-exact even though JxlEncoderSetFrameLossless is set below. */
    info.uses_original_profile = lossless ? JXL_TRUE : JXL_FALSE;

    st = JxlEncoderSetBasicInfo(enc, &info);
    dbg("encode_jxl: JxlEncoderSetBasicInfo -> %d", (int)st);
    if (st != JXL_ENC_SUCCESS) {
        dbg("encode_jxl: SetBasicInfo error = %s", jxl_enc_err_name(JxlEncoderGetError(enc)));
        goto done;
    }

    /* Explicitly tag the data as sRGB. Screen captures are sRGB, and
     * some libjxl versions behave inconsistently (or reject the frame
     * later) if the color encoding is never set at all. */
    {
        JxlColorEncoding color_encoding;
        JxlColorEncodingSetToSRGB(&color_encoding, JXL_FALSE);
        st = JxlEncoderSetColorEncoding(enc, &color_encoding);
        dbg("encode_jxl: JxlEncoderSetColorEncoding -> %d", (int)st);
        if (st != JXL_ENC_SUCCESS) {
            dbg("encode_jxl: SetColorEncoding error = %s", jxl_enc_err_name(JxlEncoderGetError(enc)));
            goto done;
        }
    }

    JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    if (!fs) { dbg("encode_jxl: JxlEncoderFrameSettingsCreate returned NULL"); goto done; }

    if (lossless) {
        st = JxlEncoderSetFrameLossless(fs, JXL_TRUE);
        dbg("encode_jxl: JxlEncoderSetFrameLossless(TRUE) -> %d", (int)st);
        JxlEncoderSetFrameDistance(fs, 0.0f);
    } else {
        st = JxlEncoderSetFrameDistance(fs, distance);
        dbg("encode_jxl: JxlEncoderSetFrameDistance(%.3f) -> %d", distance, (int)st);
    }

    JxlPixelFormat fmt = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    st = JxlEncoderAddImageFrame(fs, &fmt, rgba, (size_t)w * h * 4);
    dbg("encode_jxl: JxlEncoderAddImageFrame -> %d", (int)st);
    if (st != JXL_ENC_SUCCESS) {
        dbg("encode_jxl: AddImageFrame error = %s", jxl_enc_err_name(JxlEncoderGetError(enc)));
        goto done;
    }
    JxlEncoderCloseInput(enc);

    size_t cap = 1 << 20;
    buf = (uint8_t *)malloc(cap);
    if (!buf) { dbg("encode_jxl: malloc(%lu) failed", (unsigned long)cap); goto done; }
    uint8_t *next = buf;
    size_t avail = cap;

    for (;;) {
        st = JxlEncoderProcessOutput(enc, &next, &avail);
        dbg("encode_jxl: JxlEncoderProcessOutput -> %d (cap=%lu avail=%lu)",
            (int)st, (unsigned long)cap, (unsigned long)avail);
        if (st == JXL_ENC_SUCCESS) break;
        if (st == JXL_ENC_NEED_MORE_OUTPUT) {
            size_t used = (size_t)(next - buf);
            cap *= 2;
            uint8_t *nb = (uint8_t *)realloc(buf, cap);
            if (!nb) {
                dbg("encode_jxl: realloc(%lu) failed", (unsigned long)cap);
                free(buf); buf = NULL;
                goto done;
            }
            buf  = nb;
            next = buf + used;
            avail = cap - used;
            continue;
        }
        dbg("encode_jxl: ProcessOutput error = %s", jxl_enc_err_name(JxlEncoderGetError(enc)));
        free(buf); buf = NULL;
        goto done;
    }
    *out_buf  = buf;
    *out_size = cap - avail;
    dbg("encode_jxl: success, %lu bytes", (unsigned long)*out_size);
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

static void build_out_path(wchar_t *path, int n) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snwprintf(path, n, L"%s\\jxlshot_%04d%02d%02d_%02d%02d%02d.jxl",
               g_set.dir, st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    path[n-1] = 0;
}

static int save_rgba_as_jxl(const uint8_t *rgba, int w, int h,
                            int lossless, float distance,
                            const wchar_t *path) {
    uint8_t *buf = NULL;
    size_t   size = 0;
    int ok = 0;

    dbg("save_rgba_as_jxl: target=%S", path);

    if (!encode_jxl(rgba, w, h, lossless, distance, &buf, &size)) {
        dbg("save_rgba_as_jxl: encode_jxl failed, nothing written");
        return 0;
    }

    errno = 0;
    FILE *f = _wfopen(path, L"wb");
    if (f) {
        size_t written = fwrite(buf, 1, size, f);
        dbg("save_rgba_as_jxl: fwrite %lu/%lu bytes to %S",
            (unsigned long)written, (unsigned long)size, path);
        ok = (written == size);
        if (!ok) dbg("save_rgba_as_jxl: short write, disk full or I/O error?");
        fclose(f);
    } else {
        dbg("save_rgba_as_jxl: _wfopen(%S) failed, errno=%d (%s)",
            path, errno, strerror(errno));
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

static int grab_screen(Grab *g, int all_monitors) {
    ZeroMemory(g, sizeof *g);
    if (all_monitors) {
        g->x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        g->y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        g->w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        g->h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    } else {
        g->x = 0;
        g->y = 0;
        g->w = GetSystemMetrics(SM_CXSCREEN);
        g->h = GetSystemMetrics(SM_CYSCREEN);
    }
    dbg("grab_screen: all_monitors=%d region=(%d,%d) %dx%d",
        all_monitors, g->x, g->y, g->w, g->h);
    if (g->w <= 0 || g->h <= 0) {
        dbg("grab_screen: invalid dimensions, aborting");
        return 0;
    }

    HDC sdc = GetDC(NULL);
    if (!sdc) {
        dbg("grab_screen: GetDC(NULL) failed, GetLastError=%lu", GetLastError());
        return 0;
    }
    g->hdc = CreateCompatibleDC(sdc);
    if (!g->hdc) {
        dbg("grab_screen: CreateCompatibleDC failed, GetLastError=%lu", GetLastError());
        ReleaseDC(NULL, sdc);
        return 0;
    }

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = g->w;
    bi.bmiHeader.biHeight      = -g->h;      /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    g->hbmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&g->bits, NULL, 0);
    if (!g->hbmp) {
        dbg("grab_screen: CreateDIBSection (main) failed, GetLastError=%lu", GetLastError());
        DeleteDC(g->hdc); g->hdc = NULL;
        ReleaseDC(NULL, sdc);
        return 0;
    }
    SelectObject(g->hdc, g->hbmp);

    if (!BitBlt(g->hdc, 0, 0, g->w, g->h, sdc, g->x, g->y, SRCCOPY)) {
        dbg("grab_screen: BitBlt failed, GetLastError=%lu", GetLastError());
        ReleaseDC(NULL, sdc);
        return 0;
    }
    GdiFlush();
    dbg("grab_screen: main capture ok");

    /* Darkened copy for the overlay background (~45% brightness).
     * NOTE: this must happen before sdc is released below — it used to
     * be created from an already-released DC, which is undefined
     * behaviour and could intermittently fail. */
    uint8_t *darkbits = NULL;
    g->hbmpDark = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&darkbits, NULL, 0);
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
    } else {
        dbg("grab_screen: CreateDIBSection (dark overlay) failed, GetLastError=%lu",
            GetLastError());
        /* Non-fatal: only the region-select overlay dimming is affected. */
    }

    ReleaseDC(NULL, sdc);
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
/* Tray capture actions                                               */
/* ------------------------------------------------------------------ */

static void do_full_capture(void) {
    Grab g;
    if (!grab_screen(&g, 1)) {
        dbg("do_full_capture: grab_screen failed");
        free_grab(&g);   /* grab_screen may have partially allocated GDI objects */
        show_balloon(L"JXL Screenshot", L"Capture failed.");
        return;
    }

    uint8_t *rgba = (uint8_t *)malloc((size_t)g.w * g.h * 4);
    if (rgba) {
        convert_bgra_to_rgba(g.bits, rgba, (size_t)g.w * g.h);
        wchar_t path[MAX_PATH];
        build_out_path(path, MAX_PATH);
        if (save_rgba_as_jxl(rgba, g.w, g.h, g_set.lossless, g_set.distance, path))
            show_balloon(L"JXL Screenshot", path);
        else
            show_balloon(L"JXL Screenshot", L"Encoding or saving failed.");
        free(rgba);
    } else {
        dbg("do_full_capture: malloc for rgba buffer failed");
    }
    free_grab(&g);
}

static void begin_region(void) {
    if (g_overlay) return;
    if (!grab_screen(&g_grab, 1)) {
        dbg("begin_region: grab_screen failed");
        free_grab(&g_grab);
        show_balloon(L"JXL Screenshot", L"Capture failed.");
        return;
    }
    g_drag.have = 0;
    g_overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                L"jxlshot_overlay", L"", WS_POPUP,
                                g_grab.x, g_grab.y, g_grab.w, g_grab.h,
                                NULL, NULL, g_hinst, NULL);
    if (!g_overlay) {
        dbg("begin_region: CreateWindowExW(overlay) failed, GetLastError=%lu", GetLastError());
        free_grab(&g_grab);
        return;
    }
    ShowWindow(g_overlay, SW_SHOW);
    UpdateWindow(g_overlay);
    SetForegroundWindow(g_overlay);
}

static void cancel_region(void) {
    if (g_overlay) { DestroyWindow(g_overlay); g_overlay = NULL; }
    free_grab(&g_grab);
    g_drag.have = 0;
}

static void finish_region(const RECT *r) {
    int w = r->right - r->left, h = r->bottom - r->top;
    dbg("finish_region: %dx%d", w, h);
    uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (rgba) {
        for (int yy = 0; yy < h; yy++) {
            const uint8_t *srow = g_grab.bits
                + ((size_t)(r->top + yy) * g_grab.w + r->left) * 4;
            convert_bgra_to_rgba(srow, rgba + (size_t)yy * w * 4, (size_t)w);
        }
        wchar_t path[MAX_PATH];
        build_out_path(path, MAX_PATH);
        if (save_rgba_as_jxl(rgba, w, h, g_set.lossless, g_set.distance, path))
            show_balloon(L"JXL Screenshot", path);
        else
            show_balloon(L"JXL Screenshot", L"Encoding or saving failed.");
        free(rgba);
    } else {
        dbg("finish_region: malloc for rgba buffer failed");
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
    AppendMenuW(m, MF_STRING, ID_FULL,    L"Capture full screen (all monitors)");
    AppendMenuW(m, MF_STRING, ID_REGION,  L"Capture region");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_OPEN,     L"Open output folder");
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Open settings file");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT,     L"Exit");

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
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            show_menu(hw);
            return 0;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_FULL:     do_full_capture(); break;
        case ID_REGION:   begin_region();    break;
        case ID_OPEN:
            ShellExecuteW(NULL, L"open", g_set.dir, NULL, NULL, SW_SHOWNORMAL);
            break;
        case ID_SETTINGS:
            ShellExecuteW(NULL, L"open", g_ini_path, NULL, NULL, SW_SHOWNORMAL);
            break;
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
/* Command-line mode                                                  */
/* ------------------------------------------------------------------ */

static int run_cli(int argc, wchar_t **argv) {
    int own_console = 0;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (AllocConsole()) own_console = 1;
    }
    FILE *dummy;
    dummy = freopen("CONOUT$", "wb", stdout);
    dummy = freopen("CONOUT$", "wb", stderr);
    dummy = freopen("CONIN$",  "rb", stdin);
    (void)dummy;
    g_console_active = 1;

    const wchar_t *out_path = NULL;
    float distance = 1.0f;
    int lossless = 0, all_monitors = 0;
    DWORD wait_ms = 0;

    for (int i = 1; i < argc; i++) {
        if      (!wcscmp(argv[i], L"-l")) lossless = 1;
        else if (!wcscmp(argv[i], L"-a")) all_monitors = 1;
        else if (!wcscmp(argv[i], L"-d") && i + 1 < argc) distance = (float)wcstod(argv[++i], NULL);
        else if (!wcscmp(argv[i], L"-w") && i + 1 < argc) wait_ms = (DWORD)wcstol(argv[++i], NULL, 10);
        else if (argv[i][0] != L'-' && !out_path) out_path = argv[i];
        else {
            dbg("run_cli: unrecognized/duplicate argument at index %d", i);
            fwprintf(stderr,
                L"Usage: jxlshot.exe <output.jxl> [-l] [-d distance] [-a] [-w ms]\n"
                L"Run without arguments for tray mode.\n");
            return 2;
        }
    }
    if (!out_path) {
        fwprintf(stderr, L"Missing output file name. Usage: jxlshot.exe <output.jxl> [options]\n");
        return 2;
    }

    dbg("run_cli: out_path=%S lossless=%d distance=%.3f all_monitors=%d wait_ms=%lu",
        out_path, lossless, distance, all_monitors, (unsigned long)wait_ms);

    if (wait_ms) Sleep(wait_ms);

    Grab g;
    if (!grab_screen(&g, all_monitors)) {
        dbg("run_cli: grab_screen failed");
        free_grab(&g);
        fwprintf(stderr, L"error: screen capture failed\n");
        return 1;
    }

    int rc = 1;
    uint8_t *rgba = (uint8_t *)malloc((size_t)g.w * g.h * 4);
    if (rgba) {
        convert_bgra_to_rgba(g.bits, rgba, (size_t)g.w * g.h);
        if (save_rgba_as_jxl(rgba, g.w, g.h, lossless, distance, out_path)) {
            fwprintf(stderr, L"saved %s (%dx%d, %s)\n", out_path, g.w, g.h,
                     lossless ? L"lossless" : L"lossy");
            rc = 0;
        } else {
            fwprintf(stderr,
                L"error: encoding or saving failed (see %s\\jxlshot_debug.log for details)\n",
                g_exe_dir);
        }
        free(rgba);
    } else {
        dbg("run_cli: malloc for rgba buffer failed");
        fwprintf(stderr, L"error: out of memory\n");
    }
    free_grab(&g);

    if (own_console) {
        fwprintf(stderr, L"\nPress Enter to exit...");
        getchar();
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int nShow) {
    (void)hPrev; (void)cmd; (void)nShow;

    set_dpi_aware();

    /* Needed in both CLI and tray mode, so the debug log always ends up
     * next to the exe regardless of which mode we end up running. */
    init_paths();
    dbg_init();

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2) {
        dbg("WinMain: CLI mode, argc=%d", argc);
        return run_cli(argc, argv);
    }
    dbg("WinMain: tray mode");

    /* Tray mode: single instance. */
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\jxlshot.single");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    load_settings();
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
    if (!g_hwnd) { dbg("WinMain: CreateWindowExW(tray) failed"); return 1; }

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
    dbg("WinMain: exiting normally");
    return 0;
}
