/* jxlshot.c — minimal command-line screenshot tool for Windows.
*
* Captures the primary monitor and saves it as JPEG XL (.jxl).
*
* Usage:
*   jxlshot.exe                capture, lossless (default)
*   jxlshot.exe -q             lossy capture, default distance 1.0
*   jxlshot.exe -q -d 3.0      lossy capture, distance 3.0 (lower = better)
*   jxlshot.exe -w 3000        wait 3000 ms before capturing
*
* Configuration is read from jxlshot.ini located next to the executable.
* Debug logs are written to %TEMP%\jxlshot_debug.log.
*
* Build (MSYS2 / MinGW-w64):
*   gcc -O2 -mwindows -o jxlshot.exe jxlshot.c -ljxl -lgdi32 -luser32 -lshell32 -lole32
*/
#define UNICODE
#define _UNICODE
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <jxl/encode.h>

/* ------------------------------------------------------------------ */
/* Configuration (INI)                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    int     debug_enabled;
    int     lossless;
    float   distance;
    int     show_cursor;
    wchar_t export_path[MAX_PATH];
    UINT    hk_full_mod;
    UINT    hk_full_vk;
    UINT    hk_region_mod;
    UINT    hk_region_vk;
} AppConfig;

static AppConfig g_cfg;
static wchar_t   g_exe_dir[MAX_PATH];

static void init_paths(void) {
    wchar_t tmp[MAX_PATH];
    GetModuleFileNameW(NULL, tmp, MAX_PATH);
    wchar_t *slash = wcsrchr(tmp, L'\\');
    if (slash) *slash = 0;
    wcsncpy(g_exe_dir, tmp, MAX_PATH - 1);
    g_exe_dir[MAX_PATH - 1] = 0;
}

/* ------------------------------------------------------------------ */
/* Hotkey Parsing Logic                                               */
/* ------------------------------------------------------------------ */
static UINT parse_vk(const wchar_t* key) {
    if (_wcsicmp(key, L"PrintScreen") == 0 || _wcsicmp(key, L"ImprEcran") == 0) return VK_SNAPSHOT;
    if (_wcsicmp(key, L"ScrollLock") == 0) return VK_SCROLL;
    if (_wcsicmp(key, L"Pause") == 0) return VK_PAUSE;
    
    if (key[0] == L'F' && key[1] != L'\0' && key[2] == L'\0') {
        int n = key[1] - L'0';
        if (n >= 1 && n <= 12) return VK_F1 + n - 1;
    }
    
    if (key[1] == L'\0') return (UINT)towupper(key[0]);
    return 0;
}

static BOOL parse_hotkey(const wchar_t* str, UINT* mod, UINT* vk) {
    *mod = 0; *vk = 0;
    if (!str || !*str) return FALSE;

    wchar_t buf[256];
    wcsncpy(buf, str, 255); buf[255] = 0;
    
    wchar_t* p = buf;
    wchar_t* token;
    
    while (1) {
        token = wcschr(p, L'+');
        if (token) *token = L'\0';
        
        // Trim spaces
        while (*p == L' ') p++;
        wchar_t* end = p + wcslen(p) - 1;
        while (end > p && *end == L' ') { *end = L'\0'; end--; }

        if (_wcsicmp(p, L"Ctrl") == 0) *mod |= MOD_CONTROL;
        else if (_wcsicmp(p, L"Shift") == 0) *mod |= MOD_SHIFT;
        else if (_wcsicmp(p, L"Alt") == 0) *mod |= MOD_ALT;
        else if (_wcsicmp(p, L"Win") == 0) *mod |= MOD_WIN;
        else {
            *vk = parse_vk(p);
        }
        
        if (!token) break;
        p = token + 1;
    }
    return (*vk != 0);
}

static void ensure_default_ini(void) {
    wchar_t ini_path[MAX_PATH];
    _snwprintf(ini_path, MAX_PATH, L"%s\\jxlshot.ini", g_exe_dir);
    if (GetFileAttributesW(ini_path) == INVALID_FILE_ATTRIBUTES) {
        FILE *f = _wfopen(ini_path, L"w");
        if (f) {
            fprintf(f, "[Capture]\n");
            fprintf(f, "; 1 for debug on, 0 for debug off\n");
            fprintf(f, "Debug=0\n");
            fprintf(f, "; 1 for lossless, 0 for lossy\n");
            fprintf(f, "Lossless=1\n");
            fprintf(f, "; Lossy distance (0.0 - 25.0, lower is better)\n");
            fprintf(f, "Distance=1.0\n");
            fprintf(f, "; Show mouse cursor in region capture (1=yes, 0=no)\n");
            fprintf(f, "ShowCursor=1\n");
            fprintf(f, "; Export path for screenshots (leave empty for default Pictures folder)\n");
            fprintf(f, "ExportPath=\n");
            fprintf(f, "; Hotkeys (Modifiers: Ctrl, Shift, Alt, Win. Keys: PrintScreen, F1-F12, etc.)\n");
            fprintf(f, "HotkeyFull=PrintScreen\n");
            fprintf(f, "HotkeyRegion=Ctrl+PrintScreen\n");
            fclose(f);
        }
    }
}

static void init_config(void) {
    wchar_t ini_path[MAX_PATH];
    _snwprintf(ini_path, MAX_PATH, L"%s\\jxlshot.ini", g_exe_dir);
    
    // Defaults
    g_cfg.debug_enabled = 1;
    g_cfg.lossless = 1;
    g_cfg.distance = 1.0f;
    g_cfg.show_cursor = 1;
    g_cfg.hk_full_mod = 0; g_cfg.hk_full_vk = VK_SNAPSHOT;
    g_cfg.hk_region_mod = MOD_CONTROL; g_cfg.hk_region_vk = VK_SNAPSHOT;

    // Default export path: Windows Pictures folder
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_MYPICTURES, NULL, SHGFP_TYPE_CURRENT, g_cfg.export_path))) {
        GetEnvironmentVariableW(L"USERPROFILE", g_cfg.export_path, MAX_PATH);
        wcscat_s(g_cfg.export_path, MAX_PATH, L"\\Pictures");
    }

    // Read INI
    g_cfg.debug_enabled = GetPrivateProfileIntW(L"Capture", L"Debug", 1, ini_path);
    g_cfg.lossless = GetPrivateProfileIntW(L"Capture", L"Lossless", 1, ini_path);
    g_cfg.show_cursor = GetPrivateProfileIntW(L"Capture", L"ShowCursor", 1, ini_path);
    
    wchar_t dist_str[64];
    GetPrivateProfileStringW(L"Capture", L"Distance", L"1.0", dist_str, 64, ini_path);
    g_cfg.distance = (float)wcstod(dist_str, NULL);
    if (g_cfg.distance < 0.0f) g_cfg.distance = 0.0f;
    if (g_cfg.distance > 25.0f) g_cfg.distance = 25.0f;

    wchar_t path_buf[MAX_PATH];
    GetPrivateProfileStringW(L"Capture", L"ExportPath", L"", path_buf, MAX_PATH, ini_path);
    if (path_buf[0] != L'\0') {
        wcsncpy(g_cfg.export_path, path_buf, MAX_PATH - 1);
        g_cfg.export_path[MAX_PATH - 1] = L'\0';
    }

    // Read Hotkeys
    wchar_t hk_full_str[128], hk_region_str[128];
    GetPrivateProfileStringW(L"Capture", L"HotkeyFull", L"PrintScreen", hk_full_str, 128, ini_path);
    GetPrivateProfileStringW(L"Capture", L"HotkeyRegion", L"Ctrl+PrintScreen", hk_region_str, 128, ini_path);
    
    parse_hotkey(hk_full_str, &g_cfg.hk_full_mod, &g_cfg.hk_full_vk);
    parse_hotkey(hk_region_str, &g_cfg.hk_region_mod, &g_cfg.hk_region_vk);
}

/* ------------------------------------------------------------------ */
/* Debug logging                                                      */
/* ------------------------------------------------------------------ */
static FILE *g_dbg = NULL;
static void dbg_init(void) {
    if (!g_cfg.debug_enabled) {
        g_dbg = NULL;
        return;
    }
    wchar_t temp_dir[MAX_PATH];
    GetTempPathW(MAX_PATH, temp_dir);
    wchar_t log_path[MAX_PATH];
    _snwprintf(log_path, MAX_PATH, L"%sjxlshot_debug.log", temp_dir);
    g_dbg = _wfopen(log_path, L"a");
    if (!g_dbg) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_dbg, "\n===== jxlshot run started %04d-%02d-%02d %02d:%02d:%02d =====\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fflush(g_dbg);
}

static void dbg(const char *fmt, ...) {
    if (!g_cfg.debug_enabled) return;
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
/* Output Paths                                                       */
/* ------------------------------------------------------------------ */
static void build_out_path(wchar_t *path, int n) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snwprintf(path, n, L"%s\\jxlshot_%04d%02d%02d_%02d%02d%02d_%03d.jxl",
               g_cfg.export_path, st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    path[n - 1] = 0;
}

/* ------------------------------------------------------------------ */
/* DPI awareness                                                      */
/* ------------------------------------------------------------------ */
static void set_dpi_aware(void) {
    typedef BOOL (WINAPI *Fn)(HANDLE);
    Fn f = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (f) f((HANDLE)(LONG_PTR)-4);
    else   SetProcessDPIAware();
}

/* ------------------------------------------------------------------ */
/* Screen capture                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    HBITMAP  hbmp;
    HDC      hdc;
    uint8_t *bits;
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
    bi.bmiHeader.biHeight      = -g->h;
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
            rgb[3*i + 0] = bgra[4*i + 2];
            rgb[3*i + 1] = bgra[4*i + 1];
            rgb[3*i + 2] = bgra[4*i + 0];
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
        dbg("save_bgra_as_jxl: fwrite %lu/%lu bytes to %ls",
            (unsigned long)written, (unsigned long)size, path);
        ok = (written == size);
        if (!ok) dbg("save_bgra_as_jxl: short write, disk full or I/O error?");
        fclose(f);
    } else {
        dbg("save_bgra_as_jxl: _wfopen(%ls) failed, errno=%d (%s)", path, errno, strerror(errno));
    }
    free(buf);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Entry points & main                                                */
/* ------------------------------------------------------------------ */
static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [-q] [-d distance] [-w ms]\n"
            "  -q            lossy encoding (default: lossless)\n"
            "  -d distance   lossy distance 0.0-25.0, lower = better (default 1.0, implies -q)\n"
            "  -w ms         wait N milliseconds before capturing\n"
            "\nConfiguration and export paths are managed via jxlshot.ini.\n",
            argv0);
}

#ifndef JXLSHOT_TRAY_BUILD
int main(int argc, char **argv);
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR szCmdLine, int sw) {
    return main(__argc, __argv);
}

int main(int argc, char **argv) {
    DWORD wait_ms = 0;
    int cli_lossless = -1;
    float cli_distance = -1.0f;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-q")) cli_lossless = 0;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
            cli_distance = (float)strtod(argv[++i], NULL);
            cli_lossless = 0;
        }
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) {
            wait_ms = (DWORD)strtol(argv[++i], NULL, 10);
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]); return 0;
        }
        else {
            fprintf(stderr, "error: unrecognized argument '%s'\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }
    set_dpi_aware();
    init_paths();
    ensure_default_ini();
    init_config();
    if (cli_lossless != -1) g_cfg.lossless = cli_lossless;
    if (cli_distance >= 0.0f) g_cfg.distance = cli_distance;
    dbg_init();
    dbg("main: lossless=%d distance=%.3f wait_ms=%lu", g_cfg.lossless, g_cfg.distance, (unsigned long)wait_ms);
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
    if (save_bgra_as_jxl(g.bits, g.w, g.h, g_cfg.lossless, g_cfg.distance, out_path)) {
        fwprintf(stdout, L"saved %ls (%dx%d, %ls)\n", out_path, g.w, g.h, g_cfg.lossless ? L"lossless" : L"lossy");
        rc = 0;
    } else {
        fwprintf(stderr, L"error: encoding or saving failed (see jxlshot_debug.log for details)\n");
    }
    free_grab(&g);
    return rc;
}
#endif /* JXLSHOT_TRAY_BUILD */
