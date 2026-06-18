/* test_entropy — round-trip random quantized blocks through the range coder /
 * block coefficient coder; verify identical levels out and that sparse blocks
 * are much smaller than dense ones. Includes codec.c for the static API. */
#include "tiff.c"
#include <stdio.h>

static unsigned rng = 88172645u;
static uint32_t urand(void) { rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return rng; }

static int roundtrip(const int32_t *q, const char *name, double *bits_out) {
    scan_init();
    renc e; renc_init(&e);
    blkctx c; blkctx_init(&c);
    enc_block(&e, &c, q);
    renc_flush(&e);
    if (e.oom) { printf("FAIL %s: oom\n", name); free(e.buf); return 1; }

    rdec d; rdec_init(&d, e.buf, e.len);
    blkctx c2; blkctx_init(&c2);
    static int32_t r[NCOEF];
    dec_block(&d, &c2, r);
    int bad = 0;
    for (int i = 0; i < NCOEF; i++) if (q[i] != r[i]) { bad++; }
    if (bits_out) *bits_out = e.len * 8.0;
    if (bad) printf("FAIL %s: %d/%d mismatched levels\n", name, bad, NCOEF);
    else printf("ok   %s: %zu bytes\n", name, e.len);
    free(e.buf);
    return bad ? 1 : 0;
}

int main(void) {
    static int32_t q[NCOEF];
    int fails = 0;
    double bsparse = 0, bdense = 0;

    /* all zero */
    memset(q, 0, sizeof q);
    fails += roundtrip(q, "all-zero", NULL);

    /* DC only */
    memset(q, 0, sizeof q); q[0] = 1234;
    fails += roundtrip(q, "dc-only", NULL);

    /* sparse low-freq (typical compressed block): a few nonzeros up front */
    memset(q, 0, sizeof q);
    for (int i = 0; i < 20; i++) q[g_scan_ready ? g_scan[i] : i] = (int32_t)(urand()%50) - 25;
    scan_init(); memset(q,0,sizeof q);
    for (int i = 0; i < 20; i++) q[g_scan[i]] = (int32_t)(urand()%50) - 25;
    fails += roundtrip(q, "sparse-lowfreq", &bsparse);

    /* dense random (incompressible-ish) */
    for (int i = 0; i < NCOEF; i++) q[i] = (int32_t)(urand()%2001) - 1000;
    fails += roundtrip(q, "dense-random", &bdense);

    /* extremes */
    for (int i = 0; i < NCOEF; i++) q[i] = (i&1) ? 2000000000 : -2000000000;
    fails += roundtrip(q, "extreme-mag", NULL);

    printf("sparse %.0f bits vs dense %.0f bits\n", bsparse, bdense);
    if (bsparse >= bdense) { printf("FAIL: sparse not smaller than dense\n"); fails++; }

    printf(fails ? "test_entropy: %d FAILED\n" : "test_entropy: OK\n", fails);
    return fails ? 1 : 0;
}
