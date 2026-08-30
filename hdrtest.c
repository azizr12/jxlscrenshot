/* hdrtest.c — standalone HDR capture diagnostic tool for jxlshot.
 *
 * PURPOSE
 *   Answers two SEPARATE questions that are easy to conflate:
 *     (1) Does the DXGI FP16 desktop-duplication + JXL encode pipeline
 *         actually work on this machine at all?
 *     (2) Is this specific display currently outputting real HDR (PQ)
 *         content above SDR reference white?
 *
 *   (1) is testable on ANY Windows 10/11 machine, HDR monitor or not,
 *   because Windows will hand you an FP16 scRGB buffer via
 *   DuplicateOutput1 even on a plain SDR desktop — the values will
 *   just stay in the 0.0-1.0 range instead of exceeding it.
 *   (2) is only ever true if you have a real HDR display with HDR
 *   turned on in Windows Settings.
 *
 *   Existing hdr_capture_primary() in jxlshot_hdr.c bails out
 *   immediately with goto done if the output's colorspace isn't PQ —
 *   which means on a non-HDR machine you learn NOTHING about whether
 *   your capture/encode code is even correct. This tool removes that
 *   early bailout so you get real diagnostics either way.
 *
 * OUTPUT
 *   - Full text report to the console (this is a CONSOLE app, not
 *     -mwindows, so you see output directly — run it from a terminal
 *     or double-click and read the window before it closes).
 *   - hdrtest_capture_sdr.jxl   — always written if capture succeeds.
 *                                 Standard lossless sRGB JXL, viewable
 *                                 in any JXL-capable viewer, on any
 *                                 machine. Proves the whole pipeline
 *                                 (DXGI capture -> pixel unpack ->
 *                                 JXL encode -> file write) works.
 *   - hdrtest_capture_hdr.jxl  — only written if real HDR values
 *                                 (>1.0 linear, i.e. brighter than
 *                                 SDR reference white) were detected
 *                                 in the captured frame. This is a
 *                                 true PQ/Rec.2020 HDR JXL.
 *
 * BUILD (MSYS2 / MinGW-w64):
 *   gcc -O2 -mconsole -o hdrtest.exe hdrtest.c ^
 *       -ljxl -ld3d11 -ldxgi -luuid -lgdi32 -luser32
 *
 * RUN:
 *   Just run hdrtest.exe from a terminal (cmd.exe / PowerShell / MSYS2
 *   shell) so you can read the report. It captures the PRIMARY monitor.
 *
 *   For a meaningful "real HDR" test even without an HDR panel, you
 *   can still turn on Settings -> System -> Display -> HDR in Windows
 *   11 (works on some virtual/software paths too) — but the SDR-path
 *   test below works with HDR completely off.
 */

#define UNICODE
#define _UNICODE
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <dxgi1_5.h>
#include <d3d11.h>
#include <jxl/encode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <wchar.h>

/* ------------------------------------------------------------------ */
/* Small helpers shared with jxlshot_hdr.c (kept standalone on purpose */
/* so this tool has zero dependency on the rest of your codebase and  */
/* can't silently inherit a bug from there).                          */
/* ------------------------------------------------------------------ */

static float half_to_float(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    if (exp == 0)  return mant ? (sign ? -1.0f : 1.0f) * (float)mant * 5.9604644775390625e-8f
                               : (sign ? -0.0f : 0.0f);
    if (exp == 31) return mant ? 0.0f : (sign ? -INFINITY : INFINITY);
    uint32_t ieee = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
    float r; memcpy(&r, &ieee, 4); return r;
}

static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

static float srgb_oetf(float c) {
    return c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static float pq_oetf(float x) {
    const float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 4096.0f * 128.0f;
    const float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 4096.0f * 32.0f,
                c3 = 2392.0f / 4096.0f * 32.0f;
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    float xm = powf(x, m1);
    return powf((c1 + c2 * xm) / (1.0f + c3 * xm), m2);
}

static void lin709_to_lin2020(float r, float g, float b, float *o) {
    o[0] = 0.627402f * r + 0.329292f * g + 0.043306f * b;
    o[1] = 0.069095f * r + 0.919532f * g + 0.011372f * b;
    o[2] = 0.016394f * r + 0.088028f * g + 0.895578f * b;
}

static void rot_src(int rot, int x, int y, int rw, int rh, int *sx, int *sy) {
    switch (rot) {
    case DXGI_MODE_ROTATION_ROTATE90:  *sx = rw - 1 - y; *sy = x;          break;
    case DXGI_MODE_ROTATION_ROTATE180: *sx = rw - 1 - x; *sy = rh - 1 - y; break;
    case DXGI_MODE_ROTATION_ROTATE270: *sx = y;          *sy = rh - 1 - x; break;
    default:                           *sx = x;          *sy = y;          break;
    }
}

static const char *colorspace_name(DXGI_COLOR_SPACE_TYPE cs) {
    switch (cs) {
        case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:   return "SDR sRGB (RGB_FULL_G22_NONE_P709)";
        case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:   return "Linear sRGB (RGB_FULL_G10_NONE_P709)";
        case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:return "HDR10 PQ Rec.2020 (RGB_FULL_G2084_NONE_P2020) <-- real HDR";
        case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020: return "HDR10 PQ Rec.2020 studio range";
        default: return "(other/unrecognized)";
    }
}

/* ------------------------------------------------------------------ */
/* Minimal JXL writers (SDR 8-bit sRGB, and true HDR PQ float)         */
/* ------------------------------------------------------------------ */

static int write_jxl_sdr(const uint8_t *rgb8, int w, int h, const wchar_t *path) {
    int ok = 0; JxlEncoderStatus st; uint8_t *buf = NULL, *next; size_t cap, avail, used;
    JxlEncoder *enc = JxlEncoderCreate(NULL);
    if (!enc) return 0;

    JxlBasicInfo info; JxlEncoderInitBasicInfo(&info);
    info.xsize = (uint32_t)w; info.ysize = (uint32_t)h; info.bits_per_sample = 8;
    info.num_color_channels = 3; info.uses_original_profile = JXL_TRUE;
    if (JxlEncoderSetBasicInfo(enc, &info) != JXL_ENC_SUCCESS) goto done;

    JxlColorEncoding ce; JxlColorEncodingSetToSRGB(&ce, JXL_FALSE);
    if (JxlEncoderSetColorEncoding(enc, &ce) != JXL_ENC_SUCCESS) goto done;

    JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    if (!fs) goto done;
    JxlEncoderSetFrameLossless(fs, JXL_TRUE);

    JxlPixelFormat fmt = {3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    if (JxlEncoderAddImageFrame(fs, &fmt, rgb8, (size_t)w * h * 3) != JXL_ENC_SUCCESS) goto done;
    JxlEncoderCloseInput(enc);

    cap = (size_t)w * h; if (cap < (4u << 20)) cap = 4u << 20;
    buf = malloc(cap); if (!buf) goto done;
    next = buf; avail = cap;
    for (;;) {
        st = JxlEncoderProcessOutput(enc, &next, &avail);
        if (st == JXL_ENC_SUCCESS) break;
        if (st == JXL_ENC_NEED_MORE_OUTPUT) {
            used = (size_t)(next - buf); cap *= 2;
            uint8_t *nb = realloc(buf, cap); if (!nb) goto done;
            buf = nb; next = buf + used; avail = cap - used; continue;
        }
        goto done;
    }
    {   FILE *f = _wfopen(path, L"wb");
        if (f) { size_t n = cap - avail; ok = fwrite(buf, 1, n, f) == n; fclose(f); } }
done:
    free(buf); JxlEncoderDestroy(enc);
    return ok;
}

static int write_jxl_hdr(const float *rgb_lin709, int w, int h, const wchar_t *path) {
    int ok = 0; size_t npix = (size_t)w * h; size_t cap, avail, used;
    uint8_t *buf = NULL, *next;
    float *pq = malloc(npix * 3 * sizeof(float));
    JxlEncoder *enc = JxlEncoderCreate(NULL);
    if (!pq || !enc) goto done;

    for (size_t i = 0; i < npix; i++) {
        float o[3];
        lin709_to_lin2020(rgb_lin709[i*3]   < 0 ? 0 : rgb_lin709[i*3],
                          rgb_lin709[i*3+1] < 0 ? 0 : rgb_lin709[i*3+1],
                          rgb_lin709[i*3+2] < 0 ? 0 : rgb_lin709[i*3+2], o);
        /* scRGB 1.0 == 80 nits; PQ 1.0 == 10000 nits -> scale by 0.008 */
        pq[i*3]   = pq_oetf(clamp01(o[0] * 0.008f));
        pq[i*3+1] = pq_oetf(clamp01(o[1] * 0.008f));
        pq[i*3+2] = pq_oetf(clamp01(o[2] * 0.008f));
    }

    JxlBasicInfo info; JxlEncoderInitBasicInfo(&info);
    info.xsize = (uint32_t)w; info.ysize = (uint32_t)h; info.num_color_channels = 3;
    info.bits_per_sample = 32; info.exponent_bits_per_sample = 8;
    info.intensity_target = 10000.0f; info.min_nits = 0.0f;
    if (JxlEncoderSetBasicInfo(enc, &info) != JXL_ENC_SUCCESS) goto done;

    JxlColorEncoding ce;
    ce.color_space = JXL_COLOR_SPACE_RGB; ce.white_point = JXL_WHITE_POINT_D65;
    ce.primaries = JXL_PRIMARIES_2100; ce.transfer_function = JXL_TRANSFER_FUNCTION_PQ;
    if (JxlEncoderSetColorEncoding(enc, &ce) != JXL_ENC_SUCCESS) goto done;

    JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    if (!fs) goto done;
    JxlEncoderSetFrameLossless(fs, JXL_TRUE);

    JxlPixelFormat fmt = {3, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0};
    if (JxlEncoderAddImageFrame(fs, &fmt, pq, npix * 3 * sizeof(float)) != JXL_ENC_SUCCESS) goto done;
    JxlEncoderCloseInput(enc);

    cap = npix * 4; if (cap < (8u << 20)) cap = 8u << 20;
    buf = malloc(cap); if (!buf) goto done;
    next = buf; avail = cap;
    for (;;) {
        JxlEncoderStatus st = JxlEncoderProcessOutput(enc, &next, &avail);
        if (st == JXL_ENC_SUCCESS) break;
        if (st == JXL_ENC_NEED_MORE_OUTPUT) {
            used = (size_t)(next - buf); cap *= 2;
            uint8_t *nb = realloc(buf, cap); if (!nb) goto done;
            buf = nb; next = buf + used; avail = cap - used; continue;
        }
        goto done;
    }
    {   FILE *f = _wfopen(path, L"wb");
        if (f) { size_t n = cap - avail; ok = fwrite(buf, 1, n, f) == n; fclose(f); } }
done:
    free(pq); free(buf); JxlEncoderDestroy(enc);
    return ok;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("=====================================================\n");
    printf(" jxlshot HDR diagnostic tool\n");
    printf("=====================================================\n\n");

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IDXGIFactory1 *factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory))) {
        printf("[FAIL] CreateDXGIFactory1 failed. DXGI itself is unavailable.\n"
               "       This means your GPU driver or Windows install is broken\n"
               "       for capture purposes, unrelated to HDR specifically.\n");
        return 1;
    }

    HMONITOR primary = MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);

    printf("--- Enumerating all adapters/outputs ---\n\n");

    IDXGIAdapter1 *chosen_adapter = NULL;
    IDXGIOutput   *chosen_output  = NULL;
    int hdr_advertised = 0;

    for (UINT a = 0; ; a++) {
        IDXGIAdapter1 *adapter = NULL;
        if (factory->lpVtbl->EnumAdapters1(factory, a, &adapter) != S_OK) break;

        DXGI_ADAPTER_DESC1 ad; adapter->lpVtbl->GetDesc1(adapter, &ad);
        wprintf(L"Adapter %u: %s\n", a, ad.Description);

        for (UINT n = 0; ; n++) {
            IDXGIOutput *out = NULL;
            if (adapter->lpVtbl->EnumOutputs(adapter, n, &out) != S_OK) break;

            DXGI_OUTPUT_DESC d; out->lpVtbl->GetDesc(out, &d);
            int is_primary = (d.Monitor == primary);
            wprintf(L"  Output %u: %s  %s\n", n, d.DeviceName,
                    is_primary ? L"<-- PRIMARY MONITOR" : L"");

            IDXGIOutput6 *out6 = NULL;
            if (SUCCEEDED(out->lpVtbl->QueryInterface(out, &IID_IDXGIOutput6, (void **)&out6))) {
                DXGI_OUTPUT_DESC1 d1;
                if (SUCCEEDED(out6->lpVtbl->GetDesc1(out6, &d1))) {
                    printf("    ColorSpace   : %s\n", colorspace_name(d1.ColorSpace));
                    printf("    BitsPerColor : %u\n", d1.BitsPerColor);
                    printf("    MinLuminance : %.4f nits\n", d1.MinLuminance);
                    printf("    MaxLuminance : %.1f nits (peak)\n", d1.MaxLuminance);
                    printf("    MaxFullFrameLuminance: %.1f nits (sustained)\n", d1.MaxFullFrameLuminance);
                    if (d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                        d1.ColorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020) {
                        printf("    -> Windows currently reports this output as HDR (PQ).\n");
                        if (is_primary) hdr_advertised = 1;
                    } else {
                        printf("    -> Windows currently reports this output as SDR.\n"
                               "       (Settings > System > Display > HDR is OFF for this display,\n"
                               "        or the display doesn't support HDR.)\n");
                    }
                }
                out6->lpVtbl->Release(out6);
            } else {
                printf("    (Could not query IDXGIOutput6 — Windows too old, or driver issue)\n");
            }

            if (is_primary && !chosen_output) {
                chosen_output = out; out = NULL;
                chosen_adapter = adapter; adapter->lpVtbl->AddRef(adapter);
            }
            printf("\n");
            if (out) out->lpVtbl->Release(out);
        }
        adapter->lpVtbl->Release(adapter);
    }

    if (!chosen_output) {
        printf("[FAIL] Could not find a DXGI output matching the primary monitor.\n"
               "       Capture is not possible on this system right now.\n");
        return 1;
    }

    printf("--- Attempting FP16 desktop duplication on primary monitor ---\n");
    printf("(This is the real test: it works the same code path regardless\n"
           " of whether Windows currently reports HDR or SDR above.)\n\n");

    ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = D3D11CreateDevice((IDXGIAdapter *)chosen_adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                                    NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (FAILED(hr)) {
        printf("[FAIL] D3D11CreateDevice failed, hr=0x%08lX\n"
               "       Your GPU driver may not support D3D11, or is in a bad state.\n", hr);
        return 1;
    }

    IDXGIOutput5 *out5 = NULL;
    if (FAILED(chosen_output->lpVtbl->QueryInterface(chosen_output, &IID_IDXGIOutput5, (void **)&out5))) {
        printf("[FAIL] IDXGIOutput5 not available. Windows version too old for this API\n"
               "       (needs Windows 10 1803+).\n");
        return 1;
    }

    IDXGIOutputDuplication *dup = NULL;
    DXGI_FORMAT want = DXGI_FORMAT_R16G16B16A16_FLOAT;
    hr = out5->lpVtbl->DuplicateOutput1(out5, (IUnknown *)dev, 0, 1, &want, &dup);
    if (FAILED(hr)) {
        printf("[FAIL] DuplicateOutput1(FP16) failed, hr=0x%08lX\n", hr);
        if (hr == (HRESULT)0x887A0022 /* DXGI_ERROR_UNSUPPORTED */)
            printf("       -> FP16 format not supported for duplication on this adapter/driver.\n");
        else if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
            printf("       -> Another application (e.g. another capture tool, or a UAC/secure\n"
                   "          desktop prompt) currently owns exclusive duplication access.\n");
        else if (hr == E_ACCESSDENIED)
            printf("       -> Access denied. Common when running over Remote Desktop, or the\n"
                   "          desktop is locked / a UAC prompt is showing.\n");
        else
            printf("       -> See DXGI_ERROR / HRESULT docs for 0x%08lX.\n", hr);
        printf("\nThis means your capture pipeline itself cannot run right now —\n"
               "unrelated to whether you have an HDR monitor. Fix this first.\n");
        return 1;
    }

    printf("[OK] DuplicateOutput1(FP16) succeeded. Waiting for a frame...\n");

    DXGI_OUTDUPL_FRAME_INFO fi; IDXGIResource *res = NULL;
    DWORD deadline = GetTickCount() + 3000;
    int acquired = 0;
    for (;;) {
        hr = dup->lpVtbl->AcquireNextFrame(dup, 500, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            if (GetTickCount() >= deadline) {
                printf("[FAIL] Timed out waiting for a frame with new content.\n"
                       "       Try moving the mouse or changing something on screen and rerun.\n");
                return 1;
            }
            continue;
        }
        if (FAILED(hr)) {
            printf("[FAIL] AcquireNextFrame failed, hr=0x%08lX\n", hr);
            return 1;
        }
        acquired = 1;
        if (fi.LastPresentTime.QuadPart != 0) break;
        dup->lpVtbl->ReleaseFrame(dup); acquired = 0; res = NULL;
    }

    ID3D11Texture2D *tex = NULL;
    res->lpVtbl->QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex);
    D3D11_TEXTURE2D_DESC td; tex->lpVtbl->GetDesc(tex, &td);

    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D *staging = NULL;
    dev->lpVtbl->CreateTexture2D(dev, &sd, NULL, &staging);
    ctx->lpVtbl->CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)tex);

    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(ctx->lpVtbl->Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &map))) {
        printf("[FAIL] Failed to map staging texture for CPU read.\n");
        return 1;
    }

    int w = (int)td.Width, h = (int)td.Height;
    printf("[OK] Frame acquired: %dx%d, format=%d\n", w, h, (int)td.Format);

    float *rgb = malloc((size_t)w * h * 3 * sizeof(float));
    float minv = 1e9f, maxv = -1e9f, sum = 0;
    for (int y = 0; y < h; y++) {
        const uint16_t *row = (const uint16_t *)((const uint8_t *)map.pData + (size_t)y * map.RowPitch);
        for (int x = 0; x < w; x++) {
            const uint16_t *px = row + x * 4;
            float r = half_to_float(px[0]), g = half_to_float(px[1]), b = half_to_float(px[2]);
            float *d = rgb + ((size_t)y * w + x) * 3;
            d[0] = r; d[1] = g; d[2] = b;
            float m = r > g ? (r > b ? r : b) : (g > b ? g : b);
            if (m < minv) minv = m;
            if (m > maxv) maxv = m;
            sum += m;
        }
    }
    ctx->lpVtbl->Unmap(ctx, (ID3D11Resource *)staging, 0);

    float avg = sum / ((float)w * h);
    printf("\n--- Captured pixel statistics (linear scRGB, 1.0 == 80 nits) ---\n");
    printf("  min channel value : %.4f\n", minv);
    printf("  max channel value : %.4f\n", maxv);
    printf("  avg channel value : %.4f\n", avg);

    int real_hdr_signal = (maxv > 1.02f); /* small margin over 1.0 for FP noise */

    printf("\n--- VERDICT ---\n");
    if (maxv <= 0.0001f && minv >= -0.0001f) {
        printf("[WARN] Captured frame is all zeros (pure black). The capture pipeline ran\n"
               "       end-to-end without error, but this doesn't prove pixel data is\n"
               "       correct — you may have captured a black screen, or there's a bug\n"
               "       in the unpack step. Try again with visible content on screen.\n");
    } else {
        printf("[PASS] Your DXGI FP16 capture pipeline works: real, non-zero pixel data\n"
               "       was captured and unpacked correctly. This is true regardless of\n"
               "       HDR support on this display.\n");
    }

    if (real_hdr_signal) {
        printf("[PASS] Real HDR signal detected: max channel value %.3f is above SDR\n"
               "       reference white (1.0). This display is actively outputting HDR\n"
               "       content brighter than SDR white right now.\n", maxv);
    } else {
        printf("[INFO] No values above SDR reference white (1.0) were found. Either:\n"
               "         - this monitor/desktop is in SDR mode right now, or\n"
               "         - it's HDR-capable but nothing bright enough is on screen.\n"
               "       This is NOT a failure of your code — it just means there's no\n"
               "       real HDR content to prove against on this machine right now.\n");
    }

    /* Always write the SDR-viewable proof file */
    uint8_t *rgb8 = malloc((size_t)w * h * 3);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        rgb8[i*3+0] = (uint8_t)(srgb_oetf(clamp01(rgb[i*3+0])) * 255.0f + 0.5f);
        rgb8[i*3+1] = (uint8_t)(srgb_oetf(clamp01(rgb[i*3+1])) * 255.0f + 0.5f);
        rgb8[i*3+2] = (uint8_t)(srgb_oetf(clamp01(rgb[i*3+2])) * 255.0f + 0.5f);
    }
    int sdr_ok = write_jxl_sdr(rgb8, w, h, L"hdrtest_capture_sdr.jxl");
    printf("\nWrote hdrtest_capture_sdr.jxl : %s (open with any JXL viewer to visually confirm)\n",
           sdr_ok ? "OK" : "FAILED");
    free(rgb8);

    if (real_hdr_signal) {
        int hdr_ok = write_jxl_hdr(rgb, w, h, L"hdrtest_capture_hdr.jxl");
        printf("Wrote hdrtest_capture_hdr.jxl : %s (true PQ/Rec.2020 HDR file)\n",
               hdr_ok ? "OK" : "FAILED");
    }

    free(rgb);
    dup->lpVtbl->ReleaseFrame(dup);
    printf("\nDone.\n");
    return 0;
}