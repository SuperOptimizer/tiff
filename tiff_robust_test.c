/* tiff_robust_test — deterministic regression guard for the reader's
 * untrusted-input hardening. tiff_read parses attacker-controlled bytes; for
 * ANY input it must reject cleanly (rc < 0) or return a self-consistent owned
 * buffer of exactly the advertised size — never a crash, never a short buffer
 * a consumer reads past. Build malformed files directly so the contract is
 * guarded without a fuzz engine.
 *
 * Run under ASan/UBSan so a regression faults hard:
 *   clang -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
 *     tiff_robust_test.c tiff.c -o t && ./t
 */
#include "tiff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(x) do{ if(!(x)){ fails++; fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x);} }while(0)

static void w16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void w32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

/* write bytes to a temp file, tiff_read it, return rc; on accept, recompute the
 * expected extent independently, assert it matches, and touch every byte. */
static int try_read(const uint8_t *bytes, size_t n) {
    char path[] = "/tmp/tiff_robust_XXXXXX";
    int fd = mkstemp(path); if (fd < 0) return -99;
    FILE *f = fdopen(fd, "wb"); fwrite(bytes, 1, n, f); fclose(f);
    tiff_image t;
    int rc = tiff_read(path, &t);
    if (rc == TIFF_OK) {
        CHECK(t.width > 0 && t.height > 0 && t.channels >= 1);
        CHECK(t.data != NULL);
        size_t want = (size_t)t.width * t.height * t.channels * tiff_type_size(t.type);
        volatile uint64_t s = 0;
        const uint8_t *px = t.data;
        for (size_t i = 0; i < want; ++i) s ^= px[i];
        (void)s;
        tiff_free(&t);
    }
    unlink(path);
    return rc;
}

/* minimal single-IFD little-endian TIFF; pixel strip at offset 8, 64-byte
 * strip. ifd_off_override (if nonzero) poisons the header IFD offset. */
static size_t build_tiff_bps(uint8_t *buf, uint32_t w, uint32_t h, uint32_t ifd_off_override, int bps) {
    uint32_t pix_off = 8, pix_bytes = 64;
    uint32_t ifd_off = pix_off + pix_bytes;
    memset(buf, 0xAB, ifd_off);
    buf[0]='I'; buf[1]='I'; w16(buf+2,42);
    w32(buf+4, ifd_off_override ? ifd_off_override : ifd_off);
    uint8_t *e = buf + ifd_off;
    int ntag = 9;
    w16(e, (uint16_t)ntag); e += 2;
    #define T_LONG(tag,val)  do{ w16(e,(tag)); w16(e+2,4); w32(e+4,1); w32(e+8,(uint32_t)(val)); e+=12; }while(0)
    #define T_SHORT(tag,val) do{ w16(e,(tag)); w16(e+2,3); w32(e+4,1); w16(e+8,(uint16_t)(val)); w16(e+10,0); e+=12; }while(0)
    T_LONG (256, w);          /* ImageWidth        */
    T_LONG (257, h);          /* ImageLength       */
    T_SHORT(258, bps);        /* BitsPerSample     */
    T_SHORT(259, 1);          /* Compression=none  */
    T_SHORT(262, 1);          /* Photometric       */
    T_LONG (273, pix_off);    /* StripOffsets      */
    T_SHORT(277, 1);          /* SamplesPerPixel   */
    T_LONG (278, h);          /* RowsPerStrip      */
    T_SHORT(279, pix_bytes);  /* StripByteCounts   */
    #undef T_LONG
    #undef T_SHORT
    w32(e, 0); e += 4;        /* next IFD = 0 */
    return (size_t)(e - buf);
}
static size_t build_tiff(uint8_t *buf, uint32_t w, uint32_t h, uint32_t ifd_off_override) {
    return build_tiff_bps(buf, w, h, ifd_off_override, 8);
}

int main(void) {
    uint8_t buf[512];

    /* IFD-offset 32-bit overflow: ifd_off near UINT32_MAX must not wrap the
     * bounds check into a ~4GB OOB read. Reject. */
    {
        uint8_t poison[24]; memset(poison,0,sizeof poison);
        poison[0]='I'; poison[1]='I'; w16(poison+2,42); w32(poison+4, 0xFFFFFFFEu);
        int rc = try_read(poison, sizeof poison);
        CHECK(rc < 0);
        printf("ifd_off=0xFFFFFFFE overflow: rc=%d %s\n", rc, rc<0?"rejected":"ACCEPTED!");
    }

    /* zero width / height -> degenerate. Reject. */
    {
        size_t n = build_tiff(buf, 0, 8, 0);
        CHECK(try_read(buf, n) < 0);
        n = build_tiff(buf, 8, 0, 0);
        CHECK(try_read(buf, n) < 0);
        printf("zero dims rejected\n");
    }

    /* well-formed 8x8 u8 still opens (no over-rejection). */
    {
        size_t n = build_tiff(buf, 8, 8, 0);
        int rc = try_read(buf, n);
        CHECK(rc == TIFF_OK);
        printf("valid 8x8 u8: rc=%d %s\n", rc, rc==TIFF_OK?"accepted":"REJECTED!");
    }

    /* more adversarial shapes that must reject without crashing. */
    {
        /* absurd dims: the strip can't possibly cover them. Reject. */
        size_t n = build_tiff(buf, 0x7FFFFFFFu, 0x7FFFFFFFu, 0);
        CHECK(try_read(buf, n) < 0);

        /* truncated below the 8-byte header. Reject. */
        uint8_t tiny[4] = {'I','I',42,0};
        CHECK(try_read(tiny, sizeof tiny) < 0);

        /* unsupported sample width (bps=4): only 8/16/32 decode. Reject. */
        n = build_tiff_bps(buf, 8, 8, 0, 4);
        CHECK(try_read(buf, n) < 0);
        printf("adversarial shapes rejected\n");
    }

    printf(fails ? "tiff_robust_test: %d FAILED\n" : "tiff_robust_test: OK\n", fails);
    return fails ? 1 : 0;
}
