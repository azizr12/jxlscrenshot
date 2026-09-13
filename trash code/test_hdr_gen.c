/*
 * test_hdr_gen.c
 * 
 * Generates a synthetic HDR test image to verify libjxl HDR encoding.
 * Output: A 512x512 image with a dark background (0.1 linear) and a 
 * bright central highlight (10.0 linear, representing ~800-1000 nits in scRGB).
 *
 * Build (MSYS2 / MinGW-w64):
 *   gcc -O2 -o test_hdr_gen.exe test_hdr_gen.c -ljxl
 *
 * Usage:
 *   ./test_hdr_gen.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <jxl/encode.h>

#define WIDTH 512
#define HEIGHT 512

int main(void) {
    printf("Generating synthetic HDR test image (%dx%d)...\n", WIDTH, HEIGHT);

    // 1. Allocate memory for 32-bit float RGB image
    size_t num_pixels = WIDTH * HEIGHT;
    size_t num_floats = num_pixels * 3;
    float *image = (float *)malloc(num_floats * sizeof(float));
    if (!image) {
        fprintf(stderr, "Failed to allocate memory.\n");
        return 1;
    }

    // 2. Generate the image data (Dark background, bright HDR highlight)
    float center_x = WIDTH / 2.0f;
    float center_y = HEIGHT / 2.0f;
    float radius = 100.0f;

    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            size_t idx = (y * WIDTH + x) * 3;
            
            // Calculate distance from center
            float dx = x - center_x;
            float dy = y - center_y;
            float dist = sqrtf(dx * dx + dy * dy);

            if (dist < radius) {
                // Bright HDR highlight (10.0 in linear scRGB ≈ 800-1000 nits)
                image[idx + 0] = 10.0f; // R
                image[idx + 1] = 10.0f; // G
                image[idx + 2] = 10.0f; // B
            } else {
                // Dim background (0.1 in linear scRGB ≈ 8-10 nits)
                image[idx + 0] = 0.1f; // R
                image[idx + 1] = 0.1f; // G
                image[idx + 2] = 0.1f; // B
            }
        }
    }

    // 3. Initialize JPEG XL Encoder
    JxlEncoder *enc = JxlEncoderCreate(NULL);
    if (!enc) {
        fprintf(stderr, "Failed to create JxlEncoder.\n");
        free(image);
        return 1;
    }

    // 4. Set Basic Info (32-bit float)
    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = WIDTH;
    info.ysize = HEIGHT;
    info.bits_per_sample = 32;
    info.exponent_bits_per_sample = 8; // Standard IEEE 754 32-bit float

    if (JxlEncoderSetBasicInfo(enc, &info) != JXL_ENC_SUCCESS) {
        fprintf(stderr, "Failed to set basic info.\n");
        JxlEncoderDestroy(enc);
        free(image);
        return 1;
    }

    // 5. Set HDR Color Encoding (Linear scRGB)
    JxlColorEncoding ce;
    ce.color_space = JXL_COLOR_SPACE_RGB;
    ce.white_point = JXL_WHITE_POINT_D65;
    ce.primaries = JXL_PRIMARIES_SRGB;
    ce.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR; // Crucial for HDR

    if (JxlEncoderSetColorEncoding(enc, &ce) != JXL_ENC_SUCCESS) {
        fprintf(stderr, "Failed to set color encoding.\n");
        JxlEncoderDestroy(enc);
        free(image);
        return 1;
    }

    // 6. Configure Frame Settings (Lossless for this test)
    JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    JxlEncoderSetFrameLossless(fs, JXL_TRUE);
    JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT, 7);

    // 7. Add Image Frame
    JxlPixelFormat fmt = {3, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0};
    if (JxlEncoderAddImageFrame(fs, &fmt, image, num_floats * sizeof(float)) != JXL_ENC_SUCCESS) {
        fprintf(stderr, "Failed to add image frame.\n");
        JxlEncoderDestroy(enc);
        free(image);
        return 1;
    }

    JxlEncoderCloseInput(enc);

    // 8. Process Output
    size_t cap = 1024 * 1024; // 1MB initial buffer
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) {
        fprintf(stderr, "Failed to allocate output buffer.\n");
        JxlEncoderDestroy(enc);
        free(image);
        return 1;
    }

    uint8_t *next = buf;
    size_t avail = cap;
    JxlEncoderStatus st;

    for (;;) {
        st = JxlEncoderProcessOutput(enc, &next, &avail);
        if (st == JXL_ENC_SUCCESS) break;
        if (st == JXL_ENC_NEED_MORE_OUTPUT) {
            size_t used = (size_t)(next - buf);
            cap *= 2;
            uint8_t *nb = (uint8_t *)realloc(buf, cap);
            if (!nb) {
                fprintf(stderr, "Failed to reallocate output buffer.\n");
                free(buf);
                JxlEncoderDestroy(enc);
                free(image);
                return 1;
            }
            buf = nb;
            next = buf + used;
            avail = cap - used;
            continue;
        }
        fprintf(stderr, "JXL Encoder error: %d\n", st);
        free(buf);
        JxlEncoderDestroy(enc);
        free(image);
        return 1;
    }

    // 9. Write to File
    const char *out_path = "test_hdr_output.jxl";
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "Failed to open %s for writing.\n", out_path);
        free(buf);
        JxlEncoderDestroy(enc);
        free(image);
        return 1;
    }

    size_t written = fwrite(buf, 1, cap - avail, f);
    fclose(f);

    printf("Successfully encoded HDR image to: %s (%zu bytes)\n", out_path, written);
    printf("\n--- HOW TO VERIFY ---\n");
    printf("1. Open 'test_hdr_output.jxl' in an HDR-capable viewer (e.g., Windows 11 Photos app, or a dedicated HDR viewer).\n");
    printf("2. Ensure your monitor has HDR enabled in Windows Display Settings.\n");
    printf("3. EXPECTED: The center circle should be blindingly bright (simulating ~1000 nits), while the background is very dark.\n");
    printf("4. IF IT LOOKS DARK/GRAY: Your viewer is ignoring the HDR metadata or forcing SDR tone mapping.\n");

    // Cleanup
    free(buf);
    JxlEncoderDestroy(enc);
    free(image);
    return 0;
}