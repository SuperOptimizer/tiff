/* fuzz_tiff_seed — emit valid TIFFs (a spread of sample types / channel counts)
 * as the AFL++ corpus for the TIFF reader fuzzer. Real header+IFD+strip
 * structure gives the mutator something to corrupt past the magic check.
 *
 *   usage: fuzz_tiff_seed <corpus_dir>
 */
#include "tiff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <corpus_dir>\n", argv[0]); return 2; }
    unsigned char buf[16*16*5*4];
    memset(buf, 0xAB, sizeof buf);
    char p[1024];
    struct { const char *name; uint32_t w, h; uint16_t c; tiff_type t; } seeds[] = {
        {"u8_1",  8,8,1, TIFF_U8},  {"u16_1", 8,8,1, TIFF_U16},
        {"u32_1", 8,8,1, TIFF_U32}, {"s8_1",  8,8,1, TIFF_S8},
        {"s16_1", 8,8,1, TIFF_S16}, {"s32_1", 8,8,1, TIFF_S32},
        {"f16_1", 8,8,1, TIFF_F16}, {"f32_1", 8,8,1, TIFF_F32},
        {"u8_3",  4,4,3, TIFF_U8},  {"f32_4", 4,4,4, TIFF_F32},
        {"s16_2", 4,4,2, TIFF_S16}, {"u16_5", 4,4,5, TIFF_U16},
    };
    for (size_t i = 0; i < sizeof seeds/sizeof seeds[0]; ++i) {
        snprintf(p, sizeof p, "%s/seed_%s.tif", argv[1], seeds[i].name);
        tiff_image im = { .width = seeds[i].w, .height = seeds[i].h,
                          .channels = seeds[i].c, .type = seeds[i].t, .data = buf };
        if (tiff_write(p, &im) != 0) fprintf(stderr, "seed %s failed\n", p);
        else printf("seed %s\n", p);
    }
    return 0;
}
