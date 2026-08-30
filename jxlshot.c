/* jxlshot.c
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
*
* jxlshot.c — minimal command-line screenshot tool for Windows.
*
* Build (MSYS2 / MinGW-w64) - Optimized for size:
*   gcc -Os -s -flto -ffunction-sections -fdata-sections -Wl,--gc-sections \
*       -mwindows -o jxlshot.exe jxlshot.c -ljxl -lgdi32 -luser32 -lshell32 -lole32
*/



/* jxlshot.c
 * Captures the primary monitor and saves it as JPEG XL (.jxl).
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
#include "jxlshot_hdr.h"

/* ------------------------------------------------------------------ */
/* Forward Declarations                                               */
/* ------------------------------------------------------------------ */
static void set_dpi_aware(void);
static void init_paths(void);
static void ensure_default_ini(void);
static void init_config(void);
static void dbg_init(void);
static void build_out_path(wchar_t *path, int n);
static int save_bgra_as_jxl(const uint8_t *bgra, int w, int h, int lossless, float distance, const wchar_t *path);

/* ------------------------------------------------------------------ */
/* Configuration (INI)                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    int     debug_enabled;
    int     lossless;
    float   distance;
    int     show_cursor;
    int     force_sdr;
    int     hdr_enabled;
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

static UINT parse_vk(const wchar_t* key) {
    if (!key || !*key) return 0;
    if (_wcsicmp(key, L"PrintScreen") == 0 || _wcsicmp(key, L"ImprEcran") == 0 || _wcsicmp(key, L"PrtScn") == 0) return VK_SNAPSHOT;
    if (_wcsicmp(key, L"ScrollLock") == 0) return VK_SCROLL;
    if (_wcsicmp(key, L"Pause") == 0 || _wcsicmp(key, L"Break") == 0) return VK_PAUSE;
    if (_wcsicmp(key, L"CapsLock") == 0) return VK_CAPITAL;
    if (_wcsicmp(key, L"NumLock") == 0) return VK_NUMLOCK;
    if (_wcsicmp(key, L"Space") == 0 || _wcsicmp(key, L"Spacebar") == 0) return VK_SPACE;
    if (_wcsicmp(key, L"Escape") == 0 || _wcsicmp(key, L"Esc") == 0) return VK_ESCAPE;
    if (_wcsicmp(key, L"Enter") == 0 || _wcsicmp(key, L"Return") == 0) return VK_RETURN;
    if (_wcsicmp(key, L"Tab") == 0) return VK_TAB;
    if (_wcsicmp(key, L"Backspace") == 0 || _wcsicmp(key, L"Back") == 0) return VK_BACK;
    if (_wcsicmp(key, L"Insert") == 0 || _wcsicmp(key, L"Ins") == 0) return VK_INSERT;
    if (_wcsicmp(key, L"Delete") == 0 || _wcsicmp(key, L"Del") == 0) return VK_DELETE;
    if (_wcsicmp(key, L"Home") == 0) return VK_HOME;
    if (_wcsicmp(key, L"End") == 0) return VK_END;
    if (_wcsicmp(key, L"PageUp") == 0 || _wcsicmp(key, L"PgUp") == 0) return VK_PRIOR;
    if (_wcsicmp(key, L"PageDown") == 0 || _wcsicmp(key, L"PgDn") == 0) return VK_NEXT;
    if (_wcsicmp(key, L"Up") == 0) return VK_UP;
    if (_wcsicmp(key, L"Down") == 0) return VK_DOWN;
    if (_wcsicmp(key, L"Left") == 0) return VK_LEFT;
    if (_wcsicmp(key, L"Right") == 0) return VK_RIGHT;
    if (towupper(key[0]) == L'F') {
        int n = _wtoi(key + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (key[1] == L'\0') return (UINT)towupper(key[0]);
    if (_wcsicmp(key, L"NumPad0") == 0) return VK_NUMPAD0;
    if (_wcsicmp(key, L"NumPad1") == 0) return VK_NUMPAD1;
    if (_wcsicmp(key, L"NumPad2") == 0) return VK_NUMPAD2;
    if (_wcsicmp(key, L"NumPad3") == 0) return VK_NUMPAD3;
    if (_wcsicmp(key, L"NumPad4") == 0) return VK_NUMPAD4;
    if (_wcsicmp(key, L"NumPad5") == 0) return VK_NUMPAD5;
    if (_wcsicmp(key, L"NumPad6") == 0) return VK_NUMPAD6;
    if (_wcsicmp(key, L"NumPad7") == 0) return VK_NUMPAD7;
    if (_wcsicmp(key, L"NumPad8") == 0) return VK_NUMPAD8;
    if (_wcsicmp(key, L"NumPad9") == 0) return VK_NUMPAD9;
    if (_wcsicmp(key, L"Multiply") == 0) return VK_MULTIPLY;
    if (_wcsicmp(key, L"Add") == 0) return VK_ADD;
    if (_wcsicmp(key, L"Subtract") == 0) return VK_SUBTRACT;
    if (_wcsicmp(key, L"Decimal") == 0) return VK_DECIMAL;
    if (_wcsicmp(key, L"Divide") == 0) return VK_DIVIDE;
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
        while (*p == L' ') p++;
        wchar_t* end = p + wcslen(p) - 1;
        while (end > p && *end == L' ') { *end = L'\0'; end--; }
        if (_wcsicmp(p, L"Ctrl") == 0) *mod |= MOD_CONTROL;
        else if (_wcsicmp(p, L"Shift") == 0) *mod |= MOD_SHIFT;
        else if (_wcsicmp(p, L"Alt") == 0) *mod |= MOD_ALT;
        else if (_wcsicmp(p, L"Win") == 0) *mod |= MOD_WIN;
        else *vk = parse_vk(p);
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
            fprintf(f, "[Capture]\nDebug=0\nLossless=1\nDistance=1.0\nShowCursor=1\nExportPath=\n");
            fprintf(f, "HotkeyFull=PrintScreen\nHotkeyRegion=Ctrl+PrintScreen\n");
            fprintf(f, "HDREnabled=1\nForceSDR=0\n");
            fclose(f);
        }
    }
}

static void init_config(void) {
    wchar_t ini_path[MAX_PATH];
    _snwprintf(ini_path, MAX_PATH, L"%s\\jxlshot.ini", g_exe_dir);
    g_cfg.debug_enabled = 1; g_cfg.lossless = 1; g_cfg.distance = 1.0f; g_cfg.show_cursor = 1;
    g_cfg.hdr_enabled = 1; g_cfg.force_sdr = 0;
    g_cfg.hk_full_mod = 0; g_cfg.hk_full_vk = VK_SNAPSHOT;
    g_cfg.hk_region_mod = MOD_CONTROL; g_cfg.hk_region_vk = VK_SNAPSHOT;
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_MYPICTURES, NULL, SHGFP_TYPE_CURRENT, g_cfg.export_path))) {
        GetEnvironmentVariableW(L"USERPROFILE", g_cfg.export_path, MAX_PATH);
        wcscat_s(g_cfg.export_path, MAX_PATH, L"\\Pictures");
    }
    g_cfg.debug_enabled = GetPrivateProfileIntW(L"Capture", L"Debug", 1, ini_path);
    g_cfg.lossless = GetPrivateProfileIntW(L"Capture", L"Lossless", 1, ini_path);
    g_cfg.show_cursor = GetPrivateProfileIntW(L"Capture", L"ShowCursor", 1, ini_path);
    g_cfg.hdr_enabled = GetPrivateProfileIntW(L"Capture", L"HDREnabled", 1, ini_path);
    g_cfg.force_sdr   = GetPrivateProfileIntW(L"Capture", L"ForceSDR", 0, ini_path);
    
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

    wchar_t hk_full_str[128], hk_region_str[128];
    GetPrivateProfileStringW(L"Capture", L"HotkeyFull", L"PrintScreen", hk_full_str, 128, ini_path);
    GetPrivateProfileStringW(L"Capture", L"HotkeyRegion", L"Ctrl+PrintScreen", hk_region_str, 128, ini_path);
    parse_hotkey(hk_full_str, &g_cfg.hk_full_mod, &g_cfg.hk_full_vk);
    parse_hotkey(hk_region_str, &g_cfg.hk_region_mod, &g_cfg.hk_region_vk);
}

/* ------------------------------------------------------------------ */
/* Unified Debug logging                                              */
/* ------------------------------------------------------------------ */
static FILE *g_dbg = NULL;
static void dbg_init(void) {
    if (!g_cfg.debug_enabled) { g_dbg = NULL; return; }
    wchar_t temp_dir[MAX_PATH], log_path[MAX_PATH];
    GetTempPathW(MAX_PATH, temp_dir);
    _snwprintf(log_path, MAX_PATH, L"%sjxlshot_debug.log", temp_dir);
    g_dbg = _wfopen(log_path, L"a");
    if (!g_dbg) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_dbg, "\n===== jxlshot run started %04d-%02d-%02d %02d:%02d:%02d =====\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fflush(g_dbg);
}
static void dbg(const char *fmt, ...) {
    if (!g_cfg.debug_enabled || !g_dbg) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = 0;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_dbg, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    fflush(g_dbg);
}

/* ------------------------------------------------------------------ */
/* Output Paths & DPI awareness                                       */
/* ------------------------------------------------------------------ */
static void set_dpi_aware(void) {
    typedef BOOL (WINAPI *Fn)(HANDLE);
    Fn f = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (f) f((HANDLE)(LONG_PTR)-4);
    else SetProcessDPIAware();
}

static void build_out_path(wchar_t *path, int n) {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t safe_dir[MAX_PATH];
    wcsncpy(safe_dir, g_cfg.export_path, MAX_PATH - 1);
    safe_dir[MAX_PATH - 1] = L'\0';
    size_t len = wcslen(safe_dir);
    if (len > 0 && safe_dir[len - 1] != L'\\') wcsncat(safe_dir, L"\\", MAX_PATH - len - 1);
    _snwprintf(path, n, L"%sjxlshot_%04d%02d%02d_%02d%02d%02d_%03d.jxl",
               safe_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    path[n - 1] = L'\0';
}

/* ------------------------------------------------------------------ */
/* JPEG XL encoding (SDR)                                             */
/* ------------------------------------------------------------------ */
static int encode_jxl(const uint8_t *bgra, int w, int h, int lossless, float distance, uint8_t **out_buf, size_t *out_size) {
    int ok = 0; uint8_t *rgb = NULL, *buf = NULL; JxlEncoderStatus st;
    JxlEncoder *enc = JxlEncoderCreate(NULL);
    if (!enc) return 0;

    JxlBasicInfo info; JxlEncoderInitBasicInfo(&info);
    info.xsize = w; info.ysize = h; info.bits_per_sample = 8;
    info.exponent_bits_per_sample = 0; info.num_color_channels = 3;
    info.alpha_bits = 0; info.uses_original_profile = lossless ? JXL_TRUE : JXL_FALSE;
    if (JxlEncoderSetBasicInfo(enc, &info) != JXL_ENC_SUCCESS) goto done;
    
    JxlColorEncoding ce; JxlColorEncodingSetToSRGB(&ce, JXL_FALSE);
    if (JxlEncoderSetColorEncoding(enc, &ce) != JXL_ENC_SUCCESS) goto done;

    JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    if (!fs) goto done;
    if (lossless) { JxlEncoderSetFrameLossless(fs, JXL_TRUE); JxlEncoderSetFrameDistance(fs, 0.0f); }
    else { JxlEncoderSetFrameDistance(fs, distance); }

    size_t npix = (size_t)w * h;
    rgb = (uint8_t *)malloc(npix * 3);
    if (!rgb) goto done;
    
    const uint32_t *src = (const uint32_t *)bgra;
    uint8_t *dst = rgb;
    for (size_t i = 0; i < npix; i++) {
        uint32_t pixel = src[i];
        dst[0] = (pixel >> 16) & 0xFF; dst[1] = (pixel >> 8) & 0xFF; dst[2] = pixel & 0xFF;
        dst += 3;
    }
    
    JxlPixelFormat fmt = {3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    if (JxlEncoderAddImageFrame(fs, &fmt, rgb, npix * 3) != JXL_ENC_SUCCESS) goto done;
    JxlEncoderCloseInput(enc);
    
    size_t cap = (size_t)w * h; if (cap < (4 << 20)) cap = (4 << 20);
    buf = (uint8_t *)malloc(cap);
    if (!buf) goto done;
    
    uint8_t *next = buf; size_t avail = cap;
    for (;;) {
        st = JxlEncoderProcessOutput(enc, &next, &avail);
        if (st == JXL_ENC_SUCCESS) break;
        if (st == JXL_ENC_NEED_MORE_OUTPUT) {
            size_t used = (size_t)(next - buf); cap *= 2;
            uint8_t *nb = (uint8_t *)realloc(buf, cap);
            if (!nb) { free(buf); buf = NULL; goto done; }
            buf = nb; next = buf + used; avail = cap - used; continue;
        }
        free(buf); buf = NULL; goto done;
    }
    *out_buf = buf; *out_size = cap - avail; buf = NULL; ok = 1;
done:
    free(rgb); free(buf); JxlEncoderDestroy(enc);
    return ok;
}

static int save_bgra_as_jxl(const uint8_t *bgra, int w, int h, int lossless, float distance, const wchar_t *path) {
    uint8_t *buf = NULL; size_t size = 0;
    if (!encode_jxl(bgra, w, h, lossless, distance, &buf, &size)) return 0;
    int ok = 0; FILE *f = _wfopen(path, L"wb");
    if (f) {
        size_t written = fwrite(buf, 1, size, f);
        ok = (written == size); fclose(f);
    }
    free(buf); return ok;
}

/* ------------------------------------------------------------------ */
/* Unified Screen Capture (HDR with SDR Fallback)                     */
/* ------------------------------------------------------------------ */
typedef enum { CAPTURE_SDR, CAPTURE_HDR } CaptureMode;

typedef struct {
    CaptureMode mode;
    HBITMAP hbmp;
    HDC hdc;
    uint8_t *bits;
    int w, h;
    hdr_frame_t hdr_frame;
} CaptureResult;

static int capture_screen(CaptureResult *cr) {
    ZeroMemory(cr, sizeof *cr);
    if (g_cfg.hdr_enabled && !g_cfg.force_sdr) {
        if (hdr_capture_primary(&cr->hdr_frame, 1000)) {
            cr->mode = CAPTURE_HDR;
            cr->w = cr->hdr_frame.w;
            cr->h = cr->hdr_frame.h;
            dbg("capture_screen: HDR capture successful (%dx%d)", cr->w, cr->h);
            return 1;
        }
        dbg("capture_screen: HDR not available or failed, falling back to SDR.");
    }
    
    cr->mode = CAPTURE_SDR;
    cr->w = GetSystemMetrics(SM_CXSCREEN); 
    cr->h = GetSystemMetrics(SM_CYSCREEN);
    if (cr->w <= 0 || cr->h <= 0) return 0;

    HDC sdc = GetDC(NULL);
    if (!sdc) return 0;
    cr->hdc = CreateCompatibleDC(sdc);
    if (!cr->hdc) { ReleaseDC(NULL, sdc); return 0; }

    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = cr->w; bi.bmiHeader.biHeight = -cr->h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    
    cr->hbmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&cr->bits, NULL, 0);
    if (!cr->hbmp) { DeleteDC(cr->hdc); ReleaseDC(NULL, sdc); return 0; }

    SelectObject(cr->hdc, cr->hbmp);
    if (!BitBlt(cr->hdc, 0, 0, cr->w, cr->h, sdc, 0, 0, SRCCOPY)) {
        ReleaseDC(NULL, sdc); return 0;
    }
    GdiFlush(); ReleaseDC(NULL, sdc);
    dbg("capture_screen: SDR capture successful (%dx%d)", cr->w, cr->h);
    return 1;
}

static void free_capture(CaptureResult *cr) {
    if (cr->mode == CAPTURE_HDR) {
        hdr_frame_free(&cr->hdr_frame);
    } else {
        if (cr->hdc) DeleteDC(cr->hdc);
        if (cr->hbmp) DeleteObject(cr->hbmp);
    }
    ZeroMemory(cr, sizeof *cr);
}

static int save_capture_as_jxl(const CaptureResult *cr, const wchar_t *path) {
    if (cr->mode == CAPTURE_HDR) {
        dbg("save_capture_as_jxl: Encoding true HDR JXL.");
        return hdr_encode_jxl_hdr(cr->hdr_frame.rgb, cr->w, cr->h, g_cfg.lossless, g_cfg.distance, path);
    } else {
        dbg("save_capture_as_jxl: Encoding SDR JXL.");
        return save_bgra_as_jxl(cr->bits, cr->w, cr->h, g_cfg.lossless, g_cfg.distance, path);
    }
}

/* ------------------------------------------------------------------ */
/* Entry points & main                                                */
/* ------------------------------------------------------------------ */
#ifndef JXLSHOT_TRAY_BUILD
int main(int argc, char **argv);
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR szCmdLine, int sw) { return main(__argc, __argv); }

int main(int argc, char **argv) {
    DWORD wait_ms = 0; int cli_lossless = -1; float cli_distance = -1.0f;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q")) cli_lossless = 0;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) { cli_distance = (float)strtod(argv[++i], NULL); cli_lossless = 0; }
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) { wait_ms = (DWORD)strtol(argv[++i], NULL, 10); }
    }
    set_dpi_aware(); init_paths(); ensure_default_ini(); init_config();
    if (cli_lossless != -1) g_cfg.lossless = cli_lossless;
    if (cli_distance >= 0.0f) g_cfg.distance = cli_distance;
    dbg_init();
    if (wait_ms) Sleep(wait_ms);

    CaptureResult cr;
    ZeroMemory(&cr, sizeof(cr));
    if (!capture_screen(&cr)) {
        fwprintf(stderr, L"Screen capture failed.\n");
        return 1;
    }

    wchar_t out_path[MAX_PATH];
    build_out_path(out_path, MAX_PATH);

    if (!save_capture_as_jxl(&cr, out_path)) {
        fwprintf(stderr, L"Encoding or saving failed.\n");
        free_capture(&cr);
        return 1;
    }

    fwprintf(stdout, L"Screenshot saved to: %s\n", out_path);
    free_capture(&cr);
    return 0;
}
#endif