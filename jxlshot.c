/* jxlshot.c — minimal command-line screenshot tool for Windows.
 *
 * Captures the primary monitor and saves it as JPEG XL (.jxl) in the
 * same directory as the executable. No output filename is needed —
 * the file is named from the current date/time automatically, so two
 * runs never collide.
 *
 * Usage:
 *   jxlshot.exe                capture, lossless (default)
 *   jxlshot.exe -q             lossy capture, default distance 1.0
 *   jxlshot.exe -q -d 3.0      lossy capture, distance 3.0 (lower = better)
 *   jxlshot.exe -w 3000        wait 3000 ms before capturing
 *
 * A step-by-step trace of every capture/encode call is appended to
 * jxlshot_debug.log next to the executable, and echoed to the console.
 * If a capture ever fails, check that file first — it records the
 * return code of every GDI/libjxl call, so a failure can be traced to
 * the exact step instead of just "encoding or saving failed".
 *
 * Build (MSYS2 / MinGW-w64):
 *   gcc -O2 -municode -o jxlshot.exe jxlshot.c -ljxl -lgdi32 -luser32
 *
 * (-municode is required because main() takes wchar_t argv[].)
 */

#define UNICODE
#define _UNICODE
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#include <stdarg.h>
#include <errno.h>

#include <jxl/encode.h>

/* ------------------------------------------------------------------ */
/* Debug logging                                                      */
/* ------------------------------------------------------------------ */

static FILE   *g_dbg = NULL;
static wchar_t g_exe_dir[MAX_PATH];

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
    fprintf(stderr, "[jxlshot] %s\n", buf);
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
/* Paths                                                              */
/* ------------------------------------------------------------------ */

static void init_paths(void) {
    wchar_t tmp[MAX_PATH];
    GetModuleFileNameW(NULL, tmp, MAX_PATH);
    wchar_t *slash = wcsrchr(tmp, L'\\');
    if (slash) *slash = 0;
    wcsncpy(g_exe_dir, tmp, MAX_PATH - 1);
    g_exe_dir[MAX_PATH - 1] = 0;
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

/* Output filename: <exe dir>\jxlshot_YYYYMMDD_HHMMSS_mmm.jxl
 * The timestamp (down to milliseconds) guarantees a unique name, so a
 * filename argument is never required and nothing is ever overwritten. */
static void build_out_path(wchar_t *path, int n) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snwprintf(path, n, L"%s\\jxlshot_%04d%02d%02d_%02d%02d%02d_%03d.jxl",
               g_exe_dir, st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    path[n - 1] = 0;
}

/* ------------------------------------------------------------------ */
/* DPI awareness — without this, GetSystemMetrics() can silently      */
/* return a scaled (wrong) screen size on high-DPI displays.          */
/* ------------------------------------------------------------------ */

static void set_dpi_aware(void) {
    typedef BOOL (WINAPI *Fn)(HANDLE);
    Fn f = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                              "SetProcessDpiAwarenessContext");
    if (f) f((HANDLE)(LONG_PTR)-4);   /* PER_MONITOR_AWARE_V2 */
    else   SetProcessDPIAware();
}

/* ------------------------------------------------------------------ */
/* Screen capture — primary monitor only                              */
/* ------------------------------------------------------------------ */

typedef struct {
    HBITMAP  hbmp;
    HDC      hdc;
    uint8_t *bits;   /* top-down BGRA */
    int      w, h;
} Grab;

static int grab_primary_monitor(Grab *g) {
    ZeroMemory(g, sizeof *g);
    g->w = GetSystemMetrics(SM_CXSCREEN);
    g->h = GetSystemMetrics(SM_CYSCREEN);
    dbg("grab_primary_monitor: %dx%d", g->w, g->h);
    if (g->w <= 0 || g->h <= 0) {
        dbg("grab_primary_monitor: invalid dimensions");
        return 0;
    }

    HDC sdc = GetDC(NULL);
    if (!sdc) {
        dbg("grab_primary_monitor: GetDC(NULL) failed, GetLastError=%lu", GetLastError());
        return 0;
    }
    g->hdc = CreateCompatibleDC(sdc);
    if (!g->hdc) {
        dbg("grab_primary_monitor: CreateCompatibleDC failed, GetLastError=%lu", GetLastError());
        ReleaseDC(NULL, sdc);
        return 0;
    }

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = g->w;
    bi.bmiHeader.biHeight      = -g->h;   /* negative = top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    g->hbmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&g->bits, NULL, 0);
    if (!g->hbmp) {
        dbg("grab_primary_monitor: CreateDIBSection failed, GetLastError=%lu", GetLastError());
        DeleteDC(g->hdc); g->hdc = NULL;
        ReleaseDC(NULL, sdc);
        return 0;
    }
    SelectObject(g->hdc, g->hbmp);

    if (!BitBlt(g->hdc, 0, 0, g->w, g->h, sdc, 0, 0, SRCCOPY)) {
        dbg("grab_primary_monitor: BitBlt failed, GetLastError=%lu", GetLastError());
        ReleaseDC(NULL, sdc);
        return 0;
    }
    GdiFlush();
    ReleaseDC(NULL, sdc);
    dbg("grab_primary_monitor: capture ok");
    return 1;
}

static void free_grab(Grab *g) {
    if (g->hdc)  DeleteDC(g->hdc);
    if (g->hbmp) DeleteObject(g->hbmp);
    ZeroMemory(g, sizeof *g);
}

/* ------------------------------------------------------------------ */
/* JPEG XL encoding                                                   */
/*                                                                     */
/* Encodes opaque RGB (screenshots have no meaningful alpha, so it's  */
/* dropped rather than carried through as a constant 0xFF channel).   */
/* Deliberately does NOT call JxlEncoderSetCodestreamLevel() — on     */
/* some libjxl versions that call left the encoder in an error state */
/* and made every JxlEncoderSetBasicInfo() call fail with API_USAGE.  */
/* ------------------------------------------------------------------ */

static int encode_jxl(const uint8_t *bgra, int w, int h,
                      int lossless, float distance,
                      uint8_t **out_buf, size_t *out_size) {
    int ok = 0;
    uint8_t *rgb = NULL;
    uint8_t *buf = NULL;
    JxlEncoderStatus st;

    dbg("encode_jxl: w=%d h=%d lossless=%d distance=%.3f", w, h, lossless, distance);

    JxlEncoder *enc = JxlEncoderCreate(NULL);
    if (!enc) { dbg("encode_jxl: JxlEncoderCreate returned NULL"); return 0; }

    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize                     = w;
    info.ysize                     = h;
    info.bits_per_sample           = 8;
    info.exponent_bits_per_sample  = 0;
    info.num_color_channels        = 3;
    info.alpha_bits                = 0;
    /* Bit-exact lossless requires skipping the XYB colour transform. */
    info.uses_original_profile     = lossless ? JXL_TRUE : JXL_FALSE;

    st = JxlEncoderSetBasicInfo(enc, &info);
    dbg("encode_jxl: JxlEncoderSetBasicInfo -> %d", (int)st);
    if (st != JXL_ENC_SUCCESS) {
        dbg("encode_jxl: SetBasicInfo error = %s", jxl_enc_err_name(JxlEncoderGetError(enc)));
        goto done;
    }

    {
        JxlColorEncoding ce;
        JxlColorEncodingSetToSRGB(&ce, JXL_FALSE);
        st = JxlEncoderSetColorEncoding(enc, &ce);
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
        if (st != JXL_ENC_SUCCESS) {
            dbg("encode_jxl: SetFrameLossless error = %s", jxl_enc_err_name(JxlEncoderGetError(enc)));
            goto done;
        }
        JxlEncoderSetFrameDistance(fs, 0.0f);
    } else {
        st = JxlEncoderSetFrameDistance(fs, distance);
        dbg("encode_jxl: JxlEncoderSetFrameDistance(%.3f) -> %d", distance, (int)st);
        if (st != JXL_ENC_SUCCESS) {
            dbg("encode_jxl: SetFrameDistance error = %s", jxl_enc_err_name(JxlEncoderGetError(enc)));
            goto done;
        }
    }

    {
        size_t npix = (size_t)w * h;
        rgb = (uint8_t *)malloc(npix * 3);
        if (!rgb) {
            dbg("encode_jxl: malloc(%lu) failed for RGB conversion", (unsigned long)(npix * 3));
            goto done;
        }
        for (size_t i = 0; i < npix; i++) {
            rgb[3*i + 0] = bgra[4*i + 2];   /* R */
            rgb[3*i + 1] = bgra[4*i + 1];   /* G */
            rgb[3*i + 2] = bgra[4*i + 0];   /* B */
        }

        JxlPixelFormat fmt = {3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
        st = JxlEncoderAddImageFrame(fs, &fmt, rgb, npix * 3);
        dbg("encode_jxl: JxlEncoderAddImageFrame -> %d", (int)st);
        if (st != JXL_ENC_SUCCESS) {
            dbg("encode_jxl: AddImageFrame error = %s", jxl_enc_err_name(JxlEncoderGetError(enc)));
            goto done;
        }
    }

    JxlEncoderCloseInput(enc);

    {
        size_t cap = 1 << 20;
        buf = (uint8_t *)malloc(cap);
        if (!buf) { dbg("encode_jxl: malloc(%lu) failed", (unsigned long)cap); goto done; }

        uint8_t *next = buf;
        size_t avail = cap;
        for (;;) {
            st = JxlEncoderProcessOutput(enc, &next, &avail);
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
                buf = nb; next = buf + used; avail = cap - used;
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
    }

done:
    free(rgb);
    free(buf);
    JxlEncoderDestroy(enc);
    return ok;
}

static int save_bgra_as_jxl(const uint8_t *bgra, int w, int h,
                            int lossless, float distance, const wchar_t *path) {
    uint8_t *buf = NULL;
    size_t   size = 0;

    if (!encode_jxl(bgra, w, h, lossless, distance, &buf, &size)) {
        dbg("save_bgra_as_jxl: encode_jxl failed, nothing written");
        return 0;
    }

    int ok = 0;
    errno = 0;
    FILE *f = _wfopen(path, L"wb");
    if (f) {
        size_t written = fwrite(buf, 1, size, f);
        dbg("save_bgra_as_jxl: fwrite %lu/%lu bytes to %S",
            (unsigned long)written, (unsigned long)size, path);
        ok = (written == size);
        if (!ok) dbg("save_bgra_as_jxl: short write, disk full or I/O error?");
        fclose(f);
    } else {
        dbg("save_bgra_as_jxl: _wfopen(%S) failed, errno=%d (%s)", path, errno, strerror(errno));
    }
    free(buf);
    return ok;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const wchar_t *argv0) {
    fwprintf(stderr,
        L"Usage: %s [-q] [-d distance] [-w ms]\n"
        L"  -q            lossy encoding (default: lossless)\n"
        L"  -d distance   lossy distance 0.0-25.0, lower = better (default 1.0, implies -q)\n"
        L"  -w ms         wait N milliseconds before capturing\n"
        L"No output filename is needed: the file is named from the current\n"
        L"date/time and saved next to jxlshot.exe.\n",
        argv0);
}

int wmain(int argc, wchar_t **argv) {
    int   lossless = 1;
    float distance = 1.0f;
    DWORD wait_ms  = 0;

    for (int i = 1; i < argc; i++) {
        if      (!wcscmp(argv[i], L"-q")) lossless = 0;
        else if (!wcscmp(argv[i], L"-d") && i + 1 < argc) {
            distance = (float)wcstod(argv[++i], NULL);
            lossless = 0;
        }
        else if (!wcscmp(argv[i], L"-w") && i + 1 < argc) {
            wait_ms = (DWORD)wcstol(argv[++i], NULL, 10);
        }
        else if (!wcscmp(argv[i], L"-h") || !wcscmp(argv[i], L"--help")) {
            usage(argv[0]); return 0;
        }
        else {
            fwprintf(stderr, L"error: unrecognized argument '%s'\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    set_dpi_aware();
    init_paths();
    dbg_init();
    dbg("main: lossless=%d distance=%.3f wait_ms=%lu", lossless, distance, (unsigned long)wait_ms);

    if (wait_ms) Sleep(wait_ms);

    Grab g;
    if (!grab_primary_monitor(&g)) {
        free_grab(&g);
        fwprintf(stderr, L"error: screen capture failed (see jxlshot_debug.log)\n");
        return 1;
    }

    wchar_t out_path[MAX_PATH];
    build_out_path(out_path, MAX_PATH);

    int rc = 1;
    if (save_bgra_as_jxl(g.bits, g.w, g.h, lossless, distance, out_path)) {
        wprintf(L"saved %s (%dx%d, %s)\n", out_path, g.w, g.h, lossless ? L"lossless" : L"lossy");
        rc = 0;
    } else {
        fwprintf(stderr, L"error: encoding or saving failed (see jxlshot_debug.log for details)\n");
    }

    free_grab(&g);
    return rc;
}
