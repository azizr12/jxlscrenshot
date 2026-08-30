#ifndef JXLSHOT_HDR_H
#define JXLSHOT_HDR_H

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Captured frame. If is_hdr==1, rgb holds linear scRGB floats (3 per pixel). */
typedef struct {
    float *rgb;
    int    w, h;
    int    is_hdr;
} hdr_frame_t;

/* Capture the primary monitor via DXGI Desktop Duplication.
 * Returns 1 only when the output is a real HDR (PQ) display and a valid
 * FP16 scRGB frame was acquired. Returns 0 otherwise (caller uses GDI). */
int  hdr_capture_primary(hdr_frame_t *f, int timeout_ms);
void hdr_frame_free(hdr_frame_t *f);

/* Encode linear scRGB floats as JPEG XL: Rec.2020 primaries + ST.2084 PQ,
 * float32 samples, intensity_target 10000 nits. */
int  hdr_encode_jxl_hdr(const float *rgb, int w, int h,
                        int lossless, float distance, const wchar_t *path);

/* OBS/Windows-style HDR->SDR: rescale (1.0 == 80 nits) to sdr_white_nits,
 * clip, sRGB gamma. Returns malloc'd tightly packed BGRA. */
int  hdr_tonemap_bgra(const float *rgb, int w, int h,
                      float sdr_white_nits, uint8_t **out_bgra);

#ifdef __cplusplus
}
#endif
#endif /* JXLSHOT_HDR_H */