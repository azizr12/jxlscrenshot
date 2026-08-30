/*
* Captures the primary monitor and saves it as JPEG XL (.jxl).
* Supports native HDR capture (Rec.2020 + PQ) via DXGI Desktop Duplication.
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
* Captures the primary monitor and saves it as JPEG XL (.jxl).
* Supports native HDR capture (Rec.2020 + PQ) via DXGI Desktop Duplication.
* Compiled as C++ for clean COM interface handling.
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
#include <math.h>
#include <jxl/encode.h>
#include <dxgi1_2.h>
#include <d3d11.h>

static void set_dpi_aware(void);
static void init_paths(void);
static void ensure_default_ini(void);
static void init_config(void);
static void dbg_init(void);
static void build_out_path(wchar_t *path, int n, int is_hdr);
static float half_to_float(uint16_t h);

typedef struct {
    int debug_enabled, lossless, show_cursor, force_sdr;
    float distance;
    wchar_t export_path[MAX_PATH];
    UINT hk_full_mod, hk_full_vk, hk_region_mod, hk_region_vk;
} AppConfig;

static AppConfig g_cfg;
static wchar_t g_exe_dir[MAX_PATH];
static FILE *g_dbg = NULL;

static void init_paths(void) {
    wchar_t tmp[MAX_PATH];
    GetModuleFileNameW(NULL, tmp, MAX_PATH);
    wchar_t *slash = wcsrchr(tmp, L'\\');
    if (slash) *slash = 0;
    wcsncpy(g_exe_dir, tmp, MAX_PATH - 1);
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
            fprintf(f, "[Capture]\nDebug=0\nLossless=1\nDistance=1.0\nShowCursor=1\nForceSDR=0\nExportPath=\nHotkeyFull=PrintScreen\nHotkeyRegion=Ctrl+PrintScreen\n");
            fclose(f);
        }
    }
}

static void init_config(void) {
    wchar_t ini_path[MAX_PATH];
    _snwprintf(ini_path, MAX_PATH, L"%s\\jxlshot.ini", g_exe_dir);
    g_cfg.debug_enabled = 1; g_cfg.lossless = 1; g_cfg.distance = 1.0f; 
    g_cfg.show_cursor = 1; g_cfg.force_sdr = 0;
    g_cfg.hk_full_mod = 0; g_cfg.hk_full_vk = VK_SNAPSHOT;
    g_cfg.hk_region_mod = MOD_CONTROL; g_cfg.hk_region_vk = VK_SNAPSHOT;
    
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_MYPICTURES, NULL, SHGFP_TYPE_CURRENT, g_cfg.export_path))) {
        GetEnvironmentVariableW(L"USERPROFILE", g_cfg.export_path, MAX_PATH);
        wcscat_s(g_cfg.export_path, MAX_PATH, L"\\Pictures");
    }

    g_cfg.debug_enabled = GetPrivateProfileIntW(L"Capture", L"Debug", 1, ini_path);
    g_cfg.lossless = GetPrivateProfileIntW(L"Capture", L"Lossless", 1, ini_path);
    g_cfg.show_cursor = GetPrivateProfileIntW(L"Capture", L"ShowCursor", 1, ini_path);
    g_cfg.force_sdr = GetPrivateProfileIntW(L"Capture", L"ForceSDR", 0, ini_path);
    
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

static void dbg_init(void) {
    if (!g_cfg.debug_enabled) { g_dbg = NULL; return; }
    wchar_t temp_dir[MAX_PATH], log_path[MAX_PATH];
    GetTempPathW(MAX_PATH, temp_dir);
    _snwprintf(log_path, MAX_PATH, L"%s\\jxlshot_debug.log", temp_dir);
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

static void set_dpi_aware(void) {
    typedef BOOL (WINAPI *Fn)(HANDLE);
    Fn f = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (f) f((HANDLE)(LONG_PTR)-4);
    else SetProcessDPIAware();
}

static void build_out_path(wchar_t *path, int n, int is_hdr) {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t safe_dir[MAX_PATH];
    wcsncpy(safe_dir, g_cfg.export_path, MAX_PATH - 1);
    safe_dir[MAX_PATH - 1] = L'\0';
    size_t len = wcslen(safe_dir);
    if (len > 0 && safe_dir[len - 1] != L'\\') wcsncat(safe_dir, L"\\", MAX_PATH - len - 1);
    _snwprintf(path, n, L"%sjxlshot%s_%04d%02d%02d_%02d%02d%02d_%03d.jxl",
               safe_dir, is_hdr ? L"_hdr" : L"", st.wYear, st.wMonth, st.wDay, 
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

static float half_to_float(uint16_t h) {
    uint32_t sign = (h >> 15) & 0x1, exp = (h >> 10) & 0x1f, mant = h & 0x3ff;
    if (exp == 0) {
        if (mant == 0) return sign ? -0.0f : 0.0f;
        return sign ? -((float)mant * 5.9604644775390625e-8f) : ((float)mant * 5.9604644775390625e-8f);
    }
    if (exp == 31) return mant == 0 ? (sign ? -INFINITY : INFINITY) : NAN;
    uint32_t ieee = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
    float result; memcpy(&result, &ieee, sizeof(float));
    return result;
}

/* ---- HDR color pipeline helpers -------------------------------------
 * Windows composes an HDR desktop into DXGI_FORMAT_R16G16B16A16_FLOAT
 * ("scRGB"): linear-light samples using Rec.709/sRGB primaries, where
 * 1.0 = 80 nits (the SDR reference white) and values can go negative
 * (extended gamut) or well above 1.0 (HDR highlights, up to ~125.0 for
 * 10,000 nits). JPEG XL's PQ transfer function instead expects samples
 * that are ALREADY PQ (ST.2084) encoded in Rec.2020/2100 primaries,
 * normalized so 1.0 = 10,000 nits. The two encodings are NOT
 * interchangeable, so scRGB data must be explicitly converted:
 *   1) Rec.709 primaries -> Rec.2020 primaries (still linear light)
 *   2) Rescale from "1.0 = 80 nits" to "1.0 = 10,000 nits"
 *   3) Apply the ST.2084 (PQ) OETF to get the actual PQ code values
 * ---------------------------------------------------------------------*/
#define SCRGB_REFERENCE_WHITE_NITS 80.0f
#define PQ_MAX_NITS                10000.0f

static float pq_oetf(float L) {
    /* L: normalized scene-linear light, 0.0-1.0 where 1.0 == 10,000 nits */
    if (L < 0.0f) L = 0.0f;
    if (L > 1.0f) L = 1.0f;
    const float m1 = 0.1593017578125f;
    const float m2 = 78.84375f;
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;
    float Lm1 = powf(L, m1);
    float num = c1 + c2 * Lm1;
    float den = 1.0f + c3 * Lm1;
    return powf(num / den, m2);
}

static void rec709_to_rec2020_linear(float r, float g, float b, float *outR, float *outG, float *outB) {
    /* Standard BT.709 -> BT.2020 primary conversion matrix (linear light, D65). */
    *outR = 0.6274040f * r + 0.3292820f * g + 0.0433136f * b;
    *outG = 0.0690970f * r + 0.9195400f * g + 0.0113612f * b;
    *outB = 0.0163916f * r + 0.0880132f * g + 0.8955950f * b;
}

static void unpack_r10g10b10a2_to_bgra8(const uint8_t *data, size_t npix, uint8_t *out_bgra) {
    const uint32_t *src = (const uint32_t *)data;
    uint32_t *dst = (uint32_t *)out_bgra;
    for (size_t i = 0; i < npix; i++) {
        uint32_t px = src[i];
        uint32_t r10 = px & 0x3FF;
        uint32_t g10 = (px >> 10) & 0x3FF;
        uint32_t b10 = (px >> 20) & 0x3FF;
        uint8_t r8 = (uint8_t)(r10 >> 2);
        uint8_t g8 = (uint8_t)(g10 >> 2);
        uint8_t b8 = (uint8_t)(b10 >> 2);
        dst[i] = (0xFFu << 24) | ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | b8;
    }
}

typedef struct { HBITMAP hbmp; HDC hdc; uint8_t *bits; int w, h; } Grab;

static int grab_primary_monitor(Grab *g) {
    ZeroMemory(g, sizeof *g);
    g->w = GetSystemMetrics(SM_CXSCREEN); g->h = GetSystemMetrics(SM_CYSCREEN);
    if (g->w <= 0 || g->h <= 0) return 0;
    HDC sdc = GetDC(NULL);
    if (!sdc) return 0;
    g->hdc = CreateCompatibleDC(sdc);
    if (!g->hdc) { ReleaseDC(NULL, sdc); return 0; }
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g->w; bi.bmiHeader.biHeight = -g->h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    g->hbmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&g->bits, NULL, 0);
    if (!g->hbmp) { DeleteDC(g->hdc); ReleaseDC(NULL, sdc); return 0; }
    SelectObject(g->hdc, g->hbmp);
    if (!BitBlt(g->hdc, 0, 0, g->w, g->h, sdc, 0, 0, SRCCOPY)) { ReleaseDC(NULL, sdc); return 0; }
    GdiFlush(); ReleaseDC(NULL, sdc);
    return 1;
}

static void free_grab(Grab *g) {
    if (g->hdc) DeleteDC(g->hdc);
    if (g->hbmp) DeleteObject(g->hbmp);
    ZeroMemory(g, sizeof *g);
}

static int encode_jxl_sdr(const uint8_t *bgra, int w, int h, int lossless, float distance, uint8_t **out_buf, size_t *out_size) {
    int ok = 0; 
    uint8_t *rgb = NULL, *buf = NULL;
    JxlEncoder *enc = JxlEncoderCreate(NULL);
    if (!enc) return 0;

    JxlBasicInfo info; 
    JxlColorEncoding ce; 
    JxlEncoderFrameSettings *fs = NULL;
    size_t npix = 0, cap = 0, used = 0, avail = 0;
    uint8_t *next = NULL;
    JxlPixelFormat fmt;
    const uint32_t *src = NULL;
    uint8_t *dst = NULL;
    size_t i = 0;

    JxlEncoderInitBasicInfo(&info);
    info.xsize = w; info.ysize = h; info.bits_per_sample = 8; info.num_color_channels = 3;
    info.uses_original_profile = lossless ? JXL_TRUE : JXL_FALSE;
    if (JxlEncoderSetBasicInfo(enc, &info) != JXL_ENC_SUCCESS) goto done;
    
    JxlColorEncodingSetToSRGB(&ce, JXL_FALSE);
    if (JxlEncoderSetColorEncoding(enc, &ce) != JXL_ENC_SUCCESS) goto done;

    fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    if (!fs) goto done;
    if (lossless) { JxlEncoderSetFrameLossless(fs, JXL_TRUE); JxlEncoderSetFrameDistance(fs, 0.0); }
    else { JxlEncoderSetFrameDistance(fs, (double)distance); }

    npix = (size_t)w * h;
    rgb = (uint8_t *)malloc(npix * 3);
    if (!rgb) goto done;
    
    src = (const uint32_t *)bgra;
    dst = rgb;
    for (i = 0; i < npix; i++) {
        uint32_t pixel = src[i];
        dst[0] = (pixel >> 16) & 0xFF; dst[1] = (pixel >> 8) & 0xFF; dst[2] = pixel & 0xFF;
        dst += 3;
    }
    
    fmt.num_channels = 3;
    fmt.data_type = JXL_TYPE_UINT8;
    fmt.endianness = JXL_NATIVE_ENDIAN;
    fmt.align = 0;
    
    if (JxlEncoderAddImageFrame(fs, &fmt, rgb, npix * 3) != JXL_ENC_SUCCESS) goto done;
    JxlEncoderCloseInput(enc);
    
    cap = (size_t)w * h; if (cap < (4 << 20)) cap = (4 << 20);
    buf = (uint8_t *)malloc(cap);
    if (!buf) goto done;
    
    next = buf; avail = cap;
    for (;;) {
        JxlEncoderStatus st = JxlEncoderProcessOutput(enc, &next, &avail);
        if (st == JXL_ENC_SUCCESS) break;
        if (st == JXL_ENC_NEED_MORE_OUTPUT) {
            used = (size_t)(next - buf); cap *= 2;
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
    if (!encode_jxl_sdr(bgra, w, h, lossless, distance, &buf, &size)) return 0;
    int ok = 0; FILE *f = _wfopen(path, L"wb");
    if (f) {
        size_t written = fwrite(buf, 1, size, f);
        ok = (written == size); fclose(f);
    }
    free(buf); return ok;
}

typedef struct { uint8_t *bits; int w, h; DXGI_FORMAT format; } GrabDXGI;

static int grab_screen_dxgi(GrabDXGI *g) {
    ZeroMemory(g, sizeof *g);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGIOutput* output = nullptr;
    IDXGIOutput1* output1 = nullptr;
    IDXGIOutputDuplication* dup = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    IDXGIResource* desktopResource = nullptr;
    ID3D11Texture2D* tex = nullptr;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_TEXTURE2D_DESC stagingDesc;
    ID3D11Texture2D* stagingTex = nullptr;
    D3D11_MAPPED_SUBRESOURCE mapped;
    size_t pixel_size = 0, row_pitch = 0;
    int y = 0;

    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) goto fail;
    if (FAILED(factory->EnumAdapters1(0, &adapter))) goto fail;
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, NULL, &context))) goto fail;
    if (FAILED(adapter->EnumOutputs(0, &output))) goto fail;
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(&output1)))) goto fail;
    if (FAILED(output1->DuplicateOutput(device, &dup))) goto fail;
    if (FAILED(dup->AcquireNextFrame(1000, &frameInfo, &desktopResource))) goto fail;
    if (FAILED(desktopResource->QueryInterface(IID_PPV_ARGS(&tex)))) goto fail_release_frame;

    tex->GetDesc(&desc);
    g->w = desc.Width; g->h = desc.Height; g->format = desc.Format;

    stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    if (FAILED(device->CreateTexture2D(&stagingDesc, NULL, &stagingTex))) goto fail_release_frame;

    context->CopyResource(stagingTex, tex);

    if (FAILED(context->Map(stagingTex, 0, D3D11_MAP_READ, 0, &mapped))) goto fail_release_staging;

    pixel_size = (g->format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 : 4;
    row_pitch = g->w * pixel_size;
    g->bits = (uint8_t*)malloc(g->h * row_pitch);
    if (!g->bits) goto fail_release_staging;
    
    for (y = 0; y < g->h; y++) {
        memcpy(g->bits + y * row_pitch, (uint8_t*)mapped.pData + y * mapped.RowPitch, row_pitch);
    }
    context->Unmap(stagingTex, 0);

fail_release_staging:
    if (stagingTex) stagingTex->Release();
fail_release_frame:
    if (tex) tex->Release();
    if (desktopResource) desktopResource->Release();
    if (dup) dup->ReleaseFrame();
fail:
    if (dup) dup->Release();
    if (output1) output1->Release();
    if (output) output->Release();
    if (context) context->Release();
    if (device) device->Release();
    if (adapter) adapter->Release();
    if (factory) factory->Release();
    CoUninitialize();
    return g->bits != nullptr;
}

static void free_grab_dxgi(GrabDXGI *g) {
    if (g->bits) free(g->bits);
    ZeroMemory(g, sizeof *g);
}

static int save_dxgi_as_jxl(const uint8_t *data, int w, int h, DXGI_FORMAT fmt, int lossless, float distance, int force_sdr, const wchar_t *path) {
    /* Only DXGI_FORMAT_R16G16B16A16_FLOAT (scRGB) is genuine HDR data from
     * Desktop Duplication. R10G10B10A2_UNORM has no PQ/HDR metadata attached
     * to it in this API - treating it as PQ was producing corrupt colors. */
    int is_hdr = !force_sdr && (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT);

    if (!is_hdr) {
        if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM) {
            /* 10-bit desktop, not HDR: unpack to 8bpc and encode normally. */
            size_t npix = (size_t)w * h;
            uint8_t *bgra8 = (uint8_t *)malloc(npix * 4);
            if (!bgra8) return 0;
            unpack_r10g10b10a2_to_bgra8(data, npix, bgra8);
            int ok = save_bgra_as_jxl(bgra8, w, h, lossless, distance, path);
            free(bgra8);
            return ok;
        }
        if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            /* HDR desktop but caller asked for ForceSDR: clip scRGB to
             * [0,1] (i.e. to the 80-nit SDR range) and gamma-encode. */
            size_t npix = (size_t)w * h;
            uint8_t *bgra8 = (uint8_t *)malloc(npix * 4);
            if (!bgra8) return 0;
            const uint16_t *src16 = (const uint16_t *)data;
            uint8_t *dst8 = bgra8;
            for (size_t i = 0; i < npix; i++) {
                float r = half_to_float(src16[2]);
                float g = half_to_float(src16[1]);
                float b = half_to_float(src16[0]);
                if (r < 0.0f) r = 0.0f; if (r > 1.0f) r = 1.0f;
                if (g < 0.0f) g = 0.0f; if (g > 1.0f) g = 1.0f;
                if (b < 0.0f) b = 0.0f; if (b > 1.0f) b = 1.0f;
                float rs = (r <= 0.0031308f) ? (r * 12.92f) : (1.055f * powf(r, 1.0f / 2.4f) - 0.055f);
                float gs = (g <= 0.0031308f) ? (g * 12.92f) : (1.055f * powf(g, 1.0f / 2.4f) - 0.055f);
                float bs = (b <= 0.0031308f) ? (b * 12.92f) : (1.055f * powf(b, 1.0f / 2.4f) - 0.055f);
                dst8[0] = (uint8_t)(bs * 255.0f + 0.5f);
                dst8[1] = (uint8_t)(gs * 255.0f + 0.5f);
                dst8[2] = (uint8_t)(rs * 255.0f + 0.5f);
                dst8[3] = 0xFF;
                src16 += 4; dst8 += 4;
            }
            int ok = save_bgra_as_jxl(bgra8, w, h, lossless, distance, path);
            free(bgra8);
            return ok;
        }
        /* Plain BGRA8 desktop. */
        return save_bgra_as_jxl(data, w, h, lossless, distance, path);
    }

    /* ---- True HDR path: scRGB linear -> Rec.2020 PQ ---- */
    int ok = 0; 
    uint8_t *buf = NULL;
    JxlEncoder *enc = JxlEncoderCreate(NULL);
    if (!enc) return 0;

    JxlBasicInfo info; 
    JxlColorEncoding ce; 
    JxlEncoderFrameSettings *fs = NULL;
    size_t npix = 0, cap = 0, used = 0, avail = 0;
    uint8_t *next = NULL;
    JxlPixelFormat jxl_fmt;
    float *rgb = NULL;
    float *dst = NULL;
    size_t i = 0;
    FILE *f = NULL;

    JxlEncoderInitBasicInfo(&info);
    info.xsize = w; info.ysize = h; info.num_color_channels = 3;
    info.bits_per_sample = 32; info.exponent_bits_per_sample = 8;
    if (JxlEncoderSetBasicInfo(enc, &info) != JXL_ENC_SUCCESS) goto done;
    
    JxlColorEncodingSetToSRGB(&ce, JXL_FALSE);
    ce.color_space = JXL_COLOR_SPACE_RGB;
    ce.white_point = JXL_WHITE_POINT_D65;
    ce.primaries = JXL_PRIMARIES_2100;
    ce.transfer_function = JXL_TRANSFER_FUNCTION_PQ;
    if (JxlEncoderSetColorEncoding(enc, &ce) != JXL_ENC_SUCCESS) goto done;

    fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    if (!fs) goto done;
    JxlEncoderSetFrameDistance(fs, (double)(distance < 0.0f ? 0.0f : distance));

    npix = (size_t)w * h;
    rgb = (float *)malloc(npix * 3 * sizeof(float));
    if (!rgb) goto done;
    
    dst = rgb;
    {
        const uint16_t *src = (const uint16_t *)data;
        const float scale = SCRGB_REFERENCE_WHITE_NITS / PQ_MAX_NITS; /* 80/10000 */
        for (i = 0; i < npix; i++) {
            /* DXGI channel order in memory is B,G,R,A (little-endian halves). */
            float r_lin = half_to_float(src[2]);
            float g_lin = half_to_float(src[1]);
            float b_lin = half_to_float(src[0]);

            float r2020, g2020, b2020;
            rec709_to_rec2020_linear(r_lin, g_lin, b_lin, &r2020, &g2020, &b2020);

            /* Clamp out-of-gamut negatives introduced by the primary matrix
             * or by scRGB's extended range before applying the PQ OETF. */
            if (r2020 < 0.0f) r2020 = 0.0f;
            if (g2020 < 0.0f) g2020 = 0.0f;
            if (b2020 < 0.0f) b2020 = 0.0f;

            dst[0] = pq_oetf(r2020 * scale);
            dst[1] = pq_oetf(g2020 * scale);
            dst[2] = pq_oetf(b2020 * scale);
            src += 4; dst += 3;
        }
    }

    jxl_fmt.num_channels = 3;
    jxl_fmt.data_type = JXL_TYPE_FLOAT;
    jxl_fmt.endianness = JXL_NATIVE_ENDIAN;
    jxl_fmt.align = 0;

    if (JxlEncoderAddImageFrame(fs, &jxl_fmt, rgb, npix * 3 * sizeof(float)) != JXL_ENC_SUCCESS) { free(rgb); goto done; }
    free(rgb);
    JxlEncoderCloseInput(enc);
    
    cap = (size_t)w * h * 4; if (cap < (8 << 20)) cap = (8 << 20);
    buf = (uint8_t *)malloc(cap);
    if (!buf) goto done;
    
    next = buf; avail = cap;
    for (;;) {
        JxlEncoderStatus st = JxlEncoderProcessOutput(enc, &next, &avail);
        if (st == JXL_ENC_SUCCESS) break;
        if (st == JXL_ENC_NEED_MORE_OUTPUT) {
            used = (size_t)(next - buf); cap *= 2;
            uint8_t *nb = (uint8_t *)realloc(buf, cap);
            if (!nb) { free(buf); buf = NULL; goto done; }
            buf = nb; next = buf + used; avail = cap - used; continue;
        }
        free(buf); buf = NULL; goto done;
    }
    
    f = _wfopen(path, L"wb");
    if (f) {
        size_t written = fwrite(buf, 1, cap - avail, f);
        ok = (written == cap - avail);
        fclose(f);
    }
done:
    free(buf); JxlEncoderDestroy(enc);
    return ok;
}

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

    GrabDXGI g_dxgi;
    if (grab_screen_dxgi(&g_dxgi)) {
        int is_hdr = !g_cfg.force_sdr && (g_dxgi.format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        wchar_t out_path[MAX_PATH]; 
        build_out_path(out_path, MAX_PATH, is_hdr);
        int rc = save_dxgi_as_jxl(g_dxgi.bits, g_dxgi.w, g_dxgi.h, g_dxgi.format, g_cfg.lossless, g_cfg.distance, g_cfg.force_sdr, out_path) ? 0 : 1;
        free_grab_dxgi(&g_dxgi);
        return rc;
    }
    
    dbg("DXGI capture failed, falling back to legacy GDI");
    Grab g;
    if (!grab_primary_monitor(&g)) { free_grab(&g); return 1; }
    wchar_t out_path[MAX_PATH]; 
    build_out_path(out_path, MAX_PATH, 0);
    int rc = save_bgra_as_jxl(g.bits, g.w, g.h, g_cfg.lossless, g_cfg.distance, out_path) ? 0 : 1;
    free_grab(&g);
    return rc;
}
#endif
