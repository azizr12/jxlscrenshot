
/*
* Captures the primary monitor and saves it as JPEG XL (.jxl).
* Supports both SDR (8-bit) and HDR (16-bit float) via DXGI Desktop Duplication.
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
* Build (MSYS2 / MinGW-w64) - Optimized for size:
*   gcc -Os -s -flto -ffunction-sections -fdata-sections -Wl,--gc-sections \
*       -mwindows -o jxlshot.exe jxlshot.c -ld3d11 -ldxgi -ljxl -ljxl_threads -lgdi32 -luser32 -lshell32 -lole32
*/
#define UNICODE
#define _UNICODE
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00 // Windows 10+ for IDXGIOutput6
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

#define COBJMACROS
#include <d3d11.h>
#include <dxgi1_6.h>
#include <jxl/encode.h>
#include <jxl/color_encoding.h>

#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "jxl.lib")
#pragma comment(lib, "jxl_threads.lib")
#endif

/* ------------------------------------------------------------------ */
/* HDR Screen Capture using DXGI Desktop Duplication                  */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t* bits;
    int w, h;
    int is_hdr; // 1 if HDR (16-bit float), 0 if SDR (8-bit)
} GrabHDR;

static int grab_hdr_monitor(GrabHDR* g) {
    ZeroMemory(g, sizeof(GrabHDR));
    
    ID3D11Device* device = NULL;
    ID3D11DeviceContext* context = NULL;
    IDXGIFactory1* factory = NULL;
    IDXGIAdapter1* adapter = NULL;
    IDXGIOutput* output = NULL;
    IDXGIOutput1* output1 = NULL;
    IDXGIOutput6* output6 = NULL;
    IDXGIOutputDuplication* dup = NULL;
    IDXGIResource* resource = NULL;
    ID3D11Texture2D* texture = NULL;
    ID3D11Texture2D* staging_texture = NULL;
    IDXGIDevice* dxgi_device = NULL;

    // 1. Create D3D11 Device
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, 
                                  D3D11_SDK_VERSION, &device, NULL, &context))) {
        return 0;
    }

    // 2. Get DXGI Factory and Adapter
    if (FAILED(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void**)&dxgi_device))) goto cleanup;
    if (FAILED(IDXGIDevice_GetParent(dxgi_device, &IID_IDXGIAdapter1, (void**)&adapter))) goto cleanup;
    if (FAILED(IDXGIAdapter1_GetParent(adapter, &IID_IDXGIFactory1, (void**)&factory))) goto cleanup;
    if (FAILED(IDXGIAdapter1_EnumOutputs(adapter, 0, &output))) goto cleanup;

    // 3. Check for HDR support (requires IDXGIOutput6)
    DXGI_OUTPUT_DESC1 desc1;
    int is_hdr = 0;
    if (SUCCEEDED(IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput6, (void**)&output6))) {
        IDXGIOutput6_GetDesc1(output6, &desc1);
        if (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
            is_hdr = 1;
        }
    }

    // 4. Create Desktop Duplication
    if (FAILED(IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1, (void**)&output1))) goto cleanup;
    if (FAILED(IDXGIOutput1_DuplicateOutput(output1, (IUnknown*)device, &dup))) goto cleanup;

    // 5. Acquire Next Frame
    DXGI_OUTDUPL_FRAME_INFO frame_info;
    if (FAILED(IDXGIOutputDuplication_AcquireNextFrame(dup, 1000, &frame_info, &resource))) {
        goto cleanup;
    }

    if (FAILED(IDXGIResource_QueryInterface(resource, &IID_ID3D11Texture2D, (void**)&texture))) {
        goto cleanup_frame;
    }

    D3D11_TEXTURE2D_DESC tex_desc;
    ID3D11Texture2D_GetDesc(texture, &tex_desc);
    g->w = tex_desc.Width;
    g->h = tex_desc.Height;
    g->is_hdr = is_hdr;

    // 6. Create Staging Texture for CPU Read
    D3D11_TEXTURE2D_DESC staging_desc = tex_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;

    if (FAILED(ID3D11Device_CreateTexture2D(device, &staging_desc, NULL, &staging_texture))) {
        goto cleanup_frame;
    }

    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource*)staging_texture, (ID3D11Resource*)texture);

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ID3D11DeviceContext_Map(context, (ID3D11Resource*)staging_texture, 0, D3D11_MAP_READ, 0, &mapped))) {
        size_t pixel_size = is_hdr ? 8 : 4; // 8 bytes for R16G16B16A16_FLOAT, 4 bytes for R8G8B8A8
        size_t total_size = (size_t)g->w * g->h * pixel_size;
        g->bits = (uint8_t*)malloc(total_size);
        
        if (g->bits) {
            // Copy row by row to handle pitch/stride correctly
            for (int y = 0; y < g->h; y++) {
                memcpy(g->bits + (y * g->w * pixel_size), 
                       (uint8_t*)mapped.pData + (y * mapped.RowPitch), 
                       g->w * pixel_size);
            }
        }
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource*)staging_texture, 0);
    }

cleanup_frame:
    if (dup) IDXGIOutputDuplication_ReleaseFrame(dup);
cleanup:
    if (staging_texture) ID3D11Texture2D_Release(staging_texture);
    if (texture) ID3D11Texture2D_Release(texture);
    if (resource) IDXGIResource_Release(resource);
    if (dup) IDXGIOutputDuplication_Release(dup);
    if (output1) IDXGIOutput1_Release(output1);
    if (output6) IDXGIOutput6_Release(output6);
    if (output) IDXGIOutput_Release(output);
    if (adapter) IDXGIAdapter1_Release(adapter);
    if (factory) IDXGIFactory1_Release(factory);
    if (dxgi_device) IDXGIDevice_Release(dxgi_device);
    if (context) ID3D11DeviceContext_Release(context);
    if (device) ID3D11Device_Release(device);

    return (g->bits != NULL) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* JPEG XL Encoding (HDR/SDR Aware)                                   */
/* ------------------------------------------------------------------ */
static int encode_jxl(const uint8_t* bits, int w, int h, int is_hdr, int lossless, float distance, uint8_t** out_buf, size_t* out_size) {
    int ok = 0;
    uint8_t* rgb_data = NULL;
    uint8_t* buf = NULL;
    JxlEncoderStatus st;
    JxlEncoder* enc = JxlEncoderCreate(NULL);
    if (!enc) return 0;

    JxlEncoderFrameSettings* frame_settings = JxlEncoderFrameSettingsCreate(enc, NULL);
    if (!frame_settings) goto done;

    if (lossless) {
        JxlEncoderSetFrameLossless(frame_settings, JXL_TRUE);
        JxlEncoderSetFrameDistance(frame_settings, 0.0f);
    } else {
        JxlEncoderSetFrameDistance(frame_settings, distance);
    }

    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = w;
    info.ysize = h;
    
    if (is_hdr) {
        info.bits_per_sample = 16;
        info.exponent_bits_per_sample = 5; // Half-precision float
    } else {
        info.bits_per_sample = 8;
        info.exponent_bits_per_sample = 0;
    }
    
    info.num_color_channels = 3;
    info.alpha_bits = 0;
    info.uses_original_profile = lossless ? JXL_TRUE : JXL_FALSE;

    if (JxlEncoderSetBasicInfo(enc, &info) != JXL_ENC_SUCCESS) goto done;

    JxlColorEncoding ce;
    JxlColorEncodingSetToSRGB(&ce, JXL_FALSE);
    
    if (is_hdr) {
        ce.color_space = JXL_COLOR_SPACE_RGB;
        ce.white_point = JXL_WHITE_POINT_D65;
        ce.primaries = JXL_PRIMARIES_2100;           // Rec. 2020
        ce.transfer_function = JXL_TRANSFER_FUNCTION_PQ; // SMPTE ST 2084 (HDR)
        ce.rendering_intent = JXL_RENDERING_INTENT_PERCEPTUAL;
    } else {
        ce.color_space = JXL_COLOR_SPACE_RGB;
        ce.white_point = JXL_WHITE_POINT_D65;
        ce.primaries = JXL_PRIMARIES_SRGB;
        ce.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
        ce.rendering_intent = JXL_RENDERING_INTENT_PERCEPTUAL;
    }

    if (JxlEncoderSetColorEncoding(enc, &ce) != JXL_ENC_SUCCESS) goto done;

    // Convert 4-channel (RGBA) source to 3-channel (RGB) to save space and match JxlPixelFormat
    size_t dst_stride = is_hdr ? 6 : 3;
    size_t total_dst_size = (size_t)w * h * dst_stride;
    rgb_data = (uint8_t*)malloc(total_dst_size);
    if (!rgb_data) goto done;

    if (is_hdr) {
        const uint16_t* src = (const uint16_t*)bits;
        uint16_t* dst = (uint16_t*)rgb_data;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            dst[0] = src[0]; // R
            dst[1] = src[1]; // G
            dst[2] = src[2]; // B
            src += 4;
            dst += 3;
        }
    } else {
        const uint32_t* src = (const uint32_t*)bits;
        uint8_t* dst = rgb_data;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint32_t pixel = src[i];
            dst[0] = (pixel >> 16) & 0xFF; // R
            dst[1] = (pixel >> 8)  & 0xFF; // G
            dst[2] = pixel & 0xFF;         // B
            dst += 3;
        }
    }
    
    JxlPixelFormat pixel_format = {
        3, // num_channels (RGB)
        is_hdr ? JXL_TYPE_FLOAT16 : JXL_TYPE_UINT8,
        JXL_LITTLE_ENDIAN,
        0
    };

    if (JxlEncoderAddImageFrame(frame_settings, &pixel_format, rgb_data, total_dst_size) != JXL_ENC_SUCCESS) {
        goto done;
    }

    JxlEncoderCloseInput(enc);

    size_t cap = total_dst_size; 
    if (cap < (4 << 20)) cap = (4 << 20); // Minimum 4MB buffer
    
    buf = (uint8_t*)malloc(cap);
    if (!buf) goto done;
    
    uint8_t* next_out = buf;
    size_t avail_out = cap;

    JxlEncoderStatus process_result = JXL_ENC_NEED_MORE_OUTPUT;
    while (process_result == JXL_ENC_NEED_MORE_OUTPUT) {
        process_result = JxlEncoderProcessOutput(enc, &next_out, &avail_out);
        if (process_result == JXL_ENC_NEED_MORE_OUTPUT) {
            size_t used = next_out - buf;
            cap *= 2;
            uint8_t* nb = (uint8_t*)realloc(buf, cap);
            if (!nb) goto done;
            buf = nb;
            next_out = buf + used;
            avail_out = cap - used;
        }
    }

    if (process_result == JXL_ENC_SUCCESS) {
        *out_size = next_out - buf;
        *out_buf = buf;
        buf = NULL; // Prevent freeing in done block
        ok = 1;
    }

done:
    free(rgb_data);
    free(buf);
    JxlEncoderDestroy(enc);
    return ok;
}

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
        FILE *f = _wfopen(ini_path, L"w");
        if (f) {
            fprintf(f, "[Capture]\n");
            fprintf(f, "Debug=0\n");
            fprintf(f, "Lossless=1\n");
            fprintf(f, "Distance=1.0\n");
            fprintf(f, "ShowCursor=1\n");
            fprintf(f, "ExportPath=\n");
            fprintf(f, "HotkeyFull=PrintScreen\n");
            fprintf(f, "HotkeyRegion=Ctrl+PrintScreen\n");
            fclose(f);
        }
    }
}

static void init_config(void) {
    wchar_t ini_path[MAX_PATH];
    _snwprintf(ini_path, MAX_PATH, L"%s\\jxlshot.ini", g_exe_dir);
    
    g_cfg.debug_enabled = 1; g_cfg.lossless = 1; g_cfg.distance = 1.0f; g_cfg.show_cursor = 1;
    g_cfg.hk_full_mod = 0; g_cfg.hk_full_vk = VK_SNAPSHOT;
    g_cfg.hk_region_mod = MOD_CONTROL; g_cfg.hk_region_vk = VK_SNAPSHOT;
    
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_MYPICTURES, NULL, SHGFP_TYPE_CURRENT, g_cfg.export_path))) {
        GetEnvironmentVariableW(L"USERPROFILE", g_cfg.export_path, MAX_PATH);
        wcscat_s(g_cfg.export_path, MAX_PATH, L"\\Pictures");
    }

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
static void build_out_path(wchar_t *path, int n) {
    SYSTEMTIME st; GetLocalTime(&st);
    _snwprintf(path, n, L"%s\\jxlshot_%04d%02d%02d_%02d%02d%02d_%03d.jxl",
               g_cfg.export_path, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    path[n - 1] = 0;
}

static void set_dpi_aware(void) {
    typedef BOOL (WINAPI *Fn)(HANDLE);
    Fn f = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (f) f((HANDLE)(LONG_PTR)-4); else SetProcessDPIAware();
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
    
    set_dpi_aware(); 
    init_paths(); 
    ensure_default_ini(); 
    init_config();
    
    if (cli_lossless != -1) g_cfg.lossless = cli_lossless;
    if (cli_distance >= 0.0f) g_cfg.distance = cli_distance;
    
    dbg_init();
    if (wait_ms) Sleep(wait_ms);

    GrabHDR g;
    if (!grab_hdr_monitor(&g)) { 
        dbg("Failed to capture monitor");
        if (g.bits) free(g.bits);
        return 1; 
    }
    
    dbg("Capture successful: %dx%d, HDR=%d", g.w, g.h, g.is_hdr);

    wchar_t out_path[MAX_PATH]; 
    build_out_path(out_path, MAX_PATH);
    
    uint8_t *buf = NULL; 
    size_t size = 0;
    int ok = encode_jxl(g.bits, g.w, g.h, g.is_hdr, g_cfg.lossless, g_cfg.distance, &buf, &size);
    
    if (ok) {
        FILE *f = _wfopen(out_path, L"wb");
        if (f) {
            size_t written = fwrite(buf, 1, size, f);
            if (written == size) {
                dbg("Saved successfully to: %ls", out_path);
            } else {
                dbg("Write failed");
                ok = 0;
            }
            fclose(f);
        } else {
            dbg("Failed to open file for writing: %ls", out_path);
            ok = 0;
        }
        free(buf);
    } else {
        dbg("Encoding failed");
    }
    
    free(g.bits);
    return ok ? 0 : 1;
}
#endif
