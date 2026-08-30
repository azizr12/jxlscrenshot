/* jxlshot_hdr.c — self-contained HDR capture/encode module for jxlshot.
 * Pure C (explicit COM vtable calls); builds with gcc/MinGW-w64.
 * Link: -ldxgi -ld3d11 -luuid -ljxl
 */
#include "jxlshot_hdr.h"

#include <dxgi1_5.h>
#include <d3d11.h>
#include <jxl/encode.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- color math ---------------- */

static float half_to_float(uint16_t h)
{
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    if (exp == 0)  return mant ? (sign ? -1.0f : 1.0f) * (float)mant * 5.9604644775390625e-8f
                               : (sign ? -0.0f : 0.0f);
    if (exp == 31) return mant ? 0.0f : (sign ? -INFINITY : INFINITY); /* NaN -> 0 */
    uint32_t ieee = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
    float r; memcpy(&r, &ieee, 4); return r;
}

static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

static float srgb_oetf(float c)
{
    return c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

/* ST.2084 PQ OETF; input normalized so 1.0 == 10000 nits */
static float pq_oetf(float x)
{
    const float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 4096.0f * 128.0f;
    const float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 4096.0f * 32.0f,
                c3 = 2392.0f / 4096.0f * 32.0f;
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    float xm = powf(x, m1);
    return powf((c1 + c2 * xm) / (1.0f + c3 * xm), m2);
}

/* linear Rec.709 (scRGB primaries) -> linear Rec.2020 */
static void lin709_to_lin2020(float r, float g, float b, float *o)
{
    o[0] = 0.627402f * r + 0.329292f * g + 0.043306f * b;
    o[1] = 0.069095f * r + 0.919532f * g + 0.011372f * b;
    o[2] = 0.016394f * r + 0.088028f * g + 0.895578f * b;
}

/* undo DXGI_MODE_ROTATION: map upright (x,y) back to raw (sx,sy) */
static void rot_src(int rot, int x, int y, int rw, int rh, int *sx, int *sy)
{
    switch (rot) {
    case DXGI_MODE_ROTATION_ROTATE90:  *sx = rw - 1 - y; *sy = x;          break;
    case DXGI_MODE_ROTATION_ROTATE180: *sx = rw - 1 - x; *sy = rh - 1 - y; break;
    case DXGI_MODE_ROTATION_ROTATE270: *sx = y;          *sy = rh - 1 - x; break;
    default:                           *sx = x;          *sy = y;          break;
    }
}

/* ---------------- capture ---------------- */

int hdr_capture_primary(hdr_frame_t *f, int timeout_ms)
{
    IDXGIFactory1 *factory = NULL; IDXGIAdapter1 *adapter = NULL;
    IDXGIOutput *output = NULL;    IDXGIOutput6 *output6 = NULL;
    IDXGIOutput5 *output5 = NULL;  IDXGIOutputDuplication *dup = NULL;
    IDXGIResource *res = NULL;     ID3D11Texture2D *tex = NULL, *staging = NULL;
    ID3D11Device *dev = NULL;      ID3D11DeviceContext *ctx = NULL;
    DXGI_OUTPUT_DESC1 odesc1;      DXGI_OUTDUPL_FRAME_INFO fi;
    D3D11_TEXTURE2D_DESC td, sd;   D3D11_MAPPED_SUBRESOURCE map;
    int ok = 0, acquired = 0;
    DWORD deadline = GetTickCount() + (DWORD)(timeout_ms > 0 ? timeout_ms : 1000);

    ZeroMemory(f, sizeof *f);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    HMONITOR target = MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory))) goto done;

    /* enumerate every adapter/output; select the one driving the monitor */
    for (UINT a = 0; !output && factory->lpVtbl->EnumAdapters1(factory, a, &adapter) == S_OK; a++) {
        IDXGIOutput *o = NULL;
        for (UINT n = 0; adapter->lpVtbl->EnumOutputs(adapter, n, &o) == S_OK; n++) {
            DXGI_OUTPUT_DESC d;
            o->lpVtbl->GetDesc(o, &d);
            if (d.Monitor == target) { output = o; break; }
            o->lpVtbl->Release(o); o = NULL;
        }
        if (!output) adapter->lpVtbl->Release(adapter), adapter = NULL;
    }
    if (!output) goto done;

    if (FAILED(output->lpVtbl->QueryInterface(output, &IID_IDXGIOutput6, (void **)&output6))) goto done;
    if (FAILED(output6->lpVtbl->GetDesc1(output6, &odesc1))) goto done;

    /* scoping rule: touch HDR (PQ) outputs only; SDR stays on GDI */
    if (odesc1.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) goto done;

    if (FAILED(D3D11CreateDevice((IDXGIAdapter *)adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                                 NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx))) goto done;
    if (FAILED(output->lpVtbl->QueryInterface(output, &IID_IDXGIOutput5, (void **)&output5))) goto done;

    {   /* request FP16 scRGB explicitly */
        DXGI_FORMAT want = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (FAILED(output5->lpVtbl->DuplicateOutput1(output5, (IUnknown *)dev, 0, 1, &want, &dup)))
            goto done;
    }

    /* wait for a frame that actually contains new content */
    for (;;) {
        HRESULT hr = dup->lpVtbl->AcquireNextFrame(dup, 500, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) { if (GetTickCount() >= deadline) goto done; continue; }
        if (FAILED(hr)) goto done;
        acquired = 1;
        if (fi.LastPresentTime.QuadPart != 0) break;
        dup->lpVtbl->ReleaseFrame(dup); acquired = 0; res = NULL;
    }

    if (FAILED(res->lpVtbl->QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex))) goto done;
    tex->lpVtbl->GetDesc(tex, &td);
    if (td.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) goto done;

    sd = td; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    if (FAILED(dev->lpVtbl->CreateTexture2D(dev, &sd, NULL, &staging))) goto done;
    ctx->lpVtbl->CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)tex);
    if (FAILED(ctx->lpVtbl->Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &map))) goto done;

    {   /* unpack FP16 -> float32 scRGB, applying rotation */
        int rot = (int)odesc1.Rotation;
        int rw = (int)td.Width, rh = (int)td.Height;
        int rot90 = (rot == DXGI_MODE_ROTATION_ROTATE90 || rot == DXGI_MODE_ROTATION_ROTATE270);
        int ow = rot90 ? rh : rw, oh = rot90 ? rw : rh;
        float *rgb = malloc((size_t)ow * oh * 3 * sizeof(float));
        if (rgb) {
            for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {
                int sx, sy; rot_src(rot, x, y, rw, rh, &sx, &sy);
                const uint16_t *px = (const uint16_t *)
                    ((const uint8_t *)map.pData + (size_t)sy * map.RowPitch) + (size_t)sx * 4;
                float *d = rgb + ((size_t)y * ow + x) * 3;
                d[0] = half_to_float(px[0]); d[1] = half_to_float(px[1]); d[2] = half_to_float(px[2]);
            }
            f->rgb = rgb; f->w = ow; f->h = oh; f->is_hdr = 1; ok = 1;
        }
        ctx->lpVtbl->Unmap(ctx, (ID3D11Resource *)staging, 0);
    }

done:
    if (staging)  staging->lpVtbl->Release(staging);
    if (tex)      tex->lpVtbl->Release(tex);
    if (res)      res->lpVtbl->Release(res);
    if (dup)      { if (acquired) dup->lpVtbl->ReleaseFrame(dup); dup->lpVtbl->Release(dup); }
    if (output5)  output5->lpVtbl->Release(output5);
    if (output6)  output6->lpVtbl->Release(output6);
    if (output)   output->lpVtbl->Release(output);
    if (ctx)      ctx->lpVtbl->Release(ctx);
    if (dev)      dev->lpVtbl->Release(dev);
    if (adapter)  adapter->lpVtbl->Release(adapter);
    if (factory)  factory->lpVtbl->Release(factory);
    CoUninitialize();
    return ok;
}

void hdr_frame_free(hdr_frame_t *f) { if (f->rgb) free(f->rgb); ZeroMemory(f, sizeof *f); }

/* ---------------- encode: true HDR master ---------------- */

int hdr_encode_jxl_hdr(const float *rgb, int w, int h,
                       int lossless, float distance, const wchar_t *path)
{
    int ok = 0; size_t npix = (size_t)w * h, cap = 0, avail = 0, used = 0;
    uint8_t *buf = NULL, *next = NULL;
    float *pq = malloc(npix * 3 * sizeof(float));
    JxlEncoder *enc = JxlEncoderCreate(NULL);
    if (!pq || !enc) goto done;

    /* scRGB linear (1.0 == 80 nits) -> Rec.2020 linear -> PQ (1.0 == 10000 nits) */
    for (size_t i = 0; i < npix; i++) {
        float o[3];
        lin709_to_lin2020(rgb[i*3]   < 0 ? 0 : rgb[i*3],
                          rgb[i*3+1] < 0 ? 0 : rgb[i*3+1],
                          rgb[i*3+2] < 0 ? 0 : rgb[i*3+2], o);
        pq[i*3]   = pq_oetf(clamp01(o[0] * 0.008f));
        pq[i*3+1] = pq_oetf(clamp01(o[1] * 0.008f));
        pq[i*3+2] = pq_oetf(clamp01(o[2] * 0.008f));
    }

    JxlBasicInfo info; JxlEncoderInitBasicInfo(&info);
    info.xsize = (uint32_t)w; info.ysize = (uint32_t)h;
    info.num_color_channels = 3;
    info.bits_per_sample = 32; info.exponent_bits_per_sample = 8;
    info.intensity_target = 10000.0f; info.min_nits = 0.0f;
    if (JxlEncoderSetBasicInfo(enc, &info) != JXL_ENC_SUCCESS) goto done;

    JxlColorEncoding ce;
    ce.color_space = JXL_COLOR_SPACE_RGB;
    ce.white_point = JXL_WHITE_POINT_D65;
    ce.primaries = JXL_PRIMARIES_2100;
    ce.transfer_function = JXL_TRANSFER_FUNCTION_PQ;
    if (JxlEncoderSetColorEncoding(enc, &ce) != JXL_ENC_SUCCESS) goto done;

    JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    if (!fs) goto done;
    if (lossless) JxlEncoderSetFrameLossless(fs, JXL_TRUE);
    else JxlEncoderSetFrameDistance(fs, (double)(distance < 0 ? 0 : distance));

    JxlPixelFormat fmt = { 3, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0 };
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
            uint8_t *nb = realloc(buf, cap);
            if (!nb) goto done;
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

/* ---------------- encode: OBS/Windows-style SDR tonemap ---------------- */

int hdr_tonemap_bgra(const float *rgb, int w, int h,
                     float sdr_white_nits, uint8_t **out_bgra)
{
    if (sdr_white_nits < 1.0f) sdr_white_nits = 80.0f;
    float scale = 80.0f / sdr_white_nits;
    uint8_t *b = malloc((size_t)w * h * 4);
    if (!b) return 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint8_t *d = b + i * 4;
        d[2] = (uint8_t)(srgb_oetf(clamp01(rgb[i*3]   * scale)) * 255.0f + 0.5f);
        d[1] = (uint8_t)(srgb_oetf(clamp01(rgb[i*3+1] * scale)) * 255.0f + 0.5f);
        d[0] = (uint8_t)(srgb_oetf(clamp01(rgb[i*3+2] * scale)) * 255.0f + 0.5f);
        d[3] = 255;
    }
    *out_bgra = b;
    return 1;
}