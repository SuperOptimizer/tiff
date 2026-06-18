/* tiff_test.c — round-trip every sample type at a few channel counts. */
#include "tiff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *type_name(tiff_type t) {
    static const char *n[] = {"u8","u16","u32","s8","s16","s32","f16","f32"};
    return n[t];
}

static int run_one(tiff_type type, uint16_t channels) {
    const uint32_t W = 37, H = 19;   /* deliberately odd, single strip */
    size_t ts = tiff_type_size(type);
    size_t nbytes = (size_t)W * H * channels * ts;

    uint8_t *src = malloc(nbytes);
    for (size_t i = 0; i < nbytes; i++) src[i] = (uint8_t)((i * 131 + 7) & 0xff);

    tiff_image w = { .width = W, .height = H, .channels = channels,
                     .type = type, .data = src };
    const char *path = "/tmp/tiff_rt.tif";
    int rc = tiff_write(path, &w);
    if (rc != TIFF_OK) { printf("  WRITE %s c%u: %s\n", type_name(type), channels, tiff_strerror(rc)); free(src); return 1; }

    tiff_image r;
    rc = tiff_read(path, &r);
    if (rc != TIFF_OK) { printf("  READ  %s c%u: %s\n", type_name(type), channels, tiff_strerror(rc)); free(src); return 1; }

    int fail = 0;
    if (r.width != W || r.height != H || r.channels != channels || r.type != type) {
        printf("  META  %s c%u mismatch: %ux%u c%u t%d\n", type_name(type), channels,
               r.width, r.height, r.channels, r.type);
        fail = 1;
    } else if (memcmp(src, r.data, nbytes) != 0) {
        printf("  DATA  %s c%u mismatch\n", type_name(type), channels);
        fail = 1;
    }
    tiff_free(&r);
    free(src);
    if (!fail) printf("  ok   %s c%u\n", type_name(type), channels);
    return fail;
}

int main(void) {
    tiff_type types[] = { TIFF_U8, TIFF_U16, TIFF_U32, TIFF_S8, TIFF_S16, TIFF_S32, TIFF_F16, TIFF_F32 };
    uint16_t chans[] = { 1, 2, 3, 4, 5 };
    int fails = 0;
    for (size_t t = 0; t < sizeof types / sizeof *types; t++)
        for (size_t c = 0; c < sizeof chans / sizeof *chans; c++)
            fails += run_one(types[t], chans[c]);
    printf(fails ? "FAILED (%d)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
