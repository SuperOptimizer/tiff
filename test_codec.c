/* test_codec — for every sample type and a spread of tau presets: build a
 * synthetic plane, compress + decompress, verify the max per-sample error is
 * within tau*range (the contract), and report the compression ratio.
 * Includes tiff.c for the static type helpers. */
#include "tiff.c"
#include <stdio.h>
#include <float.h>

static unsigned rng = 12345u;
static double dr(void) { rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return rng / 4294967296.0; }

/* fill a typed plane with a smooth pattern (+ a little detail + noise) spanning
 * [lo,hi]; return via the image's data buffer. */
static void fill(tiff_image *im, double lo, double hi, double noise) {
    size_t W = im->width, H = im->height, C = im->channels;
    for (size_t y = 0; y < H; y++)
        for (size_t x = 0; x < W; x++)
            for (size_t c = 0; c < C; c++) {
                double s = 0.5 + 0.5*sin(x/19.0 + c)*cos(y/23.0)
                         + 0.05*sin(x/3.0)*sin(y/3.0) + noise*(dr()-0.5);
                if (s < 0) s = 0;
                if (s > 1) s = 1;
                double v = lo + (hi-lo)*s;
                store_d(im->data, im->type, (y*W+x)*C + c, v);
            }
}

static const char *tn[] = {"u8","u16","u32","s8","s16","s32","f16","f32"};

static int one(tiff_type t, double lo, double hi, uint32_t W, uint32_t H, uint16_t C,
               double noise, float tau) {
    size_t ts = tiff_type_size(t), n = (size_t)W*H*C;
    tiff_image im = { W, H, C, t, malloc(n*ts) };
    fill(&im, lo, hi, noise);

    uint8_t *blob; size_t blen;
    tiff_codec_params pr = { tau, 0.0f };
    int rc = tiff_compress(&im, pr, &blob, &blen);
    if (rc != TIFF_OK) { printf("  FAIL compress %s tau=%.2f: %s\n", tn[t], tau, tiff_strerror(rc)); free(im.data); return 1; }

    tiff_image out;
    rc = tiff_decompress(blob, blen, &out);
    if (rc != TIFF_OK) { printf("  FAIL decompress %s tau=%.2f: %s\n", tn[t], tau, tiff_strerror(rc)); free(im.data); free(blob); return 1; }

    int fail = 0;
    if (out.width!=W||out.height!=H||out.channels!=C||out.type!=t) { printf("  FAIL meta %s\n", tn[t]); fail=1; }

    /* per-channel range -> per-channel bound; report worst (err / range) */
    double worst_ratio_err = 0;
    for (uint16_t c = 0; c < C && !fail; c++) {
        double mn=DBL_MAX, mx=-DBL_MAX;
        for (size_t i=0;i<(size_t)W*H;i++){ double v=load_d(im.data,t,i*C+c); if(v<mn)mn=v; if(v>mx)mx=v; }
        double range = mx-mn; if (range<=0) range=1;
        double tau_abs = (double)tau*range;
        double slack = (t==TIFF_F16)? range/1024.0 : (is_float_type(t)? range*1e-5 : 1e-6);
        double maxerr = 0;
        for (size_t i=0;i<(size_t)W*H;i++){
            double a=load_d(im.data,t,i*C+c), b=load_d(out.data,t,i*C+c);
            double e=fabs(a-b); if(e>maxerr)maxerr=e;
        }
        if (maxerr > tau_abs + slack) { printf("  FAIL bound %s c%u tau=%.2f: maxerr=%.4g > tau_abs=%.4g\n", tn[t], c, tau, maxerr, tau_abs); fail=1; }
        double re = maxerr/range; if (re>worst_ratio_err) worst_ratio_err=re;
    }

    double ratio = (double)(n*ts) / (double)blen;
    if (!fail) printf("  ok %-3s %ux%u c%u tau=%4.2f -> %7.1fx  (maxerr %.1f%% of range)\n",
                      tn[t], W, H, C, tau, ratio, worst_ratio_err*100.0);
    free(im.data); free(out.data); free(blob);
    return fail;
}

int main(void) {
    int fails = 0;
    float taus[] = { 0.01f, 0.05f, 0.16f, 0.64f };
    struct { tiff_type t; double lo, hi; } types[] = {
        {TIFF_U8,0,255}, {TIFF_U16,0,60000}, {TIFF_U32,0,200000},
        {TIFF_S8,-128,127}, {TIFF_S16,-30000,30000}, {TIFF_S32,-1000000,1000000},
        {TIFF_F16,-2,2}, {TIFF_F32,-1000,1000},
    };
    printf("== smooth plane 256x256, single channel ==\n");
    for (size_t i=0;i<sizeof types/sizeof*types;i++)
        for (size_t k=0;k<sizeof taus/sizeof*taus;k++)
            fails += one(types[i].t, types[i].lo, types[i].hi, 256, 256, 1, 0.0, taus[k]);

    printf("== edge size 100x70, 3 channels, with noise ==\n");
    for (size_t i=0;i<sizeof types/sizeof*types;i++)
        fails += one(types[i].t, types[i].lo, types[i].hi, 100, 70, 3, 0.02, 0.05f);

    printf("== u32 values above 2^24 (exercise correction path) ==\n");
    fails += one(TIFF_U32, 0, 100000000, 128, 128, 1, 0.0, 0.05f);

    printf(fails ? "test_codec: %d FAILED\n" : "test_codec: OK\n", fails);
    return fails ? 1 : 0;
}
