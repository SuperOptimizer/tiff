/* test_dct — verify the 64x64 float DCT: round-trip, energy (Parseval), and DC
 * compaction. Includes codec.c to reach the static transform functions. */
#include "tiff.c"
#include <stdio.h>

static unsigned rng = 2463534242u;
static float frand(void) { rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return (rng/4294967296.0f)*2.0f-1.0f; }

int main(void) {
    dct_init();
    static float B[DCTN*DCTN], F[DCTN*DCTN], R[DCTN*DCTN], tmp[DCTN*DCTN];
    int fails = 0;

    /* round-trip + Parseval on random data */
    double eB = 0, eF = 0, maxerr = 0;
    for (int i = 0; i < DCTN*DCTN; i++) { B[i] = frand() * 1000.0f; eB += (double)B[i]*B[i]; }
    dct64_fwd(B, F, tmp);
    for (int i = 0; i < DCTN*DCTN; i++) eF += (double)F[i]*F[i];
    dct64_inv(F, R, tmp);
    for (int i = 0; i < DCTN*DCTN; i++) { double e = fabs(B[i]-R[i]); if (e > maxerr) maxerr = e; }
    printf("round-trip max abs err = %.3e (values ~1000)\n", maxerr);
    printf("Parseval: sum B^2 = %.6e, sum F^2 = %.6e, rel diff = %.3e\n",
           eB, eF, fabs(eB-eF)/eB);
    if (maxerr > 1e-1)              { printf("FAIL round-trip\n"); fails++; }
    if (fabs(eB-eF)/eB > 1e-5)      { printf("FAIL Parseval\n");   fails++; }

    /* DC compaction: a constant block has all energy in F[0][0] */
    for (int i = 0; i < DCTN*DCTN; i++) B[i] = 123.0f;
    dct64_fwd(B, F, tmp);
    double off = 0; for (int i = 1; i < DCTN*DCTN; i++) off += (double)F[i]*F[i];
    printf("constant block: F[0][0] = %.4f, off-DC energy = %.3e\n", F[0], off);
    if (off > 1e-3) { printf("FAIL DC compaction\n"); fails++; }
    dct64_inv(F, R, tmp);
    double dcerr = 0; for (int i = 0; i < DCTN*DCTN; i++) { double e = fabs(R[i]-123.0); if (e>dcerr) dcerr = e; }
    if (dcerr > 1e-2) { printf("FAIL DC round-trip (%.3e)\n", dcerr); fails++; }

    printf(fails ? "test_dct: %d FAILED\n" : "test_dct: OK\n", fails);
    return fails ? 1 : 0;
}
