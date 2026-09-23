/*
* Captures the primary monitor and saves it as JPEG XL (.jxl).
*
* Usage:
*   jxlshot.exe                capture, lossless (default)
*   jxlshot.exe -q             lossy capture, default distance 1.0
*   jxlshot.exe -q -d 3.0      lossy capture, distance 3.0 (lower = better)
*   jxlshot.exe -w 3000        wait 3000 ms before capturing
*  
*
*
*  THE PICTURE EXPORTING FOLLOW THE EXPORT PATH !!!!!!!
*
*
*
*
*

* Configuration is read from jxlshot.ini located next to the executable.
* Debug logs are written to %TMP%\jxlshot_debug.log
*
* jxlshot.c — minimal command-line screenshot tool for Windows.
*
*/


#define INITGUID
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
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <locale.h>
#include <objbase.h>
#include <jxl/thread_parallel_runner.h>



// Forward Declarations


static void set_dpi_aware(void);
static void init_paths(void);
static void ensure_default_ini(void);
static void init_config(void);
static void dbg_init(void);
static void build_out_path(wchar_t *path, int n, int is_hdr);
static int save_rgb_as_jxl(const uint8_t *rgb, int w, int h, int is_hdr, int lossless, float distance, const wchar_t *path);


// Configuration (INI)


typedef struct {
    int     debug_enabled;
    int     lossless;
    float   distance;
    int     show_cursor;
    int blank_check_mode; /* 1=16 samples (4x4), 2=256 samples (16x16), 3=ALL pixels */
    wchar_t export_path[MAX_PATH];
    UINT    hk_full_mod;
    UINT    hk_full_vk;
    UINT    hk_region_mod;
    UINT    hk_region_vk;
} AppConfig;

static AppConfig g_cfg;
static wchar_t   g_exe_dir[MAX_PATH];

static void init_paths(void) {
    wchar_t tmp[32768]; // Support Windows long paths
    DWORD len = GetModuleFileNameW(NULL, tmp, 32768);
    if (len == 0 || len == 32768) {
        // Fallback to current directory if path is too long or fails
        wcscpy(g_exe_dir, L".");
        return;
    }
    wchar_t *slash = wcsrchr(tmp, L'\\');
    if (slash) *slash = 0;
    wcsncpy(g_exe_dir, tmp, MAX_PATH - 1);
    g_exe_dir[MAX_PATH - 1] = 0;
}


// Hotkey Parsing Logic


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

    if (key[1] == L'\0') {
        return (UINT)towupper(key[0]);
    }

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
        // Use binary write to explicitly control the encoding
        FILE *f = _wfopen(ini_path, L"wb");
        if (f) {
            // 1. Write UTF-16 LE Byte Order Mark (BOM)
            unsigned short bom = 0xFEFF;
            fwrite(&bom, sizeof(bom), 1, f);
            
            // 2. Write the configuration in UTF-16 LE
            const wchar_t *default_ini = 
                L"; ==============================================================================\n"
                L"; JXLShot Configuration File\n"
                L"; ==============================================================================\n"
                L";\n"
                L"; [JPEG XL Distance & Quality Guide]\n"
                L";   0.0 : Mathematically lossless compression.\n"
                L";   1.0 : Visually lossless quality. (Recommended default)\n"
                L";   2.0+: Higher compression, visible quality loss.\n"
                L";\n"
                L"; [General Settings Reference]\n"
                L";   Debug          : 1 = enable debug logging; 0 = disable.\n"
                L";   Lossless       : 1 = Force mathematically lossless mode.\n"
                L";   ShowCursor     : 1 = Include mouse cursor; 0 = Hide it.\n"
                L";   ExportPath     : Custom directory for saving screenshots.\n"
                L";   HotkeyFull     : Keyboard shortcut to capture entire screen.\n"
                L";   HotkeyRegion   : Keyboard shortcut to capture specific region.\n"
                L";   BlankCheckMode : 0 = Disabled, 1 = Basic, 2 = Advanced, 3 = HARDCORE.\n"
                L";\n"
                L"; ==============================================================================\n"
                L"\n"
                L"[Capture]\n"
                L"Debug=0\n"
                L"Lossless=1\n"
                L"Distance=1.0\n"
                L"ExportPath=\n"
                L"HotkeyFull=PrintScreen\n"
                L"HotkeyRegion=Ctrl+PrintScreen\n"
                L"BlankCheckMode=2\n"
                L"ShowCursor=1\n"
                L"     THE CURSOR FEATURE IS BROKEN DONT TOUCH IT PLEASE !! \n";
            
            fputws(default_ini, f);
            fclose(f);
        }
    }
}

// Robust INI Parsing with Backward Compatibility Fallbacks */

static int get_cfg_int(LPCWSTR key, int default_val, LPCWSTR ini_path) {
    wchar_t buf[64];
    // 1. Try primary section
    if (GetPrivateProfileStringW(L"Capture", key, L"", buf, 64, ini_path) > 0) return _wtoi(buf);
    // 2. Try legacy/alternative sections
    if (GetPrivateProfileStringW(L"Settings", key, L"", buf, 64, ini_path) > 0) return _wtoi(buf);
    if (GetPrivateProfileStringW(L"General", key, L"", buf, 64, ini_path) > 0) return _wtoi(buf);
    
    // Remove the NULL fallback. Passing NULL returns section names, not key values.
    return default_val;
}

static float get_cfg_float(LPCWSTR key, float default_val, LPCWSTR ini_path) {
    wchar_t buf[64];
    // Helper lambda-style block to parse float safely with C locale
    #define PARSE_FLOAT(buf) \
        do { \
            _locale_t c_locale = _create_locale(LC_NUMERIC, "C"); \
            float val = (float)_wcstod_l(buf, NULL, c_locale); \
            _free_locale(c_locale); \
            return val; \
        } while(0)

    if (GetPrivateProfileStringW(L"Capture", key, L"", buf, 64, ini_path) > 0) PARSE_FLOAT(buf);
    if (GetPrivateProfileStringW(L"Settings", key, L"", buf, 64, ini_path) > 0) PARSE_FLOAT(buf);
    if (GetPrivateProfileStringW(L"General", key, L"", buf, 64, ini_path) > 0) PARSE_FLOAT(buf); // Added for consistency
    
    #undef PARSE_FLOAT
    
    // Remove the NULL fallback.
    return default_val;
}

static void get_cfg_string(LPCWSTR key, LPCWSTR default_val, LPWSTR out_buf, DWORD buf_size, LPCWSTR ini_path) {
    // 1. Try primary section
    if (GetPrivateProfileStringW(L"Capture", key, L"", out_buf, buf_size, ini_path) > 0) return;
    // 2. Try legacy sections
    if (GetPrivateProfileStringW(L"Settings", key, L"", out_buf, buf_size, ini_path) > 0) return;
    if (GetPrivateProfileStringW(L"General", key, L"", out_buf, buf_size, ini_path) > 0) return;
    
    // Remove the NULL fallback.
    
    // 3. Fallback to default
    wcsncpy_s(out_buf, buf_size, default_val, _TRUNCATE);
}

static void init_config(void) {
    wchar_t ini_path[MAX_PATH];
    _snwprintf(ini_path, MAX_PATH, L"%s\\jxlshot.ini", g_exe_dir);

    // Force Windows to drop its cached copy of this INI and re-read from disk.
    // Without this, GetPrivateProfileString* can keep serving a stale in-memory
    // snapshot after the file is edited externally, which is why "Reload
    // Configuration" will do nothing until the app was fully restarted.
    WritePrivateProfileStringW(NULL, NULL, NULL, ini_path);

    // Set absolute defaults first
    g_cfg.debug_enabled = 1;
    g_cfg.lossless = 1; 
    g_cfg.distance = 1.0f; 
    g_cfg.show_cursor = 1;
    g_cfg.blank_check_mode = 2;
    g_cfg.hk_full_mod = 0; 
    g_cfg.hk_full_vk = VK_SNAPSHOT;
    g_cfg.hk_region_mod = MOD_CONTROL; 
    g_cfg.hk_region_vk = VK_SNAPSHOT;
    
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_MYPICTURES, NULL, SHGFP_TYPE_CURRENT, g_cfg.export_path))) {
        GetEnvironmentVariableW(L"USERPROFILE", g_cfg.export_path, MAX_PATH);
        wcscat_s(g_cfg.export_path, MAX_PATH, L"\\Pictures");
    }

    // Use robust fallback getters instead of direct GetPrivateProfile* calls
    g_cfg.debug_enabled = get_cfg_int(L"Debug", 1, ini_path);
    g_cfg.lossless = get_cfg_int(L"Lossless", 1, ini_path);
    g_cfg.show_cursor = get_cfg_int(L"ShowCursor", 1, ini_path);
    g_cfg.blank_check_mode = get_cfg_int(L"BlankCheckMode", 2, ini_path);
    
    if (g_cfg.blank_check_mode < 0) g_cfg.blank_check_mode = 0;
    if (g_cfg.blank_check_mode > 3) g_cfg.blank_check_mode = 3;

    g_cfg.distance = get_cfg_float(L"Distance", 1.0f, ini_path);
    if (g_cfg.distance < 0.0f) g_cfg.distance = 0.0f;
    if (g_cfg.distance > 25.0f) g_cfg.distance = 25.0f;

    wchar_t path_buf[MAX_PATH];
    get_cfg_string(L"ExportPath", L"", path_buf, MAX_PATH, ini_path);
    if (path_buf[0] != L'\0') {
        wcsncpy_s(g_cfg.export_path, MAX_PATH, path_buf, _TRUNCATE);
    }

    wchar_t hk_full_str[128], hk_region_str[128];
    get_cfg_string(L"HotkeyFull", L"PrintScreen", hk_full_str, 128, ini_path);
    get_cfg_string(L"HotkeyRegion", L"Ctrl+PrintScreen", hk_region_str, 128, ini_path);
    
    parse_hotkey(hk_full_str, &g_cfg.hk_full_mod, &g_cfg.hk_full_vk);
    parse_hotkey(hk_region_str, &g_cfg.hk_region_mod, &g_cfg.hk_region_vk);
}


// Unified Debug logging


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


// Output Paths & DPI awareness


static void set_dpi_aware(void) {
    typedef BOOL (WINAPI *Fn)(HANDLE);
    Fn f = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (f) {
        f((HANDLE)(LONG_PTR)-4); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    } else {
        SetProcessDPIAware();
    }
}

static void build_out_path(wchar_t *path, int n, int is_hdr) {
    SYSTEMTIME st; 
    GetLocalTime(&st);
    
    wchar_t safe_dir[MAX_PATH];
    wcsncpy(safe_dir, g_cfg.export_path, MAX_PATH - 1);
    safe_dir[MAX_PATH - 1] = L'\0';
    
    size_t len = wcslen(safe_dir);
    if (len > 0 && safe_dir[len - 1] != L'\\') {
        wcsncat(safe_dir, L"\\", MAX_PATH - len - 1);
    }
    
    // Dynamically append _hdr if the capture is HDR
    if (is_hdr) {
        _snwprintf(path, n, L"%sjxlshot_%04d%02d%02d_%02d%02d%02d_%03d_hdr.jxl",
                   safe_dir, st.wYear, st.wMonth, st.wDay, 
                   st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    } else {
        _snwprintf(path, n, L"%sjxlshot_%04d%02d%02d_%02d%02d%02d_%03d.jxl",
                   safe_dir, st.wYear, st.wMonth, st.wDay, 
                   st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    }
    path[n - 1] = L'\0';
}


// Screen capture (DXGI Desktop Duplication for native SDR/HDR)


typedef struct {
    uint8_t *bits;
    size_t size;
    int w, h;
    int is_hdr; // 1 if FP16 scRGB, 0 if 8-bit SDR
} Grab;


// Blank-frame detection



/*  THIS FUNTION MAY BE STUPID BUT ITS BETTER TO FIX THE STUPID BLANK SCREENSHOT    */
//  ON SOME STUPID HARDWARE
/*
 * Checks if a frame is completely blank (all black).
 * 
 * sample_mode:
 *   1 = 16 samples (4x4 grid) - Fastest, may miss tiny non-black artifacts.
 *   2 = 256 samples (16x16 grid) - Balanced, default behavior.
 *   3 = ALL pixels (exhaustive scan) - Slowest, absolute certainty.
 *
 * A frame is deemed blank ONLY when every checked pixel is exactly black.
 * If even one checked pixel carries any colour information, the frame is
 * considered valid (returns 0).
 */
static int is_frame_blank(const uint8_t *rgb, int w, int h, int is_hdr, int sample_mode) {
    // Mode 0: Disabled. Assume the frame is valid and skip checking.
    if (sample_mode == 0) return 0; 

    if (!rgb || w <= 0 || h <= 0) return 1;

    size_t bpp = is_hdr ? 6 : 3;

    if (sample_mode == 3) {
        /* Mode 3: Exhaustive scan of every single pixel */
        size_t total_pixels = (size_t)w * h;
        for (size_t i = 0; i < total_pixels; i++) {
            const uint8_t *px = rgb + (i * bpp);
            if (is_hdr) {
                if (px[0] | px[1] | px[2] | px[3] | px[4] | px[5]) return 0;
            } else {
                if (px[0] | px[1] | px[2]) return 0;
            }
        }
    } else {
        /* Mode 1 or 2: Grid sampling (4x4 or 16x16) */
        int cols = (sample_mode == 1) ? 4 : 16;
        int rows = (sample_mode == 1) ? 4 : 16;

        for (int r = 0; r < rows; r++) {
            int y = (h * r) / rows;
            for (int c = 0; c < cols; c++) {
                int x = (w * c) / cols;
                const uint8_t *px = rgb + ((size_t)y * w + x) * bpp;

                if (is_hdr) {
                    if (px[0] | px[1] | px[2] | px[3] | px[4] | px[5]) return 0;
                } else {
                    if (px[0] | px[1] | px[2]) return 0;
                }
            }
        }
    }

    // Every checked pixel was exactly zero — frame is blank
    return 1;
}


// GDI BitBlt fallback capture (works without DXGI Desktop Duplication)


/*
 * Classic BitBlt screen capture. Always SDR/8-bit, but far more
 * broadly compatible than DXGI Desktop Duplication — it doesn't
 * depend on DWM, doesn't care about weak/legacy GPU drivers, and
 * works even when Desktop Duplication silently returns black frames.
 * Used as a fallback when the DXGI path fails or produces a blank
 * frame after retrying.
 */
static int grab_via_gdi(Grab *g, HMONITOR target_monitor) {
    ZeroMemory(g, sizeof *g);

    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(target_monitor, &mi)) {
        dbg("gdi: GetMonitorInfo FAILED");
        return 0;
    }

    int x = mi.rcMonitor.left;
    int y = mi.rcMonitor.top;
    int w = mi.rcMonitor.right - mi.rcMonitor.left;
    int h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (w <= 0 || h <= 0) { dbg("gdi: invalid monitor rect"); return 0; }

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) { dbg("gdi: GetDC FAILED"); return 0; }

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) { dbg("gdi: CreateCompatibleDC FAILED"); ReleaseDC(NULL, hdcScreen); return 0; }

    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *dibBits = NULL;
    HBITMAP hbm = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, &dibBits, NULL, 0);
    if (!hbm || !dibBits) {
        dbg("gdi: CreateDIBSection FAILED");
        DeleteDC(hdcMem); ReleaseDC(NULL, hdcScreen);
        return 0;
    }

    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbm);

    /* CAPTUREBLT pulls in layered/UI-composited windows too, not just
     * the raw framebuffer, which matters on some setups. */
    BOOL blt_ok = BitBlt(hdcMem, 0, 0, w, h, hdcScreen, x, y, SRCCOPY | CAPTUREBLT);

    SelectObject(hdcMem, hbmOld);
    ReleaseDC(NULL, hdcScreen);

    if (!blt_ok) {
        dbg("gdi: BitBlt FAILED");
        DeleteObject(hbm); DeleteDC(hdcMem);
        return 0;
    }

    /* Convert BGRA (DIB) -> tightly packed RGB, matching the SDR
     * layout the rest of the pipeline (encode_jxl_identity) expects. */
    g->w = w; g->h = h; g->is_hdr = 0;
    g->size = (size_t)w * h * 3;
    g->bits = (uint8_t *)malloc(g->size);
    if (!g->bits) {
        dbg("gdi: malloc FAILED");
        DeleteObject(hbm); DeleteDC(hdcMem);
        return 0;
    }

    const uint8_t *src = (const uint8_t *)dibBits;
    uint8_t *dst = g->bits;
    for (int py = 0; py < h; py++) {
        const uint8_t *srow = src + (size_t)py * w * 4;
        for (int px = 0; px < w; px++) {
            dst[0] = srow[2]; /* R */
            dst[1] = srow[1]; /* G */
            dst[2] = srow[0]; /* B */
            dst += 3;
            srow += 4;
        }
    }

    DeleteObject(hbm);
    DeleteDC(hdcMem);

    dbg("gdi: capture ok w=%d h=%d", w, h);
    return 1;
}


// DXGI Desktop Duplication capture, with retry + blank detection


static int grab_via_dxgi(Grab *g, HMONITOR target_monitor) {
    ZeroMemory(g, sizeof *g);

    ID3D11Device *device = NULL;
    ID3D11DeviceContext *ctx = NULL;
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL;
    IDXGIOutput *output = NULL;
    IDXGIOutput1 *output1 = NULL;
    IDXGIOutput5 *output5 = NULL;
    IDXGIOutputDuplication *dupl = NULL;
    IDXGIResource *resource = NULL;
    ID3D11Texture2D *tex = NULL;
    ID3D11Texture2D *staging = NULL;
    int ok = 0;

    dbg("dxgi: start");

    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&factory))) {
        dbg("dxgi: CreateDXGIFactory1 FAILED"); goto cleanup;
    }

    for (UINT ai = 0; !adapter; ai++) {
        IDXGIAdapter1 *cand_adapter = NULL;
        HRESULT hr = factory->lpVtbl->EnumAdapters1(factory, ai, &cand_adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (!cand_adapter) continue;

        for (UINT oi = 0; ; oi++) {
            IDXGIOutput *cand_output = NULL;
            if (cand_adapter->lpVtbl->EnumOutputs(cand_adapter, oi, &cand_output) == DXGI_ERROR_NOT_FOUND) break;
            if (!cand_output) continue;

            DXGI_OUTPUT_DESC out_desc;
            if (SUCCEEDED(cand_output->lpVtbl->GetDesc(cand_output, &out_desc)) &&
                out_desc.Monitor == target_monitor) {
                adapter = cand_adapter;
                output  = cand_output;
                break;
            }
            cand_output->lpVtbl->Release(cand_output);
        }
        if (!adapter) cand_adapter->lpVtbl->Release(cand_adapter);
    }

    if (!adapter || !output) { dbg("dxgi: no matching adapter/output"); goto cleanup; }
    if (FAILED(output->lpVtbl->QueryInterface(output, &IID_IDXGIOutput1, (void**)&output1))) {
        dbg("dxgi: QI IDXGIOutput1 FAILED"); goto cleanup;
    }

    if (FAILED(D3D11CreateDevice(
        (IDXGIAdapter *)adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0,
        D3D11_SDK_VERSION, &device, NULL, &ctx))) {
        dbg("dxgi: D3D11CreateDevice FAILED"); goto cleanup;
    }

    if (SUCCEEDED(output->lpVtbl->QueryInterface(output, &IID_IDXGIOutput5, (void**)&output5))) {
        const DXGI_FORMAT supported_formats[] = {
            DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM
        };
        HRESULT hr = output5->lpVtbl->DuplicateOutput1(
            output5, (IUnknown *)device, 0, 2, supported_formats, &dupl);
        if (FAILED(hr)) { dbg("dxgi: DuplicateOutput1 FAILED hr=0x%08lX", hr); goto cleanup; }
    } else {
        HRESULT hr = output1->lpVtbl->DuplicateOutput(output1, (IUnknown *)device, &dupl);
        if (FAILED(hr)) { dbg("dxgi: DuplicateOutput FAILED hr=0x%08lX", hr); goto cleanup; }
    }

    /*
     * Retry loop: acquire frames repeatedly, discarding blank/stale
     * ones, up to a fixed number of attempts. Weak/legacy drivers
     * (older Kepler-class NVIDIA cards among them) are known to
     * occasionally hand back black frames for several calls in a row
     * right after the duplication interface is (re)created.
     */
    {
        const int MAX_ATTEMPTS = 8;
        for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
            if (resource) { resource->lpVtbl->Release(resource); resource = NULL; }
            if (tex) { tex->lpVtbl->Release(tex); tex = NULL; }

            DXGI_OUTDUPL_FRAME_INFO frame_info;
            HRESULT hr = dupl->lpVtbl->AcquireNextFrame(dupl, 500, &frame_info, &resource);
            if (FAILED(hr)) {
                dbg("dxgi: AcquireNextFrame attempt %d FAILED hr=0x%08lX", attempt, hr);
                goto cleanup;
            }

            if (FAILED(resource->lpVtbl->QueryInterface(resource, &IID_ID3D11Texture2D, (void**)&tex))) {
                dbg("dxgi: QI ID3D11Texture2D FAILED"); dupl->lpVtbl->ReleaseFrame(dupl); goto cleanup;
            }

            D3D11_TEXTURE2D_DESC desc;
            tex->lpVtbl->GetDesc(tex, &desc);
            g->w = desc.Width;
            g->h = desc.Height;
            g->is_hdr = (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 1 : 0;

            D3D11_TEXTURE2D_DESC staging_desc = desc;
            staging_desc.Usage = D3D11_USAGE_STAGING;
            staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging_desc.BindFlags = 0;
            staging_desc.MiscFlags = 0;

            if (staging) { staging->lpVtbl->Release(staging); staging = NULL; }
            if (FAILED(device->lpVtbl->CreateTexture2D(device, &staging_desc, NULL, &staging))) {
                dbg("dxgi: CreateTexture2D(staging) FAILED");
                dupl->lpVtbl->ReleaseFrame(dupl); goto cleanup;
            }
            ctx->lpVtbl->CopyResource(ctx, (ID3D11Resource*)staging, (ID3D11Resource*)tex);

            D3D11_MAPPED_SUBRESOURCE mapped;
            if (FAILED(ctx->lpVtbl->Map(ctx, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &mapped))) {
                dbg("dxgi: Map FAILED");
                dupl->lpVtbl->ReleaseFrame(dupl); goto cleanup;
            }

            size_t rgb_bpp = g->is_hdr ? 6 : 3;
            if (g->bits) { free(g->bits); g->bits = NULL; }
            g->size = (size_t)g->w * g->h * rgb_bpp;
            g->bits = (uint8_t *)malloc(g->size);
            if (!g->bits) {
                dbg("dxgi: malloc FAILED");
                ctx->lpVtbl->Unmap(ctx, (ID3D11Resource*)staging, 0);
                dupl->lpVtbl->ReleaseFrame(dupl); goto cleanup;
            }

            uint8_t *dst = g->bits;
            uint8_t *src_row = (uint8_t *)mapped.pData;
            size_t src_pitch = mapped.RowPitch;

            for (int py = 0; py < g->h; py++) {
                uint8_t *src = src_row;
                for (int px = 0; px < g->w; px++) {
                    if (g->is_hdr) {
                        dst[0]=src[0]; dst[1]=src[1]; dst[2]=src[2];
                        dst[3]=src[3]; dst[4]=src[4]; dst[5]=src[5];
                        dst += 6; src += 8;
                    } else {
                        dst[0]=src[2]; dst[1]=src[1]; dst[2]=src[0];
                        dst += 3; src += 4;
                    }
                }
                src_row += src_pitch;
            }

            ctx->lpVtbl->Unmap(ctx, (ID3D11Resource*)staging, 0);
            dupl->lpVtbl->ReleaseFrame(dupl);

            /* Pass the configured blank check mode to the detection function */
            if (!is_frame_blank(g->bits, g->w, g->h, g->is_hdr, g_cfg.blank_check_mode)) {
                dbg("dxgi: attempt %d produced non-blank frame, accepting", attempt);
                ok = 1;
                break;
            }

            dbg("dxgi: attempt %d frame is blank, retrying", attempt);
            SwitchToThread(); // Yields remainder of time slice to the game/DWM
            Sleep(7);         // Fallback to a short sleep to guarantee frame pacing alignment
        }

        if (!ok) dbg("dxgi: all attempts produced blank frames, giving up on DXGI");
    }

cleanup:
    if (staging) staging->lpVtbl->Release(staging);
    if (tex) tex->lpVtbl->Release(tex);
    if (resource) resource->lpVtbl->Release(resource);
    if (dupl) dupl->lpVtbl->Release(dupl);
    if (ctx) ctx->lpVtbl->Release(ctx);
    if (device) device->lpVtbl->Release(device);
    if (output5) output5->lpVtbl->Release(output5);
    if (output1) output1->lpVtbl->Release(output1);
    if (output) output->lpVtbl->Release(output);
    if (adapter) adapter->lpVtbl->Release(adapter);
    if (factory) factory->lpVtbl->Release(factory);

    if (!ok && g->bits) { free(g->bits); g->bits = NULL; }
    dbg("dxgi: end ok=%d", ok);
    return ok;
}


// Public entry point: try DXGI first, fall back to GDI


static int grab_primary_monitor(Grab *g) {
    HMONITOR target_monitor = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);

    if (grab_via_dxgi(g, target_monitor)) {
        return 1;
    }

    dbg("grab: DXGI path failed or stayed blank, falling back to GDI BitBlt");
    return grab_via_gdi(g, target_monitor);
}

static void free_grab(Grab *g) {
    if (g->bits) free(g->bits);
    ZeroMemory(g, sizeof *g);
}


// JPEG XL encoding (Identity SDR/HDR passthrough)

static int encode_jxl_identity(const uint8_t *rgb, int w, int h, int is_hdr, int lossless, float distance, uint8_t **out_buf, size_t *out_size) {
    int ok = 0;
    uint8_t *buf = NULL;
    JxlEncoderStatus st;
    
    // 1. Create a parallel runner to use all available CPU cores (0 = auto-detect)
    void *runner = JxlThreadParallelRunnerCreate(NULL, 0);
    
    JxlEncoder *enc = JxlEncoderCreate(NULL);
    if (!enc) {
        if (runner) JxlThreadParallelRunnerDestroy(runner);
        return 0;
    }

    // 2. Attach the multithreading runner to the encoder
    if (runner) {
        JxlEncoderSetParallelRunner(enc, JxlThreadParallelRunner, runner);
    }

    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = w;
    info.ysize = h;
    
    JxlPixelFormat fmt;
    fmt.num_channels = 3;
    fmt.endianness = JXL_NATIVE_ENDIAN;
    fmt.align = 0;

    if (is_hdr) {
        info.bits_per_sample = 16;
        info.exponent_bits_per_sample = 5; // Indicates float16
        fmt.data_type = JXL_TYPE_FLOAT16;

        /*
         * DXGI HDR desktop duplication uses scRGB:
         *
         *   R/G/B = linear-light sRGB
         *   1.0    = SDR white reference (80 nits)
         *   values > 1.0 represent HDR highlights
         *
         * Leave the samples untouched. libjxl accepts floating-point
         * samples outside 0..1 and encodes them as extended linear sRGB.
         */
    } else {
        info.bits_per_sample = 8;
        info.exponent_bits_per_sample = 0;
        fmt.data_type = JXL_TYPE_UINT8;
    }
    
    if (JxlEncoderSetBasicInfo(enc, &info) != JXL_ENC_SUCCESS) goto done;
    
    JxlColorEncoding ce;
    if (is_hdr) {
        /*
         * The captured FP16 samples are linear scRGB.
         * Use libjxl's canonical linear-sRGB setup rather than
         * manually constructing the same fields.
         */
        JxlColorEncodingSetToLinearSRGB(&ce, JXL_FALSE);

        /*
         * scRGB is a relative-luminance color space whose 1.0 level
         * corresponds to the SDR reference white. Leave intensity_target
         * at its default (0) so libjxl chooses the appropriate target
         * for linear sRGB rather than falsely declaring a fixed HDR peak.
         */
    } else {
        JxlColorEncodingSetToSRGB(&ce, JXL_FALSE);
    }
    
    if (JxlEncoderSetColorEncoding(enc, &ce) != JXL_ENC_SUCCESS) goto done;

    JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    if (!fs) goto done;

    if (lossless) {
        JxlEncoderSetFrameLossless(fs, JXL_TRUE);
    } else {
        JxlEncoderSetFrameDistance(fs, distance);
    }
    JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT, 7);

    size_t npix = (size_t)w * h;
    size_t bytes_per_pixel = is_hdr ? 6 : 3;
    
    if (JxlEncoderAddImageFrame(fs, &fmt, rgb, npix * bytes_per_pixel) != JXL_ENC_SUCCESS) goto done;
    
    JxlEncoderCloseInput(enc);
    
    size_t cap = (size_t)w * h * (is_hdr ? 8 : 4);
    if (cap < (4 << 20)) cap = (4 << 20);
    buf = (uint8_t *)malloc(cap);
    if (!buf) goto done;
    
    uint8_t *next = buf;
    size_t avail = cap;
    for (;;) {
        st = JxlEncoderProcessOutput(enc, &next, &avail);
        if (st == JXL_ENC_SUCCESS) break;
        if (st == JXL_ENC_NEED_MORE_OUTPUT) {
            size_t used = (size_t)(next - buf);
            cap *= 2;
            uint8_t *nb = (uint8_t *)realloc(buf, cap);
            if (!nb) { free(buf); buf = NULL; goto done; }
            buf = nb;
            next = buf + used;
            avail = cap - used;
            continue;
        }
        free(buf); buf = NULL; goto done;
    }
    *out_buf = buf;
    *out_size = cap - avail;
    buf = NULL;
    ok = 1;

done:
    free(buf);
    JxlEncoderDestroy(enc);
    
    // 3. Clean up the parallel runner
    if (runner) JxlThreadParallelRunnerDestroy(runner);
    
    return ok;
}

static int save_rgb_as_jxl(const uint8_t *rgb, int w, int h, int is_hdr, int lossless, float distance, const wchar_t *path) {
    uint8_t *buf = NULL; size_t size = 0;
    if (!encode_jxl_identity(rgb, w, h, is_hdr, lossless, distance, &buf, &size)) return 0;
    
    int ok = 0; FILE *f = _wfopen(path, L"wb");
    if (f) {
        size_t written = fwrite(buf, 1, size, f);
        ok = (written == size); fclose(f);
    }
    free(buf); return ok;
}




// Asynchronous Encoding Worker


typedef struct {
    uint8_t *bits;
    int w, h, is_hdr, lossless;
    float distance;
    wchar_t out_path[MAX_PATH];
} EncodeTask;

static DWORD WINAPI EncodeWorker(LPVOID param) {
    // Tell Windows this is a background task so it yields to the games/OS
    // This help to avoid hammering perfomance
    // and then user may experience some sort of lagging
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    EncodeTask *task = (EncodeTask *)param;
    
    // Perform the heavy encoding and file I/O in the background
    save_rgb_as_jxl(task->bits, task->w, task->h, task->is_hdr, task->lossless, task->distance, task->out_path);
    
    // Clean up memory allocated for this specific task
    free(task->bits);
    free(task);
    return 0;
}





// Entry points & main


#ifndef JXLSHOT_TRAY_BUILD
int main(int argc, char **argv);
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR szCmdLine, int sw) { return main(__argc, __argv); }

int main(int argc, char **argv) {
    // Initialize COM for DXGI/D3D11 stability
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

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

    Grab g;
    if (!grab_primary_monitor(&g)) { 
        free_grab(&g); 
        if (g_dbg) { fclose(g_dbg); g_dbg = NULL; }
        
        // Uninitialize COM on early exit
        CoUninitialize(); 
        return 1; 
    }
    
    wchar_t out_path[MAX_PATH]; 
    build_out_path(out_path, MAX_PATH, g.is_hdr); 
    
    int rc = 0;
    
    // Transfer ownership of the captured buffer to the background thread
    EncodeTask *task = (EncodeTask *)malloc(sizeof(EncodeTask));
    if (task) {
        task->bits = g.bits;
        task->w = g.w;
        task->h = g.h;
        task->is_hdr = g.is_hdr;
        task->lossless = g_cfg.lossless;
        task->distance = g_cfg.distance;
        wcsncpy_s(task->out_path, MAX_PATH, out_path, _TRUNCATE);
        
        // Spawn the background thread
        HANDLE hThread = CreateThread(NULL, 0, EncodeWorker, task, 0, NULL);
        if (hThread) {
            CloseHandle(hThread);      // Detach thread; OS keeps process alive until it finishes
            g.bits = NULL;             // Prevent free_grab from freeing the buffer (thread owns it now)
            dbg("main: encoding offloaded to background thread");
        } else {
            // Fallback to synchronous if thread creation fails
            dbg("main: CreateThread failed, falling back to synchronous save");
            rc = save_rgb_as_jxl(task->bits, task->w, task->h, task->is_hdr, task->lossless, task->distance, task->out_path) ? 0 : 1;
            free(task->bits);
            free(task);
            g.bits = NULL;
        }
    } else {
        // Fallback if malloc fails
        dbg("main: malloc failed for EncodeTask, falling back to synchronous save");
        rc = save_rgb_as_jxl(g.bits, g.w, g.h, g.is_hdr, g_cfg.lossless, g_cfg.distance, out_path) ? 0 : 1;
    }
    
    free_grab(&g); 
    
    // Close the debug log file before exiting
    if (g_dbg) {
        fclose(g_dbg);
        g_dbg = NULL;
    }
    
    // Uninitialize COM before normal exit
    CoUninitialize();
    
    return rc;
}
#endif