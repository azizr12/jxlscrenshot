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
*  THE PICTURE EXPORTING FOLLOW THE EXPORT PATH !!!!!!!
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
#include <math.h>      // DIAG: pow/fabs/ldexpf for pixel statistics
#include <jxl/encode.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <locale.h>
#include <objbase.h>
#include <jxl/thread_parallel_runner.h>
#include <knownfolders.h> // Required for FOLDERID_Pictures
#include <dxgi1_6.h>   // IDXGIOutput6 / DXGI_OUTPUT_DESC1


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
    int blank_check_mode; /* 1=16 samples (4x4), 2=256 samples (16x16), 3=ALL pixels */
    wchar_t export_path[MAX_PATH];
    UINT    hk_full_mod;
    UINT    hk_full_vk;
    UINT    hk_region_mod;
    UINT    hk_region_vk;
} AppConfig;

static AppConfig g_cfg;
static float g_peak_nits = 1000.0f; // Stores the display's actual peak luminance
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




static void ensure_default_ini(void) {
    wchar_t ini_path[MAX_PATH];
    _snwprintf(ini_path, MAX_PATH, L"%s\\jxlshot.ini", g_exe_dir);
    
    BOOL file_exists = (GetFileAttributesW(ini_path) != INVALID_FILE_ATTRIBUTES);
    
    // 1. Read existing values if the file exists, using a marker to detect missing keys.
    const wchar_t *marker = L"__JXLSHOT_KEY_NOT_FOUND__";
    wchar_t buf[512];
    
    wchar_t val_debug[64] = L"";
    wchar_t val_lossless[64] = L"";
    wchar_t val_distance[64] = L"";
    wchar_t val_export_path[MAX_PATH] = L"";
    wchar_t val_blank_check[64] = L"";
    
    // New integer-based hotkey variables
    wchar_t val_hk_full_vk[64] = L"";
    wchar_t val_hk_full_mod[64] = L"";
    wchar_t val_hk_region_vk[64] = L"";
    wchar_t val_hk_region_mod[64] = L"";
    
    BOOL has_debug = FALSE;
    BOOL has_lossless = FALSE;
    BOOL has_distance = FALSE;
    BOOL has_export_path = FALSE;
    BOOL has_blank_check = FALSE;
    
    BOOL has_hk_full_vk = FALSE;
    BOOL has_hk_full_mod = FALSE;
    BOOL has_hk_region_vk = FALSE;
    BOOL has_hk_region_mod = FALSE;

    if (file_exists) {
        #define CHECK_KEY(key, val_buf, size, has_flag) \
            do { \
                if (GetPrivateProfileStringW(L"Capture", key, marker, buf, 512, ini_path) > 0 && wcscmp(buf, marker) != 0) { \
                    wcsncpy_s(val_buf, size, buf, _TRUNCATE); has_flag = TRUE; \
                } else if (GetPrivateProfileStringW(L"Settings", key, marker, buf, 512, ini_path) > 0 && wcscmp(buf, marker) != 0) { \
                    wcsncpy_s(val_buf, size, buf, _TRUNCATE); has_flag = TRUE; \
                } else if (GetPrivateProfileStringW(L"General", key, marker, buf, 512, ini_path) > 0 && wcscmp(buf, marker) != 0) { \
                    wcsncpy_s(val_buf, size, buf, _TRUNCATE); has_flag = TRUE; \
                } \
            } while(0)

        CHECK_KEY(L"Debug", val_debug, 64, has_debug);
        CHECK_KEY(L"Lossless", val_lossless, 64, has_lossless);
        CHECK_KEY(L"Distance", val_distance, 64, has_distance);
        CHECK_KEY(L"ExportPath", val_export_path, MAX_PATH, has_export_path);
        CHECK_KEY(L"BlankCheckMode", val_blank_check, 64, has_blank_check);
        
        // Read new integer-based hotkey values
        CHECK_KEY(L"HotkeyFullVK", val_hk_full_vk, 64, has_hk_full_vk);
        CHECK_KEY(L"HotkeyFullMod", val_hk_full_mod, 64, has_hk_full_mod);
        CHECK_KEY(L"HotkeyRegionVK", val_hk_region_vk, 64, has_hk_region_vk);
        CHECK_KEY(L"HotkeyRegionMod", val_hk_region_mod, 64, has_hk_region_mod);
        
        #undef CHECK_KEY
    }
    
    // 2. Write the full default template. 
    FILE *f = _wfopen(ini_path, L"wb");
    if (f) {
        unsigned short bom = 0xFEFF;
        fwrite(&bom, sizeof(bom), 1, f);
        
        const wchar_t *default_ini = 
            L"; ==============================================================================\n"
            L"; JXLShot Configuration File\n"
            L"; ==============================================================================\n"
            L";\n"
            L"; [Quality & Compression Guide]\n"
            L";   Distance       : Controls the quality vs. file size trade-off.\n"
            L";                    0.0 = True lossless (exact pixel match, larger file).\n"
            L";                    1.0 = Visually lossless (recommended for screenshots).\n"
            L";                    2.0+ = Higher compression, slight quality reduction.\n"
            L";   Lossless       : If set to 1, strictly forces true lossless (overrides Distance).\n"
            L";                    If set to 0, the encoder uses the 'Distance' value above.\n"
            L";\n"
            L"; [General Settings]\n"
            L";   Debug          : 1 = Enable debug logging; 0 = Disable.\n"
            L";   ExportPath     : Custom directory for saving screenshots.\n"
            L";                    Leave blank to use the default Windows Pictures folder.\n"
            L";   BlankCheckMode : 0 = Disabled, 1 = Basic, 2 = Advanced, 3 = HARDCORE.\n"
            L";\n"
            L"; [Hotkey Settings] (Use the 'Hotkey Settings...' tray menu to configure these easily!)\n"
            L";   HotkeyFullVK    : Virtual Key code for Full Screen capture (e.g., 44 = PrintScreen).\n"
            L";   HotkeyFullMod   : Modifier flags for Full Screen (0=None, 1=Shift, 2=Ctrl, 4=Alt, 8=Win).\n"
            L";   HotkeyRegionVK  : Virtual Key code for Region capture.\n"
            L";   HotkeyRegionMod : Modifier flags for Region capture.\n"
            L";\n"
            L"; ==============================================================================\n"
            L"\n"
            L"[Capture]\n"
            L"Debug=1\n"
            L"Lossless=1\n"
            L"Distance=1.0\n"
            L"ExportPath=\n"
            L"HotkeyFullVK=44\n"
            L"HotkeyFullMod=0\n"
            L"HotkeyRegionVK=44\n"
            L"HotkeyRegionMod=2\n"
            L"BlankCheckMode=2\n";
        
        fputws(default_ini, f);
        fclose(f);
    } else {
        return; // Failed to create file
    }
    
    // 3. Restore the user's custom values (only if they existed previously).
    if (file_exists) {
        if (has_debug) WritePrivateProfileStringW(L"Capture", L"Debug", val_debug, ini_path);
        if (has_lossless) WritePrivateProfileStringW(L"Capture", L"Lossless", val_lossless, ini_path);
        if (has_distance) WritePrivateProfileStringW(L"Capture", L"Distance", val_distance, ini_path);
        if (has_export_path) WritePrivateProfileStringW(L"Capture", L"ExportPath", val_export_path, ini_path);
        if (has_blank_check) WritePrivateProfileStringW(L"Capture", L"BlankCheckMode", val_blank_check, ini_path);
        
        // Restore new integer-based hotkey values
        if (has_hk_full_vk) WritePrivateProfileStringW(L"Capture", L"HotkeyFullVK", val_hk_full_vk, ini_path);
        if (has_hk_full_mod) WritePrivateProfileStringW(L"Capture", L"HotkeyFullMod", val_hk_full_mod, ini_path);
        if (has_hk_region_vk) WritePrivateProfileStringW(L"Capture", L"HotkeyRegionVK", val_hk_region_vk, ini_path);
        if (has_hk_region_mod) WritePrivateProfileStringW(L"Capture", L"HotkeyRegionMod", val_hk_region_mod, ini_path);
    }
}



// Simplified INI Parsing 
// (Relies on ensure_default_ini to normalize the file strictly to [Capture])


static int get_cfg_int(LPCWSTR key, int default_val, LPCWSTR ini_path) {
    wchar_t buf[64];
    if (GetPrivateProfileStringW(L"Capture", key, L"", buf, 64, ini_path) > 0) {
        return _wtoi(buf);
    }
    return default_val;
}

static float get_cfg_float(LPCWSTR key, float default_val, LPCWSTR ini_path) {
    wchar_t buf[64];
    if (GetPrivateProfileStringW(L"Capture", key, L"", buf, 64, ini_path) > 0) {
        // Safely parse float using C locale to prevent comma/dot decimal issues in different regions
        _locale_t c_locale = _create_locale(LC_NUMERIC, "C");
        float val = (float)_wcstod_l(buf, NULL, c_locale);
        _free_locale(c_locale);
        return val;
    }
    return default_val;
}

static void get_cfg_string(LPCWSTR key, LPCWSTR default_val, LPWSTR out_buf, DWORD buf_size, LPCWSTR ini_path) {
    if (GetPrivateProfileStringW(L"Capture", key, L"", out_buf, buf_size, ini_path) > 0) {
        return; // Successfully read from [Capture]
    }
    // Fallback to default value if key is missing or empty
    wcsncpy_s(out_buf, buf_size, default_val, _TRUNCATE);
}

static void init_config(void) {
    wchar_t ini_path[MAX_PATH];
    _snwprintf(ini_path, MAX_PATH, L"%s\\jxlshot.ini", g_exe_dir);

    // Force Windows to drop its cached copy of this INI and re-read from disk.
    WritePrivateProfileStringW(NULL, NULL, NULL, ini_path);

    // Set absolute defaults first
    g_cfg.debug_enabled = 1;
    g_cfg.lossless = 1; 
    g_cfg.distance = 1.0f; 
    g_cfg.blank_check_mode = 2;
    g_cfg.hk_full_mod = 0; 
    g_cfg.hk_full_vk = VK_SNAPSHOT;
    g_cfg.hk_region_mod = MOD_CONTROL; 
    g_cfg.hk_region_vk = VK_SNAPSHOT;
    
    // Use Known Folders API to correctly resolve localized folder names
    PWSTR pszPicturesPath = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Pictures, 0, NULL, &pszPicturesPath))) {
        wcsncpy_s(g_cfg.export_path, MAX_PATH, pszPicturesPath, _TRUNCATE);
        CoTaskMemFree(pszPicturesPath); // Free the memory allocated by the API
    } else {
        // Absolute last-resort fallback: Just use the User Profile root directory.
        GetEnvironmentVariableW(L"USERPROFILE", g_cfg.export_path, MAX_PATH);
    }

    // Use robust fallback getters instead of direct GetPrivateProfile* calls
    g_cfg.debug_enabled = get_cfg_int(L"Debug", 1, ini_path);
    g_cfg.lossless = get_cfg_int(L"Lossless", 1, ini_path);
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

    // Read integer-based hotkey config directly instead of parsing strings
    g_cfg.hk_full_vk = get_cfg_int(L"HotkeyFullVK", VK_SNAPSHOT, ini_path);
    g_cfg.hk_full_mod = get_cfg_int(L"HotkeyFullMod", 0, ini_path);
    g_cfg.hk_region_vk = get_cfg_int(L"HotkeyRegionVK", VK_SNAPSHOT, ini_path);
    g_cfg.hk_region_mod = get_cfg_int(L"HotkeyRegionMod", MOD_CONTROL, ini_path);
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


typedef enum {
    CAPTURE_SDR_SRGB,
    CAPTURE_SCRGB,
    CAPTURE_HDR10,
    CAPTURE_UNSUPPORTED
} CaptureType;

static CaptureType classify_capture(DXGI_FORMAT format, DXGI_COLOR_SPACE_TYPE color_space)
{
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
        color_space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)
        return CAPTURE_SCRGB;

    if (format == DXGI_FORMAT_R10G10B10A2_UNORM &&
        color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
        return CAPTURE_HDR10;

    if (format == DXGI_FORMAT_B8G8R8A8_UNORM &&
        color_space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)
        return CAPTURE_SDR_SRGB;

    return CAPTURE_UNSUPPORTED;
}

static const char *capture_type_name(CaptureType t) {
    switch (t) {
        case CAPTURE_SDR_SRGB: return "SDR_SRGB";
        case CAPTURE_SCRGB:    return "SCRGB (FP16 linear)";
        case CAPTURE_HDR10:    return "HDR10 (R10G10B10A2, PQ, Rec.2020)";
        default:               return "UNSUPPORTED";
    }
}

typedef struct {
    uint8_t *bits;
    size_t size;
    int w, h;
    int is_hdr;          // 1 if HDR10 (10-bit samples stored in uint16), 0 if 8-bit SDR
    CaptureType type;
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


// ============================================================================
// DIAGNOSTICS (log only -- none of this changes what is captured or saved)
//
// Goal: assume NOTHING about HDR. Log what Windows, the display, the GPU
// driver, the captured pixels and the JXL encoder REALLY say.
// Needs Debug=1 in jxlshot.ini.  Log: %TMP%\jxlshot_debug.log
// ============================================================================

#define DG_M1 0.1593017578125
#define DG_M2 78.84375
#define DG_C1 0.8359375
#define DG_C2 18.8515625
#define DG_C3 18.6875

/* PQ code (0..1) -> absolute nits */
static double pq_to_nits(double e) {
    if (e <= 0.0) return 0.0;
    if (e > 1.0) e = 1.0;
    double p = pow(e, 1.0 / DG_M2);
    double num = p - DG_C1; if (num < 0.0) num = 0.0;
    double den = DG_C2 - DG_C3 * p;
    return 10000.0 * pow(num / den, 1.0 / DG_M1);
}

/* absolute nits -> PQ code (0..1) */
static double nits_to_pq(double nits) {
    if (nits <= 0.0) return 0.0;
    double y = nits / 10000.0; if (y > 1.0) y = 1.0;
    double p = pow(y, DG_M1);
    return pow((DG_C1 + DG_C2 * p) / (1.0 + DG_C3 * p), DG_M2);
}

static float half_to_float(uint16_t h) {
    uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float v;
    if (e == 0)       v = ldexpf((float)m, -24);
    else if (e == 31) v = m ? (float)NAN : (float)INFINITY;
    else              v = ldexpf((float)(m | 0x400u), (int)e - 25);
    return s ? -v : v;
}

static const char *dg_cs_name(int cs) {
    switch (cs) {
        case 0:  return "RGB_FULL_G22_NONE_P709   (sRGB / SDR)";
        case 1:  return "RGB_FULL_G10_NONE_P709   (scRGB linear)";
        case 2:  return "RGB_STUDIO_G22_NONE_P709";
        case 3:  return "RGB_STUDIO_G22_NONE_P2020";
        case 12: return "RGB_FULL_G2084_NONE_P2020 (HDR10 / PQ / Rec.2020)";
        case 14: return "RGB_STUDIO_G2084_NONE_P2020";
        case 17: return "RGB_FULL_G22_NONE_P2020";
        default: return "other/unknown";
    }
}

static const char *dg_fmt_name(int f) {
    switch (f) {
        case 2:  return "R32G32B32A32_FLOAT";
        case 10: return "R16G16B16A16_FLOAT (scRGB)";
        case 24: return "R10G10B10A2_UNORM";
        case 28: return "R8G8B8A8_UNORM";
        case 87: return "B8G8R8A8_UNORM";
        case 88: return "B8G8R8X8_UNORM";
        default: return "other";
    }
}

/* Windows display configuration: Advanced Color + SDR white level.
 * Own struct definitions so older SDK headers still compile. */

typedef struct {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 value;               /* bit0 supported, bit1 enabled, bit2 wideColorEnforced, bit3 forceDisabled */
    UINT32 colorEncoding;       /* 0 RGB, 1 YCbCr444, 2 YCbCr422, 3 YCbCr420, 4 Intensity */
    UINT32 bitsPerColorChannel;
} DG_ADVCOLOR;

typedef struct {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    ULONG SDRWhiteLevel;        /* 1000 = 80 nits */
} DG_SDRWHITE;

static void diag_log_display_config(const wchar_t *gdi_name) {
    UINT32 np = 0, nm = 0;
    LONG r = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm);
    if (r != ERROR_SUCCESS) { dbg("cfg: GetDisplayConfigBufferSizes failed %ld", r); return; }

    DISPLAYCONFIG_PATH_INFO *paths = (DISPLAYCONFIG_PATH_INFO *)calloc(np ? np : 1, sizeof *paths);
    DISPLAYCONFIG_MODE_INFO *modes = (DISPLAYCONFIG_MODE_INFO *)calloc(nm ? nm : 1, sizeof *modes);
    if (!paths || !modes) { free(paths); free(modes); return; }

    r = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL);
    if (r != ERROR_SUCCESS) { dbg("cfg: QueryDisplayConfig failed %ld", r); free(paths); free(modes); return; }

    dbg("cfg: %u active display path(s)", (unsigned)np);
    for (UINT32 i = 0; i < np; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn;
        ZeroMemory(&sn, sizeof sn);
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof sn;
        sn.header.adapterId = paths[i].sourceInfo.adapterId;
        sn.header.id = paths[i].sourceInfo.id;
        int have_sn = (DisplayConfigGetDeviceInfo(&sn.header) == ERROR_SUCCESS);
        int match = have_sn && gdi_name && wcscmp(sn.viewGdiDeviceName, gdi_name) == 0;

        DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
        ZeroMemory(&tn, sizeof tn);
        tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size = sizeof tn;
        tn.header.adapterId = paths[i].targetInfo.adapterId;
        tn.header.id = paths[i].targetInfo.id;
        int have_tn = (DisplayConfigGetDeviceInfo(&tn.header) == ERROR_SUCCESS);

        DG_ADVCOLOR ac;
        ZeroMemory(&ac, sizeof ac);
        ac.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)9;   /* GET_ADVANCED_COLOR_INFO */
        ac.header.size = sizeof ac;
        ac.header.adapterId = paths[i].targetInfo.adapterId;
        ac.header.id = paths[i].targetInfo.id;
        LONG rac = DisplayConfigGetDeviceInfo(&ac.header);

        DG_SDRWHITE sw;
        ZeroMemory(&sw, sizeof sw);
        sw.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)11;  /* GET_SDR_WHITE_LEVEL */
        sw.header.size = sizeof sw;
        sw.header.adapterId = paths[i].targetInfo.adapterId;
        sw.header.id = paths[i].targetInfo.id;
        LONG rsw = DisplayConfigGetDeviceInfo(&sw.header);

        dbg("cfg: path %u %s source='%ls' monitor='%ls'", (unsigned)i,
            match ? "<== CAPTURED MONITOR" : "",
            have_sn ? sn.viewGdiDeviceName : L"?",
            have_tn ? tn.monitorFriendlyDeviceName : L"?");

        if (rac == ERROR_SUCCESS) {
            dbg("cfg:   AdvancedColor supported=%u enabled=%u wideColorEnforced=%u forceDisabled=%u encoding=%u bitsPerChannel=%u",
                ac.value & 1u, (ac.value >> 1) & 1u, (ac.value >> 2) & 1u, (ac.value >> 3) & 1u,
                ac.colorEncoding, ac.bitsPerColorChannel);
        } else {
            dbg("cfg:   AdvancedColor query FAILED err=%ld", rac);
        }
        if (rsw == ERROR_SUCCESS) {
            dbg("cfg:   SDR white level raw=%lu  => %.1f nits (1000 = 80 nits)",
                sw.SDRWhiteLevel, sw.SDRWhiteLevel / 1000.0 * 80.0);
        } else {
            dbg("cfg:   SDR white level query FAILED err=%ld", rsw);
        }
    }
    free(paths); free(modes);
}

/* Adapter / output / Desc1: everything the OS reports about this display */
static void diag_log_output(IDXGIAdapter1 *adapter, IDXGIOutput *output, IDXGIOutput6 *output6) {
    if (!g_cfg.debug_enabled || !g_dbg) return;

    DXGI_ADAPTER_DESC1 ad;
    if (adapter && SUCCEEDED(adapter->lpVtbl->GetDesc1(adapter, &ad))) {
        dbg("diag: adapter '%ls' vendor=0x%04X device=0x%04X rev=%u flags=0x%X vram=%llu MB",
            ad.Description, ad.VendorId, ad.DeviceId, ad.Revision, ad.Flags,
            (unsigned long long)(ad.DedicatedVideoMemory / (1024 * 1024)));
    }

    DXGI_OUTPUT_DESC od;
    ZeroMemory(&od, sizeof od);
    if (output && SUCCEEDED(output->lpVtbl->GetDesc(output, &od))) {
        dbg("diag: output '%ls' desktop=(%ld,%ld)-(%ld,%ld) rotation=%d attached=%d",
            od.DeviceName, od.DesktopCoordinates.left, od.DesktopCoordinates.top,
            od.DesktopCoordinates.right, od.DesktopCoordinates.bottom,
            (int)od.Rotation, (int)od.AttachedToDesktop);
    }

    if (output6) {
        DXGI_OUTPUT_DESC1 d1;
        if (SUCCEEDED(output6->lpVtbl->GetDesc1(output6, &d1))) {
            dbg("diag: Desc1 ColorSpace=%d  [%s]", (int)d1.ColorSpace, dg_cs_name((int)d1.ColorSpace));
            dbg("diag: Desc1 BitsPerColor=%u", d1.BitsPerColor);
            dbg("diag: Desc1 MaxLuminance=%.1f MinLuminance=%.4f MaxFullFrameLuminance=%.1f",
                d1.MaxLuminance, d1.MinLuminance, d1.MaxFullFrameLuminance);
            dbg("diag: Desc1 Red   primary = %.4f, %.4f", d1.RedPrimary[0],   d1.RedPrimary[1]);
            dbg("diag: Desc1 Green primary = %.4f, %.4f", d1.GreenPrimary[0], d1.GreenPrimary[1]);
            dbg("diag: Desc1 Blue  primary = %.4f, %.4f", d1.BluePrimary[0],  d1.BluePrimary[1]);
            dbg("diag: Desc1 White point   = %.4f, %.4f", d1.WhitePoint[0],   d1.WhitePoint[1]);
            dbg("diag:   ref Rec.709 : R .640,.330  G .300,.600  B .150,.060  W .3127,.3290");
            dbg("diag:   ref Rec.2020: R .708,.292  G .170,.797  B .131,.046  W .3127,.3290");
            dbg("diag:   ref DCI-P3  : R .680,.320  G .265,.690  B .150,.060  W .3127,.3290");
        } else {
            dbg("diag: GetDesc1 FAILED");
        }
    } else {
        dbg("diag: IDXGIOutput6 not available (no Desc1 info)");
    }

    diag_log_display_config(od.DeviceName);
}

static void diag_log_dupl(IDXGIOutputDuplication *dupl, int used_dup1) {
    if (!g_cfg.debug_enabled || !g_dbg || !dupl) return;
    DXGI_OUTDUPL_DESC dd;
    dupl->lpVtbl->GetDesc(dupl, &dd);
    dbg("diag: dupl used DuplicateOutput1=%d (1 = requested format list {R10G10B10A2, B8G8R8A8})", used_dup1);
    dbg("diag: dupl ModeDesc %ux%u format=%d [%s] rotation=%d inSysMem=%d refresh=%u/%u",
        dd.ModeDesc.Width, dd.ModeDesc.Height, (int)dd.ModeDesc.Format, dg_fmt_name((int)dd.ModeDesc.Format),
        (int)dd.Rotation, (int)dd.DesktopImageInSystemMemory,
        dd.ModeDesc.RefreshRate.Numerator, dd.ModeDesc.RefreshRate.Denominator);
}

/* Called right after Map() succeeds, inside the retry loop */
static void diag_log_frame(const DXGI_OUTDUPL_FRAME_INFO *fi, const D3D11_TEXTURE2D_DESC *d, const void *mapped_first) {
    if (!g_cfg.debug_enabled || !g_dbg) return;
    dbg("diag: frame accumulated=%u lastPresent=%lld lastMouse=%lld meta=%u protectedMasked=%d",
        fi->AccumulatedFrames, (long long)fi->LastPresentTime.QuadPart,
        (long long)fi->LastMouseUpdateTime.QuadPart, fi->TotalMetadataBufferSize,
        (int)fi->ProtectedContentMaskedOut);
    dbg("diag: texture %ux%u format=%d [%s] mips=%u array=%u usage=%d bind=0x%X misc=0x%X samples=%u",
        d->Width, d->Height, (int)d->Format, dg_fmt_name((int)d->Format),
        d->MipLevels, d->ArraySize, (int)d->Usage, d->BindFlags, d->MiscFlags, d->SampleDesc.Count);
    if (mapped_first && d->Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        uint32_t p; memcpy(&p, mapped_first, 4);
        dbg("diag: first raw dword=0x%08X -> R=%u G=%u B=%u A=%u (10/10/10/2 bits)",
            p, p & 0x3FF, (p >> 10) & 0x3FF, (p >> 20) & 0x3FF, (p >> 30) & 3u);
    }
}

/* Statistics of the buffer that is actually written to the file */
static void diag_log_pixel_stats(const Grab *g, const char *tag) {
    if (!g_cfg.debug_enabled || !g_dbg || !g->bits) return;

    int W = g->w, H = g->h;
    dbg("stats[%s]: %dx%d is_hdr=%d type=%s bytes=%zu", tag, W, H, g->is_hdr,
        capture_type_name(g->type), g->size);

    size_t total = (size_t)W * H;
    size_t step = total > 2000000 ? total / 2000000 : 1;
    double maxv = g->is_hdr ? 1023.0 : 255.0;

    unsigned mn[3] = {~0u, ~0u, ~0u}, mx[3] = {0, 0, 0};
    double sum[3] = {0, 0, 0}, sat_sum = 0.0;
    size_t n = 0, black = 0, clipped = 0, oog709 = 0, over203 = 0, over_hi = 0;
    double max_nits = 0.0, nits_sum = 0.0;
    size_t hist[8] = {0};     /* max-channel code histogram */
    size_t nb[8] = {0};       /* luminance buckets in nits */
    const double edges[7] = {1, 10, 50, 100, 203, 400, 1000};

    for (size_t i = 0; i < total; i += step) {
        unsigned c[3];
        if (g->is_hdr) {
            const uint16_t *p = (const uint16_t *)g->bits + i * 3;
            c[0] = p[0]; c[1] = p[1]; c[2] = p[2];
        } else {
            const uint8_t *p = g->bits + i * 3;
            c[0] = p[0]; c[1] = p[1]; c[2] = p[2];
        }
        unsigned hi = c[0], lo = c[0];
        for (int k = 0; k < 3; k++) {
            if (c[k] < mn[k]) mn[k] = c[k];
            if (c[k] > mx[k]) mx[k] = c[k];
            sum[k] += c[k];
            if (c[k] > hi) hi = c[k];
            if (c[k] < lo) lo = c[k];
        }
        n++;
        if (!hi) black++;
        if (hi >= (unsigned)maxv) clipped++;
        if (hi) sat_sum += (double)(hi - lo) / (double)hi;
        int b = (int)((hi / maxv) * 7.999); if (b < 0) b = 0; if (b > 7) b = 7;
        hist[b]++;

        if (g->is_hdr) {
            double r = pq_to_nits(c[0] / maxv), gg = pq_to_nits(c[1] / maxv), bb = pq_to_nits(c[2] / maxv);
            double Y = 0.2627 * r + 0.6780 * gg + 0.0593 * bb;
            if (Y > max_nits) max_nits = Y;
            nits_sum += Y;
            if (Y > 203.0) over203++;
            if (Y > 400.0) over_hi++;
            int k = 0; while (k < 7 && Y >= edges[k]) k++;
            nb[k]++;
            double r7 =  1.6605 * r - 0.5876 * gg - 0.0728 * bb;
            double g7 = -0.1246 * r + 1.1329 * gg - 0.0083 * bb;
            double b7 = -0.0182 * r - 0.1006 * gg + 1.1187 * bb;
            double m = r; if (gg > m) m = gg; if (bb > m) m = bb;
            double tol = -0.02 * m;
            if (m > 1.0 && (r7 < tol || g7 < tol || b7 < tol)) oog709++;
        }
    }
    if (!n) return;

    dbg("stats[%s]: sampled=%zu step=%zu", tag, n, step);
    dbg("stats[%s]: R min/max/mean = %u / %u / %.1f", tag, mn[0], mx[0], sum[0] / n);
    dbg("stats[%s]: G min/max/mean = %u / %u / %.1f", tag, mn[1], mx[1], sum[1] / n);
    dbg("stats[%s]: B min/max/mean = %u / %u / %.1f", tag, mn[2], mx[2], sum[2] / n);
    dbg("stats[%s]: black=%.2f%% clipped(max code)=%.2f%%", tag, 100.0 * black / n, 100.0 * clipped / n);
    dbg("stats[%s]: MEAN SATURATION (code domain, (max-min)/max) = %.4f", tag, sat_sum / n);
    dbg("stats[%s]: max-channel histogram dark->bright: %zu %zu %zu %zu %zu %zu %zu %zu", tag,
        hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7]);

    if (g->is_hdr) {
        dbg("stats[%s]: PQ luminance max=%.1f nits mean=%.1f nits  >203nits=%.2f%%  >400nits=%.2f%%",
            tag, max_nits, nits_sum / n, 100.0 * over203 / n, 100.0 * over_hi / n);
        dbg("stats[%s]: luminance buckets nits <1 | 1-10 | 10-50 | 50-100 | 100-203 | 203-400 | 400-1000 | >1000 : "
            "%zu %zu %zu %zu %zu %zu %zu %zu", tag, nb[0], nb[1], nb[2], nb[3], nb[4], nb[5], nb[6], nb[7]);
        dbg("stats[%s]: pixels outside Rec.709 gamut (if read as Rec.2020) = %.3f%%", tag, 100.0 * oog709 / n);
    }

    int pts[5][2] = { {0,0}, {W-1,0}, {0,H-1}, {W-1,H-1}, {W/2,H/2} };
    for (int k = 0; k < 5; k++) {
        size_t idx = (size_t)pts[k][1] * W + pts[k][0];
        if (g->is_hdr) {
            const uint16_t *p = (const uint16_t *)g->bits + idx * 3;
            dbg("stats[%s]: px(%d,%d) code=%u,%u,%u  nits=%.1f,%.1f,%.1f", tag, pts[k][0], pts[k][1],
                p[0], p[1], p[2],
                pq_to_nits(p[0] / 1023.0), pq_to_nits(p[1] / 1023.0), pq_to_nits(p[2] / 1023.0));
        } else {
            const uint8_t *p = g->bits + idx * 3;
            dbg("stats[%s]: px(%d,%d) = %u,%u,%u", tag, pts[k][0], pts[k][1], p[0], p[1], p[2]);
        }
    }
}

/*
 * FP16 / scRGB REFERENCE capture, compared against the 10-bit capture.
 *
 * The FP16 desktop image is Windows' own un-quantised linear scRGB
 * (1.0 = 80 nits, Rec.709 primaries, negatives = wider than 709).
 * We convert it to HDR10 (Rec.2020 + PQ) ourselves and compare it
 * pixel-by-pixel with what the R10G10B10A2 path gave us.
 *   If they agree  -> the capture is truthful, look at tagging/viewer.
 *   If they differ -> the R10G10B10A2 surface is already altered.
 * The output file is NOT affected.
 */
static void diag_probe_fp16(const Grab *ref, HMONITOR mon) {
    if (!g_cfg.debug_enabled || !g_dbg) return;
    dbg("probe: ===== FP16/scRGB reference capture (diagnostic only) =====");

    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL;
    IDXGIOutput *output = NULL;
    IDXGIOutput5 *output5 = NULL;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *ctx = NULL;
    IDXGIOutputDuplication *dupl = NULL;
    IDXGIResource *res = NULL;
    ID3D11Texture2D *tex = NULL, *staging = NULL;
    int frame_held = 0, mapped_ok = 0;
    D3D11_MAPPED_SUBRESOURCE mapped;
    ZeroMemory(&mapped, sizeof mapped);

    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory))) { dbg("probe: CreateDXGIFactory1 FAILED"); goto done; }

    for (UINT ai = 0; !adapter; ai++) {
        IDXGIAdapter1 *ca = NULL;
        HRESULT hr = factory->lpVtbl->EnumAdapters1(factory, ai, &ca);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (!ca) continue;
        for (UINT oi = 0; ; oi++) {
            IDXGIOutput *co = NULL;
            if (ca->lpVtbl->EnumOutputs(ca, oi, &co) == DXGI_ERROR_NOT_FOUND) break;
            if (!co) continue;
            DXGI_OUTPUT_DESC od;
            if (SUCCEEDED(co->lpVtbl->GetDesc(co, &od)) && od.Monitor == mon) { adapter = ca; output = co; break; }
            co->lpVtbl->Release(co);
        }
        if (!adapter) ca->lpVtbl->Release(ca);
    }
    if (!adapter || !output) { dbg("probe: no matching adapter/output"); goto done; }

    if (FAILED(output->lpVtbl->QueryInterface(output, &IID_IDXGIOutput5, (void **)&output5))) {
        dbg("probe: IDXGIOutput5 unavailable -> cannot request FP16 duplication"); goto done;
    }
    if (FAILED(D3D11CreateDevice((IDXGIAdapter *)adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0,
                                 D3D11_SDK_VERSION, &device, NULL, &ctx))) {
        dbg("probe: D3D11CreateDevice FAILED"); goto done;
    }

    {
        const DXGI_FORMAT fp16_only[] = { DXGI_FORMAT_R16G16B16A16_FLOAT };
        HRESULT hr = output5->lpVtbl->DuplicateOutput1(output5, (IUnknown *)device, 0, 1, fp16_only, &dupl);
        dbg("probe: DuplicateOutput1({R16G16B16A16_FLOAT}) hr=0x%08lX", (unsigned long)hr);
        if (FAILED(hr)) goto done;
    }

    {
        int got = 0;
        for (int a = 1; a <= 4; a++) {
            DXGI_OUTDUPL_FRAME_INFO fi;
            if (res) { res->lpVtbl->Release(res); res = NULL; }
            HRESULT hr = dupl->lpVtbl->AcquireNextFrame(dupl, 500, &fi, &res);
            if (FAILED(hr)) { dbg("probe: AcquireNextFrame %d FAILED hr=0x%08lX", a, (unsigned long)hr); goto done; }
            frame_held = 1;
            if (fi.LastPresentTime.QuadPart == 0 && a < 4) { dupl->lpVtbl->ReleaseFrame(dupl); frame_held = 0; continue; }
            got = 1; break;
        }
        if (!got) goto done;
    }

    if (FAILED(res->lpVtbl->QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex))) { dbg("probe: QI texture FAILED"); goto done; }

    D3D11_TEXTURE2D_DESC desc;
    tex->lpVtbl->GetDesc(tex, &desc);
    dbg("probe: frame texture %ux%u format=%d [%s]", desc.Width, desc.Height, (int)desc.Format, dg_fmt_name((int)desc.Format));
    if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
        dbg("probe: driver did NOT give FP16 (got another format) -> no reference available");
        goto done;
    }

    DXGI_OUTDUPL_DESC dd;
    dupl->lpVtbl->GetDesc(dupl, &dd);

    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.BindFlags = 0; sd.MiscFlags = 0;
    if (FAILED(device->lpVtbl->CreateTexture2D(device, &sd, NULL, &staging))) { dbg("probe: staging create FAILED"); goto done; }
    ctx->lpVtbl->CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)tex);
    if (FAILED(ctx->lpVtbl->Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped))) { dbg("probe: Map FAILED"); goto done; }
    mapped_ok = 1;

    int W = (int)desc.Width, H = (int)desc.Height;
    int rot = (int)dd.Rotation;
    int comparable = ref && ref->bits && ref->is_hdr && ref->w == W && ref->h == H && (rot == 0 || rot == 1);
    dbg("probe: rotation=%d comparable_with_10bit_capture=%d", rot, comparable);

    int sx = W / 1000; if (sx < 1) sx = 1;
    int sy = H / 1000; if (sy < 1) sy = 1;

    double maxY = 0, sumY = 0, satFp = 0, satCap = 0, sumYcap = 0;
    double adiff[3] = {0,0,0}, mfp[3] = {0,0,0}, mcap[3] = {0,0,0};
    size_t n = 0, neg = 0, nonfinite = 0;

    for (int y = 0; y < H; y += sy) {
        const uint8_t *row = (const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch;
        for (int x = 0; x < W; x += sx) {
            const uint16_t *h = (const uint16_t *)(row + (size_t)x * 8);
            double r  = half_to_float(h[0]) * 80.0;
            double gg = half_to_float(h[1]) * 80.0;
            double b  = half_to_float(h[2]) * 80.0;
            if (!isfinite(r) || !isfinite(gg) || !isfinite(b)) { nonfinite++; continue; }

            double Y = 0.2126 * r + 0.7152 * gg + 0.0722 * b;
            if (Y > maxY) maxY = Y;
            sumY += Y;
            if (r < -0.5 || gg < -0.5 || b < -0.5) neg++;

            /* Rec.709 linear -> Rec.2020 linear -> PQ 10-bit code */
            double r2 = 0.6274 * r + 0.3293 * gg + 0.0433 * b;
            double g2 = 0.0691 * r + 0.9195 * gg + 0.0114 * b;
            double b2 = 0.0164 * r + 0.0880 * gg + 0.8956 * b;
            double fc[3] = { nits_to_pq(r2) * 1023.0, nits_to_pq(g2) * 1023.0, nits_to_pq(b2) * 1023.0 };
            double hi = fc[0], lo = fc[0];
            for (int k = 1; k < 3; k++) { if (fc[k] > hi) hi = fc[k]; if (fc[k] < lo) lo = fc[k]; }
            if (hi > 0) satFp += (hi - lo) / hi;
            for (int k = 0; k < 3; k++) mfp[k] += fc[k];

            if (comparable) {
                const uint16_t *c = (const uint16_t *)ref->bits + ((size_t)y * W + x) * 3;
                double cc[3] = { c[0], c[1], c[2] };
                double chi = cc[0], clo = cc[0];
                for (int k = 1; k < 3; k++) { if (cc[k] > chi) chi = cc[k]; if (cc[k] < clo) clo = cc[k]; }
                if (chi > 0) satCap += (chi - clo) / chi;
                for (int k = 0; k < 3; k++) { adiff[k] += fabs(cc[k] - fc[k]); mcap[k] += cc[k]; }
                sumYcap += 0.2627 * pq_to_nits(cc[0] / 1023.0) + 0.6780 * pq_to_nits(cc[1] / 1023.0) + 0.0593 * pq_to_nits(cc[2] / 1023.0);
            }
            n++;
        }
    }

    if (n) {
        dbg("probe: FP16 sampled=%zu nonfinite=%zu  scRGB-negative(outside 709)=%.3f%%", n, nonfinite, 100.0 * neg / n);
        dbg("probe: FP16 luminance max=%.1f nits mean=%.1f nits", maxY, sumY / n);
        dbg("probe: FP16->HDR10 (2020+PQ) mean code R/G/B = %.1f / %.1f / %.1f", mfp[0] / n, mfp[1] / n, mfp[2] / n);
        dbg("probe: FP16->HDR10 MEAN SATURATION = %.4f", satFp / n);
        if (comparable) {
            dbg("probe: 10-bit CAPTURE mean code R/G/B = %.1f / %.1f / %.1f", mcap[0] / n, mcap[1] / n, mcap[2] / n);
            dbg("probe: 10-bit CAPTURE MEAN SATURATION = %.4f", satCap / n);
            dbg("probe: 10-bit CAPTURE mean luminance = %.1f nits", sumYcap / n);
            dbg("probe: mean |capture - fp16derived| code diff R/G/B = %.2f / %.2f / %.2f  (0 = identical, 1023 = full scale)",
                adiff[0] / n, adiff[1] / n, adiff[2] / n);
            dbg("probe: >> similar saturation + tiny diff = 10-bit capture is truthful. Much lower capture saturation = surface is altered.");
            dbg("probe: >> (desktop must be static between the two captures for exact agreement)");
        }
    }

    if (comparable) {
        for (int gy = 1; gy <= 3; gy++) for (int gx = 1; gx <= 3; gx++) {
            int x = W * gx / 4, y = H * gy / 4;
            const uint16_t *h = (const uint16_t *)((const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch + (size_t)x * 8);
            double r = half_to_float(h[0]) * 80.0, gg = half_to_float(h[1]) * 80.0, b = half_to_float(h[2]) * 80.0;
            double r2 = 0.6274 * r + 0.3293 * gg + 0.0433 * b;
            double g2 = 0.0691 * r + 0.9195 * gg + 0.0114 * b;
            double b2 = 0.0164 * r + 0.0880 * gg + 0.8956 * b;
            const uint16_t *c = (const uint16_t *)ref->bits + ((size_t)y * W + x) * 3;
            dbg("probe: px(%d,%d) scRGBnits=%.1f,%.1f,%.1f | fp16->PQ2020 code=%.0f,%.0f,%.0f | 10bit capture code=%u,%u,%u",
                x, y, r, gg, b,
                nits_to_pq(r2) * 1023.0, nits_to_pq(g2) * 1023.0, nits_to_pq(b2) * 1023.0,
                c[0], c[1], c[2]);
        }
    }

done:
    if (mapped_ok && ctx && staging) ctx->lpVtbl->Unmap(ctx, (ID3D11Resource *)staging, 0);
    if (frame_held && dupl) dupl->lpVtbl->ReleaseFrame(dupl);
    if (staging) staging->lpVtbl->Release(staging);
    if (tex) tex->lpVtbl->Release(tex);
    if (res) res->lpVtbl->Release(res);
    if (dupl) dupl->lpVtbl->Release(dupl);
    if (ctx) ctx->lpVtbl->Release(ctx);
    if (device) device->lpVtbl->Release(device);
    if (output5) output5->lpVtbl->Release(output5);
    if (output) output->lpVtbl->Release(output);
    if (adapter) adapter->lpVtbl->Release(adapter);
    if (factory) factory->lpVtbl->Release(factory);
    dbg("probe: ===== end =====");
}

/* Encoder: exactly what is written into the .jxl */
static void diag_log_enc_info(int w, int h, int is_hdr, int lossless, float distance, const JxlBasicInfo *info) {
    if (!g_cfg.debug_enabled || !g_dbg) return;
    uint32_t v = JxlEncoderVersion();
    dbg("enc: libjxl version %u.%u.%u", v / 1000000, (v / 1000) % 1000, v % 1000);
    dbg("enc: %dx%d hdr=%d lossless=%d distance=%.2f", w, h, is_hdr, lossless, distance);
    dbg("enc: basic_info bits=%u exp_bits=%u intensity_target=%.1f min_nits=%.4f uses_original_profile=%d alpha_bits=%u",
        info->bits_per_sample, info->exponent_bits_per_sample, info->intensity_target,
        info->min_nits, (int)info->uses_original_profile, info->alpha_bits);
}

static void diag_log_enc_color(const JxlColorEncoding *ce) {
    if (!g_cfg.debug_enabled || !g_dbg) return;
    dbg("enc: color_encoding space=%d (0=RGB) white=%d (1=D65) primaries=%d (1=sRGB 2=custom 9=Rec2100/2020) "
        "transfer=%d (8=linear 13=sRGB 16=PQ 18=HLG) intent=%d (0=perceptual 1=relative)",
        (int)ce->color_space, (int)ce->white_point, (int)ce->primaries,
        (int)ce->transfer_function, (int)ce->rendering_intent);
}

// ============================ end of DIAGNOSTICS ============================


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
    g->w = w; g->h = h; g->is_hdr = 0; g->type = CAPTURE_SDR_SRGB;
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
    IDXGIOutput6 *output6 = NULL;
    IDXGIOutputDuplication *dupl = NULL;
    IDXGIResource *resource = NULL;
    ID3D11Texture2D *tex = NULL;
    ID3D11Texture2D *staging = NULL;
    DXGI_COLOR_SPACE_TYPE out_cs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709; // assume SDR if unknown
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

    if (SUCCEEDED(output->lpVtbl->QueryInterface(output, &IID_IDXGIOutput6, (void**)&output6))) {
        DXGI_OUTPUT_DESC1 d1;
        if (SUCCEEDED(output6->lpVtbl->GetDesc1(output6, &d1))) {
            out_cs = d1.ColorSpace;
            if (d1.MaxLuminance > 0.0f) g_peak_nits = d1.MaxLuminance;  // Added: Store actual display peak
            dbg("dxgi: output color space = %d, BitsPerColor=%u, MaxLum=%.1f, MinLum=%.4f, MaxFullFrameLum=%.1f",
                (int)d1.ColorSpace, d1.BitsPerColor,
                d1.MaxLuminance, d1.MinLuminance, d1.MaxFullFrameLuminance);
        } else {
            dbg("dxgi: IDXGIOutput6::GetDesc1 FAILED, assuming SDR sRGB colorspace");
        }
    } else {
        dbg("dxgi: IDXGIOutput6 not available, assuming SDR sRGB colorspace");
    }

    // DIAG: full dump of adapter / output / Desc1 / Windows Advanced Color + SDR white level
    diag_log_output(adapter, output, output6);

    if (FAILED(D3D11CreateDevice(
        (IDXGIAdapter *)adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0,
        D3D11_SDK_VERSION, &device, NULL, &ctx))) {
        dbg("dxgi: D3D11CreateDevice FAILED"); goto cleanup;
    }

    if (SUCCEEDED(output->lpVtbl->QueryInterface(output, &IID_IDXGIOutput5, (void**)&output5))) {
        const DXGI_FORMAT supported_formats[] = {
            DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM
        };
        HRESULT hr = output5->lpVtbl->DuplicateOutput1(
            output5, (IUnknown *)device, 0, 2, supported_formats, &dupl);
        if (FAILED(hr)) { dbg("dxgi: DuplicateOutput1 FAILED hr=0x%08lX", hr); goto cleanup; }
    } else {
        HRESULT hr = output1->lpVtbl->DuplicateOutput(output1, (IUnknown *)device, &dupl);
        if (FAILED(hr)) { dbg("dxgi: DuplicateOutput FAILED hr=0x%08lX", hr); goto cleanup; }
    }

    // DIAG: what the duplication object really reports
    diag_log_dupl(dupl, output5 != NULL);

    /*
     * Retry loop: weak/legacy drivers (older Kepler-class NVIDIA cards among
     * them) can return black frames for several calls in a row right after
     * the duplication interface is created.
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

            // GET ROTATION FROM DXGI

            D3D11_TEXTURE2D_DESC desc;
            tex->lpVtbl->GetDesc(tex, &desc);

            DXGI_OUTDUPL_DESC dupl_desc;
            dupl->lpVtbl->GetDesc(dupl, &dupl_desc);
            DXGI_MODE_ROTATION rotation = dupl_desc.Rotation;
            dbg("dxgi: DXGI reports rotation = %d (1=Identity, 2=90, 3=180, 4=270)", rotation);

            CaptureType ctype = classify_capture(desc.Format, out_cs);
            dbg("dxgi: frame format=%d, colorspace=%d -> %s",
                (int)desc.Format, (int)out_cs, capture_type_name(ctype));

            if (ctype != CAPTURE_SDR_SRGB && ctype != CAPTURE_HDR10) {
                dbg("dxgi: unsupported capture type, aborting DXGI path (will fall back to GDI)");
                dupl->lpVtbl->ReleaseFrame(dupl);
                goto cleanup;
            }

            int w_phys = desc.Width;
            int h_phys = desc.Height;
            int w_logical = w_phys;
            int h_logical = h_phys;

            // Swap dimensions if the monitor is rotated 90 or 270 degrees

            if (rotation == DXGI_MODE_ROTATION_ROTATE90 || rotation == DXGI_MODE_ROTATION_ROTATE270) {
                w_logical = h_phys;
                h_logical = w_phys;
            }

            g->w = w_logical;
            g->h = h_logical;
            g->type = ctype;
            g->is_hdr = (ctype == CAPTURE_HDR10) ? 1 : 0;

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

            // DIAG: frame info, full texture description, first raw 32-bit pixel
            diag_log_frame(&frame_info, &desc, mapped.pData);

            size_t rgb_bpp = g->is_hdr ? 6 : 3; // HDR10: 3 x uint16, SDR: 3 x uint8
            size_t src_bpp = 4;                 // R10G10B10A2 and B8G8R8A8 are both 32-bit

            if (g->bits) { free(g->bits); g->bits = NULL; }

            // Allocate using LOGICAL dimensions to prevent out-of-bounds writes
            g->size = (size_t)g->w * g->h * rgb_bpp;
            g->bits = (uint8_t *)malloc(g->size);
            if (!g->bits) {
                dbg("dxgi: malloc FAILED");
                ctx->lpVtbl->Unmap(ctx, (ID3D11Resource*)staging, 0);
                dupl->lpVtbl->ReleaseFrame(dupl); goto cleanup;
            }

            uint8_t *src_row = (uint8_t *)mapped.pData;
            size_t src_pitch = mapped.RowPitch;

            // Copy, convert format, and rotate in a single pass
            for (int py = 0; py < h_phys; py++) {
                uint8_t *src = src_row;
                for (int px = 0; px < w_phys; px++) {
                    int dx, dy;

                    // Map physical pixels to logical coordinates based on rotation
                    // Calculate destination pointer in the logical buffer

                    switch (rotation) {
                        case DXGI_MODE_ROTATION_ROTATE90:
                            dx = h_phys - 1 - py;
                            dy = px;
                            break;
                        case DXGI_MODE_ROTATION_ROTATE180:
                            dx = w_phys - 1 - px;
                            dy = h_phys - 1 - py;
                            break;
                        case DXGI_MODE_ROTATION_ROTATE270:
                            dx = py;
                            dy = w_phys - 1 - px;
                            break;
                        default:
                            dx = px;
                            dy = py;
                            break;
                    }

                    uint8_t *dst_px = g->bits + ((size_t)dy * w_logical + dx) * rgb_bpp;

                    if (g->is_hdr) {
                        // R10G10B10A2_UNORM: R=bits0-9, G=bits10-19, B=bits20-29, A=bits30-31
                        uint32_t p;
                        memcpy(&p, src, 4);
                        uint16_t *d = (uint16_t *)dst_px;
                        d[0] = (uint16_t)( p        & 0x3FF);
                        d[1] = (uint16_t)((p >> 10) & 0x3FF);
                        d[2] = (uint16_t)((p >> 20) & 0x3FF);
                    } else {
                        // B8G8R8A8 -> RGB24
                        dst_px[0] = src[2]; dst_px[1] = src[1]; dst_px[2] = src[0];
                    }
                    src += src_bpp;
                }
                src_row += src_pitch;
            }


            ctx->lpVtbl->Unmap(ctx, (ID3D11Resource*)staging, 0);
            dupl->lpVtbl->ReleaseFrame(dupl);

            // Pass the configured blank check mode to the detection function

            if (!is_frame_blank(g->bits, g->w, g->h, g->is_hdr, g_cfg.blank_check_mode)) {
                dbg("dxgi: attempt %d produced non-blank frame (%s, %dx%d), accepting",
                    attempt, capture_type_name(g->type), g->w, g->h);
                ok = 1;
                break;
            }

            dbg("dxgi: attempt %d frame is blank, retrying", attempt);
            SwitchToThread();
            Sleep(7);
        }

        // Re-evaluate the 8th frame using Mode 3 (ALL pixels)

        if (!ok) {
            dbg("dxgi: all %d attempts deemed blank by mode %d. Running one last full-pixel (mode 3) check on the final frame.", MAX_ATTEMPTS, g_cfg.blank_check_mode);

            if (g->bits && !is_frame_blank(g->bits, g->w, g->h, g->is_hdr, 3)) {
                dbg("dxgi: full-pixel check found non-black pixels! Accepting frame.");
                ok = 1;
            } else {
                dbg("dxgi: full-pixel check confirmed frame is truly blank. Giving up on DXGI.");
            }
        }
    }

cleanup:
    if (staging) staging->lpVtbl->Release(staging);
    if (tex) tex->lpVtbl->Release(tex);
    if (resource) resource->lpVtbl->Release(resource);
    if (dupl) dupl->lpVtbl->Release(dupl);
    if (ctx) ctx->lpVtbl->Release(ctx);
    if (device) device->lpVtbl->Release(device);
    if (output6) output6->lpVtbl->Release(output6);
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
        // DIAG: statistics of the exact buffer that will be saved, then an
        // independent FP16/scRGB capture to compare against it (log only)
        diag_log_pixel_stats(g, "dxgi-final");
        diag_probe_fp16(g, target_monitor);
        return 1;
    }

    dbg("grab: DXGI path failed or stayed blank, falling back to GDI BitBlt");
    int gdi_ok = grab_via_gdi(g, target_monitor);
    if (gdi_ok) diag_log_pixel_stats(g, "gdi-final");   // DIAG
    return gdi_ok;
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

    // 0 = auto-detect thread count
    void *runner = JxlThreadParallelRunnerCreate(NULL, 0);

    JxlEncoder *enc = JxlEncoderCreate(NULL);
    if (!enc) {
        if (runner) JxlThreadParallelRunnerDestroy(runner);
        return 0;
    }
    // Attach the multithreading runner to the encoder
    if (runner) {
        JxlEncoderSetParallelRunner(enc, JxlThreadParallelRunner, runner);
    }

    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = w;
    info.ysize = h;
    info.uses_original_profile = lossless ? JXL_TRUE : JXL_FALSE; // lossless requires the original profile

    JxlPixelFormat fmt;
    fmt.num_channels = 3;
    fmt.endianness = JXL_NATIVE_ENDIAN;
    fmt.align = 0;

    if (is_hdr) {
        // HDR10: raw 10-bit values (0..1023) stored in uint16
        info.bits_per_sample = 10;
        info.exponent_bits_per_sample = 0;
        info.intensity_target = g_peak_nits; // Modified: Use actual display peak instead of 10000.0f
        fmt.data_type = JXL_TYPE_UINT16;
    } else {
        info.bits_per_sample = 8;
        info.exponent_bits_per_sample = 0;
        fmt.data_type = JXL_TYPE_UINT8;
    }

    // DIAG: exactly what the encoder is told
    diag_log_enc_info(w, h, is_hdr, lossless, distance, &info);

    if (JxlEncoderSetBasicInfo(enc, &info) != JXL_ENC_SUCCESS) goto done;

    JxlColorEncoding ce;
    if (is_hdr) {
        memset(&ce, 0, sizeof ce);
        ce.color_space       = JXL_COLOR_SPACE_RGB;
        ce.white_point       = JXL_WHITE_POINT_D65;
        ce.primaries         = JXL_PRIMARIES_2100;        // Rec.2020
        ce.transfer_function = JXL_TRANSFER_FUNCTION_PQ;  // ST 2084
        ce.rendering_intent  = JXL_RENDERING_INTENT_RELATIVE;
    } else {
        JxlColorEncodingSetToSRGB(&ce, JXL_FALSE);
    }

    // DIAG: exactly which color tag is written into the file
    diag_log_enc_color(&ce);

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
    dbg("enc: encoded OK, %zu bytes", cap - avail);   // DIAG
    *out_buf = buf;
    *out_size = cap - avail;
    buf = NULL;
    ok = 1;

done:
    free(buf);
    JxlEncoderDestroy(enc);
    // Clean up the parallel runner
    if (runner) JxlThreadParallelRunnerDestroy(runner);
    return ok;
}

static int save_rgb_as_jxl(const uint8_t *rgb, int w, int h, int is_hdr, int lossless, float distance, const wchar_t *path) {
    uint8_t *buf = NULL; 
    size_t size = 0;
    
    if (!encode_jxl_identity(rgb, w, h, is_hdr, lossless, distance, &buf, &size)) {
        dbg("save_rgb_as_jxl: encode_jxl_identity failed");
        return 0;
    }
    
    // Safeguard: If the encoder somehow produced 0 bytes, treat it as a failure.
    if (size == 0) {
        dbg("save_rgb_as_jxl: Encoder produced 0 bytes. Aborting write.");
        free(buf);
        return 0;
    }

    int ok = 0; 
    FILE *f = _wfopen(path, L"wb");
    if (f) {
        size_t written = fwrite(buf, 1, size, f);
        if (written != size) {
            dbg("save_rgb_as_jxl: fwrite failed (written %zu of %zu). ferror: %d, GetLastError: %lu", 
                written, size, ferror(f), GetLastError());
            ok = 0;
        } else if (fflush(f) != 0) {
            // Crucial: Catch disk-full or I/O errors that happen during buffer flush
            dbg("save_rgb_as_jxl: fflush failed (potential disk full or I/O error). ferror: %d, GetLastError: %lu", 
                ferror(f), GetLastError());
            ok = 0;
        } else {
            ok = 1; // Write and flush succeeded
        }
        fclose(f);
    } else {
        DWORD err = GetLastError();
        const wchar_t *err_desc = L"Unknown error";
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) err_desc = L"Path not found";
        else if (err == ERROR_ACCESS_DENIED) err_desc = L"Access denied";
        else if (err == ERROR_DISK_FULL) err_desc = L"Disk full";
        dbg("save_rgb_as_jxl: _wfopen FAILED for path '%ls'. Error code: %lu (%ls)", path, err, err_desc);
    }
    
    if (!ok) {
        // Fallback logic: try saving to the user's localized Pictures folder
        dbg("save_rgb_as_jxl: Primary save failed. Attempting fallback to User Pictures folder.");
        
        wchar_t fallback_path[MAX_PATH];
        wchar_t fallback_dir[MAX_PATH];
        
        // Use Known Folders API to guarantee the correct localized folder name
        PWSTR pszPicturesPath = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Pictures, 0, NULL, &pszPicturesPath))) {
            wcsncpy_s(fallback_dir, MAX_PATH, pszPicturesPath, _TRUNCATE);
            CoTaskMemFree(pszPicturesPath); // Free the memory allocated by the API
        } else {
            // Absolute last-resort fallback: Just use the User Profile root directory.
            GetEnvironmentVariableW(L"USERPROFILE", fallback_dir, MAX_PATH);
        }
        
        size_t len = wcslen(fallback_dir);
        if (len > 0 && fallback_dir[len - 1] != L'\\') {
            wcsncat_s(fallback_dir, MAX_PATH, L"\\", _TRUNCATE);
        }
        
        const wchar_t *filename = wcsrchr(path, L'\\');
        filename = filename ? filename + 1 : path;
        
        _snwprintf(fallback_path, MAX_PATH, L"%s%s", fallback_dir, filename);
        fallback_path[MAX_PATH - 1] = L'\0';
        
        dbg("save_rgb_as_jxl: Retrying save to fallback path: %ls", fallback_path);
        
        FILE *f_fallback = _wfopen(fallback_path, L"wb");
        if (f_fallback) {
            size_t written_fb = fwrite(buf, 1, size, f_fallback);
            if (written_fb != size) {
                dbg("save_rgb_as_jxl: fallback fwrite failed. ferror: %d", ferror(f_fallback));
                ok = 0;
            } else if (fflush(f_fallback) != 0) {
                dbg("save_rgb_as_jxl: fallback fflush failed. ferror: %d", ferror(f_fallback));
                ok = 0;
            } else {
                ok = 1; // Fallback succeeded
            }
            fclose(f_fallback);
        } else {
            dbg("save_rgb_as_jxl: fallback _wfopen FAILED. GetLastError: %lu", GetLastError());
        }
        
        // User Notifications
        if (!ok) {
            wchar_t err_msg[512];
            _snwprintf(err_msg, 512, 
                L"Failed to save screenshot to:\n%s\n\nFallback to Pictures folder:\n%s\nalso failed.\n\n"
                L"Please check disk space and permissions.", 
                path, fallback_path);
            MessageBoxW(NULL, err_msg, L"jxlshot Save Error", MB_ICONERROR | MB_OK | MB_SYSTEMMODAL);
        } else {
            wchar_t success_msg[512];
            _snwprintf(success_msg, 512, 
                L"Failed to save to configured export path.\n\n"
                L"Screenshot was successfully saved to fallback location:\n%s", 
                fallback_path);
            MessageBoxW(NULL, success_msg, L"jxlshot Fallback Save", MB_ICONWARNING | MB_OK | MB_SYSTEMMODAL);
        }
    }
    
    free(buf); 
    return ok;
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
            g.bits = NULL; // Prevent free_grab from freeing the buffer (thread owns it now)
            dbg("main: encoding offloaded to background thread");
            
            // CRITICAL: Wait for the thread to finish. 
            // Returning from main() calls ExitProcess()
            // which instantly 
            // kills all background threads, aborting the save.
            WaitForSingleObject(hThread, INFINITE);
            CloseHandle(hThread);
            dbg("main: background thread completed");
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