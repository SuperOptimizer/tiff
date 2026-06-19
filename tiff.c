/* tiff.c — see tiff.h. Plain uncompressed baseline TIFF, no external deps. */
#include "tiff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ types -- */

size_t tiff_type_size(tiff_type t) {
    switch (t) {
        case TIFF_U8:  case TIFF_S8:                return 1;
        case TIFF_U16: case TIFF_S16: case TIFF_F16:return 2;
        case TIFF_U32: case TIFF_S32: case TIFF_F32:return 4;
    }
    return 0;
}

const char *tiff_strerror(int code) {
    switch (code) {
        case TIFF_OK:           return "ok";
        case TIFF_EOPEN:        return "could not open file";
        case TIFF_EIO:          return "I/O error";
        case TIFF_EFORMAT:      return "not a TIFF / malformed";
        case TIFF_EUNSUPPORTED: return "unsupported TIFF layout";
        case TIFF_EMEM:         return "out of memory";
        case TIFF_EINVAL:       return "invalid argument";
    }
    return "unknown error";
}

/* TIFF SampleFormat values. */
#define SF_UINT  1
#define SF_INT   2
#define SF_FLOAT 3

static int type_bits(tiff_type t)   { return (int)(tiff_type_size(t) * 8); }
static int type_format(tiff_type t) {
    switch (t) {
        case TIFF_U8: case TIFF_U16: case TIFF_U32: return SF_UINT;
        case TIFF_S8: case TIFF_S16: case TIFF_S32: return SF_INT;
        case TIFF_F16: case TIFF_F32:               return SF_FLOAT;
    }
    return 0;
}

/* (SampleFormat, bits) -> type, or -1 if unsupported. */
static int decode_type(int format, int bits, tiff_type *out) {
    switch (format) {
        case SF_UINT:
            if (bits == 8)  { *out = TIFF_U8;  return 0; }
            if (bits == 16) { *out = TIFF_U16; return 0; }
            if (bits == 32) { *out = TIFF_U32; return 0; }
            break;
        case SF_INT:
            if (bits == 8)  { *out = TIFF_S8;  return 0; }
            if (bits == 16) { *out = TIFF_S16; return 0; }
            if (bits == 32) { *out = TIFF_S32; return 0; }
            break;
        case SF_FLOAT:
            if (bits == 16) { *out = TIFF_F16; return 0; }
            if (bits == 32) { *out = TIFF_F32; return 0; }
            break;
    }
    return -1;
}

/* ----------------------------------------------------------- LE put helpers - */

static void put16(uint8_t *b, size_t off, uint16_t v) {
    b[off]   = (uint8_t)(v & 0xff);
    b[off+1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *b, size_t off, uint32_t v) {
    b[off]   = (uint8_t)(v & 0xff);
    b[off+1] = (uint8_t)((v >> 8)  & 0xff);
    b[off+2] = (uint8_t)((v >> 16) & 0xff);
    b[off+3] = (uint8_t)((v >> 24) & 0xff);
}

/* TIFF field type codes used by the writer. */
#define FT_SHORT    3
#define FT_LONG     4
#define FT_RATIONAL 5

/* Tag numbers. */
#define T_WIDTH        256
#define T_LENGTH       257
#define T_BITSPERSAMP  258
#define T_COMPRESSION  259
#define T_PHOTOMETRIC  262
#define T_STRIPOFFSETS 273
#define T_SAMPLESPP    277
#define T_ROWSPERSTRIP 278
#define T_STRIPCOUNTS  279
#define T_XRES         282
#define T_YRES         283
#define T_PLANARCONFIG 284
#define T_RESUNIT      296
#define T_EXTRASAMPLES 338
#define T_SAMPLEFORMAT 339
#define T_TILEWIDTH    322

/* ---------------------------------------------------------------- writer --- */

int tiff_write(const char *path, const tiff_image *img) {
    if (!path || !img || !img->data) return TIFF_EINVAL;
    if (img->width == 0 || img->height == 0 || img->channels == 0) return TIFF_EINVAL;
    size_t ts = tiff_type_size(img->type);
    if (ts == 0) return TIFF_EINVAL;

    const uint32_t W = img->width, H = img->height;
    const uint16_t C = img->channels;
    const int bits = type_bits(img->type);
    const int sf   = type_format(img->type);

    /* pixel size must fit a classic-TIFF 32-bit StripByteCounts. */
    uint64_t pixel_bytes = (uint64_t)W * H * C * ts;
    if (pixel_bytes > 0xffffffffULL) return TIFF_EUNSUPPORTED;

    /* photometric + extra samples */
    uint16_t photometric;
    int extra;                 /* number of ExtraSamples */
    uint16_t extra_val[4];     /* values for up to a handful; only [0] varies   */
    if (C == 1)      { photometric = 1; extra = 0; }          /* BlackIsZero    */
    else if (C == 3) { photometric = 2; extra = 0; }          /* RGB            */
    else if (C == 4) { photometric = 2; extra = 1; extra_val[0] = 2; } /* RGBA  */
    else             { photometric = 1; extra = C - 1; }      /* gray + extras  */
    for (int i = (C == 4 ? 1 : 0); i < extra; i++) extra_val[i] = 0; /* unspecified */

    const int has_extra = extra > 0;
    const int n_entries = 14 + (has_extra ? 1 : 0);

    /* layout: header(8) | IFD | overflow | pad | pixels */
    size_t ifd_off   = 8;
    size_t ifd_bytes = 2 + (size_t)n_entries * 12 + 4;
    size_t ov        = ifd_off + ifd_bytes;        /* overflow cursor */

    size_t xres_off = ov; ov += 8;
    size_t yres_off = ov; ov += 8;
    size_t bps_off  = 0, sf_off = 0, es_off = 0;
    if (C > 2)     { bps_off = ov; ov += (size_t)C * 2; sf_off = ov; ov += (size_t)C * 2; }
    if (extra > 2) { es_off  = ov; ov += (size_t)extra * 2; }

    size_t pixel_off = (ov + 7) & ~(size_t)7;      /* 8-byte align pixels */

    uint8_t *hdr = calloc(1, pixel_off);
    if (!hdr) return TIFF_EMEM;

    /* file header */
    hdr[0] = 'I'; hdr[1] = 'I';
    put16(hdr, 2, 42);
    put32(hdr, 4, (uint32_t)ifd_off);

    /* IFD */
    put16(hdr, ifd_off, (uint16_t)n_entries);
    size_t e = ifd_off + 2;
    #define ENTRY(tag, ftype, count, value) do { \
        put16(hdr, e, (uint16_t)(tag));   put16(hdr, e+2, (uint16_t)(ftype)); \
        put32(hdr, e+4, (uint32_t)(count)); put32(hdr, e+8, (uint32_t)(value)); \
        e += 12; } while (0)

    /* inline-packed SHORT array value for count 1 or 2 */
    #define PACK2(a, b) ((uint32_t)(a) | ((uint32_t)(b) << 16))

    uint32_t bps_value = (C == 1) ? (uint32_t)bits
                       : (C == 2) ? PACK2(bits, bits) : (uint32_t)bps_off;
    uint32_t sf_value  = (C == 1) ? (uint32_t)sf
                       : (C == 2) ? PACK2(sf, sf)     : (uint32_t)sf_off;
    uint32_t es_value  = (extra == 1) ? (uint32_t)extra_val[0]
                       : (extra == 2) ? PACK2(extra_val[0], extra_val[1])
                       : (uint32_t)es_off;

    ENTRY(T_WIDTH,        FT_LONG,  1, W);
    ENTRY(T_LENGTH,       FT_LONG,  1, H);
    ENTRY(T_BITSPERSAMP,  FT_SHORT, C, bps_value);
    ENTRY(T_COMPRESSION,  FT_SHORT, 1, 1);              /* none */
    ENTRY(T_PHOTOMETRIC,  FT_SHORT, 1, photometric);
    ENTRY(T_STRIPOFFSETS, FT_LONG,  1, (uint32_t)pixel_off);
    ENTRY(T_SAMPLESPP,    FT_SHORT, 1, C);
    ENTRY(T_ROWSPERSTRIP, FT_LONG,  1, H);             /* one strip */
    ENTRY(T_STRIPCOUNTS,  FT_LONG,  1, (uint32_t)pixel_bytes);
    ENTRY(T_XRES,         FT_RATIONAL, 1, (uint32_t)xres_off);
    ENTRY(T_YRES,         FT_RATIONAL, 1, (uint32_t)yres_off);
    ENTRY(T_PLANARCONFIG, FT_SHORT, 1, 1);             /* chunky */
    ENTRY(T_RESUNIT,      FT_SHORT, 1, 2);             /* inch */
    if (has_extra)
        ENTRY(T_EXTRASAMPLES, FT_SHORT, extra, es_value);
    ENTRY(T_SAMPLEFORMAT, FT_SHORT, C, sf_value);
    /* next-IFD offset already zero from calloc */

    /* overflow data */
    put32(hdr, xres_off, 72); put32(hdr, xres_off + 4, 1);
    put32(hdr, yres_off, 72); put32(hdr, yres_off + 4, 1);
    if (C > 2)
        for (int i = 0; i < C; i++) {
            put16(hdr, bps_off + (size_t)i * 2, (uint16_t)bits);
            put16(hdr, sf_off  + (size_t)i * 2, (uint16_t)sf);
        }
    if (extra > 2)
        for (int i = 0; i < extra; i++)
            put16(hdr, es_off + (size_t)i * 2, extra_val[i]);

    #undef ENTRY
    #undef PACK2

    /* write header block then pixels */
    int rc = TIFF_OK;
    FILE *f = fopen(path, "wb");
    if (!f) { free(hdr); return TIFF_EOPEN; }
    if (fwrite(hdr, 1, pixel_off, f) != pixel_off ||
        fwrite(img->data, 1, (size_t)pixel_bytes, f) != (size_t)pixel_bytes)
        rc = TIFF_EIO;
    if (fclose(f) != 0 && rc == TIFF_OK) rc = TIFF_EIO;
    free(hdr);
    return rc;
}

/* ---------------------------------------------------------------- reader --- */

static uint16_t rd16(const uint8_t *b, int big) {
    return big ? (uint16_t)((b[0] << 8) | b[1])
               : (uint16_t)((b[1] << 8) | b[0]);
}
static uint32_t rd32(const uint8_t *b, int big) {
    return big ? ((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3])
               : ((uint32_t)b[3] << 24 | (uint32_t)b[2] << 16 | (uint32_t)b[1] << 8 | b[0]);
}

/* size in bytes of a TIFF field type, 0 if unknown to us */
static int field_size(int ft) {
    switch (ft) {
        case 1: case 2: case 6: case 7: return 1; /* BYTE/ASCII/SBYTE/UNDEFINED */
        case 3: case 8:                 return 2; /* SHORT/SSHORT */
        case 4: case 9:                 return 4; /* LONG/SLONG */
        case 5: case 10:                return 8; /* RATIONAL/SRATIONAL */
        case 11:                        return 4; /* FLOAT */
        case 12:                        return 8; /* DOUBLE */
    }
    return 0;
}

typedef struct { uint16_t tag, type; uint32_t count, valoff; } entry;

static const entry *find(const entry *es, int n, uint16_t tag) {
    for (int i = 0; i < n; i++) if (es[i].tag == tag) return &es[i];
    return NULL;
}

/* Read up to `max` integer values of an entry (SHORT/LONG family) into out[].
 * Returns count read, or -1 on bounds/type error. */
static long read_ints(const uint8_t *fb, size_t n, int big,
                      const entry *en, uint32_t *out, uint32_t max) {
    int es = field_size(en->type);
    if (es != 1 && es != 2 && es != 4) return -1;
    uint64_t total = (uint64_t)en->count * es;
    size_t base = (total <= 4) ? en->valoff : rd32(fb + en->valoff, big);
    if (base + total > n) return -1;
    uint32_t cnt = en->count < max ? en->count : max;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *p = fb + base + (size_t)i * es;
        out[i] = (es == 1) ? p[0] : (es == 2) ? rd16(p, big) : rd32(p, big);
    }
    return (long)cnt;
}

/* single scalar with default */
static uint32_t read_scalar(const uint8_t *fb, size_t n, int big,
                            const entry *es, int ne, uint16_t tag, uint32_t dflt) {
    const entry *en = find(es, ne, tag);
    if (!en) return dflt;
    uint32_t v = dflt;
    if (read_ints(fb, n, big, en, &v, 1) == 1) return v;
    return dflt;
}

int tiff_read(const char *path, tiff_image *img) {
    if (!path || !img) return TIFF_EINVAL;
    memset(img, 0, sizeof *img);

    /* slurp file */
    FILE *f = fopen(path, "rb");
    if (!f) return TIFF_EOPEN;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return TIFF_EIO; }
    long flen = ftell(f);
    if (flen < 8) { fclose(f); return TIFF_EFORMAT; }
    rewind(f);
    uint8_t *fb = malloc((size_t)flen);
    if (!fb) { fclose(f); return TIFF_EMEM; }
    if (fread(fb, 1, (size_t)flen, f) != (size_t)flen) { free(fb); fclose(f); return TIFF_EIO; }
    fclose(f);
    size_t n = (size_t)flen;

    int rc = TIFF_EFORMAT;
    uint32_t *soff = NULL, *scnt = NULL;
    entry *es = NULL;

    /* header */
    int big;
    if      (fb[0] == 'I' && fb[1] == 'I') big = 0;
    else if (fb[0] == 'M' && fb[1] == 'M') big = 1;
    else goto done;
    if (rd16(fb + 2, big) != 42) { rc = TIFF_EUNSUPPORTED; goto done; } /* 43 = BigTIFF */

    uint32_t ifd = rd32(fb + 4, big);
    if ((uint64_t)ifd + 2 > n) goto done;   /* 64-bit math: avoid uint32 wrap */
    int ne = rd16(fb + ifd, big);
    if (ne <= 0 || ifd + 2 + (uint64_t)ne * 12 + 4 > n) goto done;

    es = malloc((size_t)ne * sizeof *es);
    if (!es) { rc = TIFF_EMEM; goto done; }
    for (int i = 0; i < ne; i++) {
        size_t p = ifd + 2 + (size_t)i * 12;
        es[i].tag    = rd16(fb + p, big);
        es[i].type   = rd16(fb + p + 2, big);
        es[i].count  = rd32(fb + p + 4, big);
        es[i].valoff = (uint32_t)(p + 8);   /* offset OF the value field */
    }

    /* reject layouts we don't decode */
    if (read_scalar(fb, n, big, es, ne, T_COMPRESSION, 1) != 1) { rc = TIFF_EUNSUPPORTED; goto done; }
    if (read_scalar(fb, n, big, es, ne, T_PLANARCONFIG, 1) != 1) { rc = TIFF_EUNSUPPORTED; goto done; }
    if (find(es, ne, T_TILEWIDTH)) { rc = TIFF_EUNSUPPORTED; goto done; }

    uint32_t W   = read_scalar(fb, n, big, es, ne, T_WIDTH, 0);
    uint32_t H   = read_scalar(fb, n, big, es, ne, T_LENGTH, 0);
    uint32_t spp = read_scalar(fb, n, big, es, ne, T_SAMPLESPP, 1);
    if (W == 0 || H == 0 || spp == 0 || spp > 0xffff) goto done;

    /* bits per sample: per channel, must be uniform */
    int bits;
    {
        const entry *en = find(es, ne, T_BITSPERSAMP);
        if (!en) { bits = 1; }
        else {
            uint32_t *tmp = malloc((size_t)spp * sizeof *tmp);
            if (!tmp) { rc = TIFF_EMEM; goto done; }
            long got = read_ints(fb, n, big, en, tmp, spp);
            if (got < 1) { free(tmp); goto done; }
            bits = (int)tmp[0];
            for (long i = 1; i < got; i++) if ((int)tmp[i] != bits) { free(tmp); rc = TIFF_EUNSUPPORTED; goto done; }
            free(tmp);
        }
    }
    /* sample format: per channel, must be uniform; default unsigned int */
    int sf = SF_UINT;
    {
        const entry *en = find(es, ne, T_SAMPLEFORMAT);
        if (en) {
            uint32_t *tmp = malloc((size_t)spp * sizeof *tmp);
            if (!tmp) { rc = TIFF_EMEM; goto done; }
            long got = read_ints(fb, n, big, en, tmp, spp);
            if (got >= 1) {
                sf = (int)tmp[0];
                for (long i = 1; i < got; i++) if ((int)tmp[i] != sf) { free(tmp); rc = TIFF_EUNSUPPORTED; goto done; }
            }
            free(tmp);
        }
    }

    tiff_type type;
    if (decode_type(sf, bits, &type) != 0) { rc = TIFF_EUNSUPPORTED; goto done; }
    size_t ts = tiff_type_size(type);

    /* strips */
    const entry *eo = find(es, ne, T_STRIPOFFSETS);
    const entry *ec = find(es, ne, T_STRIPCOUNTS);
    if (!eo || !ec || eo->count == 0 || eo->count != ec->count) { rc = TIFF_EUNSUPPORTED; goto done; }
    uint32_t nstrips = eo->count;
    soff = malloc((size_t)nstrips * sizeof *soff);
    scnt = malloc((size_t)nstrips * sizeof *scnt);
    if (!soff || !scnt) { rc = TIFF_EMEM; goto done; }
    if (read_ints(fb, n, big, eo, soff, nstrips) != (long)nstrips ||
        read_ints(fb, n, big, ec, scnt, nstrips) != (long)nstrips) goto done;

    uint64_t expect = (uint64_t)W * H * spp * ts;
    uint64_t have = 0;
    for (uint32_t i = 0; i < nstrips; i++) {
        if ((uint64_t)soff[i] + scnt[i] > n) goto done;
        have += scnt[i];
    }
    if (have < expect) goto done;   /* not enough pixel data */

    uint8_t *out = malloc((size_t)expect);
    if (!out) { rc = TIFF_EMEM; goto done; }
    uint64_t pos = 0;
    for (uint32_t i = 0; i < nstrips && pos < expect; i++) {
        uint64_t take = scnt[i];
        if (pos + take > expect) take = expect - pos;
        memcpy(out + pos, fb + soff[i], (size_t)take);
        pos += take;
    }

    img->width    = W;
    img->height   = H;
    img->channels = (uint16_t)spp;
    img->type     = type;
    img->data     = out;
    rc = TIFF_OK;

done:
    free(es); free(soff); free(scnt); free(fb);
    return rc;
}

void tiff_free(tiff_image *img) {
    if (!img) return;
    free(img->data);
    memset(img, 0, sizeof *img);
}

/* ============================================================================
 * Multi-page volume read with LZW (Compression=5) + uncompressed (=1) support.
 * ==========================================================================*/

/* TIFF LZW: MSB-first bit packing, 9..12 bit codes, ClearCode 256, EOI 257,
 * "early change" (bump code width one code before the table would overflow),
 * no Predictor. Decodes into dst (capacity dstcap); returns bytes written. */
static long lzw_decode(const uint8_t *src, size_t srclen,
                       uint8_t *dst, size_t dstcap) {
    enum { CLEAR = 256, EOI = 257, MAXCODE = 4096 };
    uint16_t prefix[MAXCODE];
    uint8_t  suffix[MAXCODE];
    uint8_t  stack[MAXCODE];
    size_t   outpos = 0, bitpos = 0, nbits = srclen * 8;
    int bits = 9, next = 258, oldcode = -1, firstchar = 0;

    while (bitpos + (size_t)bits <= nbits) {
        /* read `bits` bits, MSB-first */
        int code = 0;
        for (int i = 0; i < bits; i++) {
            size_t bp = bitpos + i;
            int b = (src[bp >> 3] >> (7 - (bp & 7))) & 1;
            code = (code << 1) | b;
        }
        bitpos += bits;

        if (code == EOI) break;
        if (code == CLEAR) { bits = 9; next = 258; oldcode = -1; continue; }

        int sp = 0, cur = code;
        if (cur >= next) {                 /* KwKwK special case */
            if (oldcode < 0) break;        /* malformed */
            stack[sp++] = (uint8_t)firstchar;
            cur = oldcode;
        }
        while (cur >= 256) { stack[sp++] = suffix[cur]; cur = prefix[cur]; }
        firstchar = cur;
        stack[sp++] = (uint8_t)cur;

        while (sp > 0 && outpos < dstcap) dst[outpos++] = stack[--sp];

        if (oldcode >= 0 && next < MAXCODE) {
            prefix[next] = (uint16_t)oldcode;
            suffix[next] = (uint8_t)firstchar;
            next++;
            if (next == (1 << bits) - 1 && bits < 12) bits++;  /* early change */
        }
        oldcode = code;
        if (outpos >= dstcap) break;
    }
    return (long)outpos;
}

/* Decode one strip (compression 1 = raw copy, 5 = LZW) into dst[..dstcap]. */
static int decode_strip(uint32_t comp, const uint8_t *src, size_t srclen,
                        uint8_t *dst, size_t dstcap, size_t *written) {
    if (comp == 1) {
        size_t t = srclen < dstcap ? srclen : dstcap;
        memcpy(dst, src, t);
        *written = t;
        return TIFF_OK;
    }
    if (comp == 5) {
        long w = lzw_decode(src, srclen, dst, dstcap);
        if (w < 0) return TIFF_EFORMAT;
        *written = (size_t)w;
        return TIFF_OK;
    }
    return TIFF_EUNSUPPORTED;
}

int tiff_read_volume(const char *path, tiff_volume *vol) {
    if (!path || !vol) return TIFF_EINVAL;
    memset(vol, 0, sizeof *vol);

    FILE *f = fopen(path, "rb");
    if (!f) return TIFF_EOPEN;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return TIFF_EIO; }
    long flen = ftell(f);
    if (flen < 8) { fclose(f); return TIFF_EFORMAT; }
    rewind(f);
    uint8_t *fb = malloc((size_t)flen);
    if (!fb) { fclose(f); return TIFF_EMEM; }
    if (fread(fb, 1, (size_t)flen, f) != (size_t)flen) { free(fb); fclose(f); return TIFF_EIO; }
    fclose(f);
    size_t n = (size_t)flen;

    int rc = TIFF_EFORMAT, big;
    entry *es = NULL;
    uint32_t *soff = NULL, *scnt = NULL;
    uint8_t *out = NULL;

    if      (fb[0] == 'I' && fb[1] == 'I') big = 0;
    else if (fb[0] == 'M' && fb[1] == 'M') big = 1;
    else goto done;
    if (rd16(fb + 2, big) != 42) { rc = TIFF_EUNSUPPORTED; goto done; }

    uint32_t W = 0, H = 0, spp = 0, depth = 0;
    tiff_type type = TIFF_U8;
    size_t ts = 0, pagebytes = 0;

    uint32_t ifd = rd32(fb + 4, big);
    while (ifd != 0) {
        if ((uint64_t)ifd + 2 > n) goto done;
        int ne = rd16(fb + ifd, big);
        if (ne <= 0 || ifd + 2 + (uint64_t)ne * 12 + 4 > n) goto done;
        free(es);
        es = malloc((size_t)ne * sizeof *es);
        if (!es) { rc = TIFF_EMEM; goto done; }
        for (int i = 0; i < ne; i++) {
            size_t p = ifd + 2 + (size_t)i * 12;
            es[i].tag = rd16(fb + p, big); es[i].type = rd16(fb + p + 2, big);
            es[i].count = rd32(fb + p + 4, big); es[i].valoff = (uint32_t)(p + 8);
        }
        uint32_t next_ifd = rd32(fb + ifd + 2 + (size_t)ne * 12, big);

        uint32_t comp = read_scalar(fb, n, big, es, ne, T_COMPRESSION, 1);
        if (read_scalar(fb, n, big, es, ne, T_PLANARCONFIG, 1) != 1) { rc = TIFF_EUNSUPPORTED; goto done; }
        if (find(es, ne, T_TILEWIDTH)) { rc = TIFF_EUNSUPPORTED; goto done; }
        uint32_t w = read_scalar(fb, n, big, es, ne, T_WIDTH, 0);
        uint32_t h = read_scalar(fb, n, big, es, ne, T_LENGTH, 0);
        uint32_t s = read_scalar(fb, n, big, es, ne, T_SAMPLESPP, 1);
        if (w == 0 || h == 0 || s == 0 || s > 0xffff) goto done;

        int bits;
        { const entry *en = find(es, ne, T_BITSPERSAMP);
          bits = en ? (int)read_scalar(fb, n, big, es, ne, T_BITSPERSAMP, 1) : 1; }
        int sf = SF_UINT;
        { const entry *en = find(es, ne, T_SAMPLEFORMAT);
          if (en) sf = (int)read_scalar(fb, n, big, es, ne, T_SAMPLEFORMAT, SF_UINT); }
        tiff_type pt;
        if (decode_type(sf, bits, &pt) != 0) { rc = TIFF_EUNSUPPORTED; goto done; }

        if (depth == 0) {
            W = w; H = h; spp = s; type = pt;
            ts = tiff_type_size(type);
            pagebytes = (size_t)W * H * spp * ts;
        } else if (w != W || h != H || s != spp || pt != type) {
            rc = TIFF_EUNSUPPORTED; goto done;   /* pages must be uniform */
        }

        const entry *eo = find(es, ne, T_STRIPOFFSETS);
        const entry *ec = find(es, ne, T_STRIPCOUNTS);
        if (!eo || !ec || eo->count == 0 || eo->count != ec->count) { rc = TIFF_EUNSUPPORTED; goto done; }
        uint32_t nstrips = eo->count;
        free(soff); free(scnt);
        soff = malloc((size_t)nstrips * sizeof *soff);
        scnt = malloc((size_t)nstrips * sizeof *scnt);
        if (!soff || !scnt) { rc = TIFF_EMEM; goto done; }
        if (read_ints(fb, n, big, eo, soff, nstrips) != (long)nstrips ||
            read_ints(fb, n, big, ec, scnt, nstrips) != (long)nstrips) goto done;

        uint8_t *grown = realloc(out, (size_t)(depth + 1) * pagebytes);
        if (!grown) { rc = TIFF_EMEM; goto done; }
        out = grown;
        uint8_t *slice = out + (size_t)depth * pagebytes;

        size_t pos = 0;
        for (uint32_t i = 0; i < nstrips; i++) {
            if ((uint64_t)soff[i] + scnt[i] > n) goto done;
            if (pos >= pagebytes) break;
            size_t wrote = 0;
            int sr = decode_strip(comp, fb + soff[i], scnt[i],
                                  slice + pos, pagebytes - pos, &wrote);
            if (sr != TIFF_OK) { rc = sr; goto done; }
            pos += wrote;
        }
        if (pos != pagebytes) goto done;   /* short page */

        depth++;
        if (next_ifd != 0 && next_ifd <= ifd) goto done;  /* non-monotonic: bail */
        ifd = next_ifd;
    }
    if (depth == 0) goto done;

    vol->width = W; vol->height = H; vol->depth = depth;
    vol->channels = (uint16_t)spp; vol->type = type; vol->data = out;
    out = NULL;
    rc = TIFF_OK;

done:
    free(es); free(soff); free(scnt); free(out); free(fb);
    return rc;
}

void tiff_volume_free(tiff_volume *vol) {
    if (!vol) return;
    free(vol->data);
    memset(vol, 0, sizeof *vol);
}

/* ###########################################################################
 * Custom 2D near-lossless codec (see codec API in tiff.h).
 * ######################################################################### */

/* ============================================================================
 * 64x64 separable float DCT-II / DCT-III (orthonormal).
 *   C[k][n] = a(k)*cos(pi*(2n+1)*k/(2N)), a(0)=sqrt(1/N), a(k)=sqrt(2/N).
 *   forward 2D: F = C B C^T   inverse 2D: B = C^T F C
 * ==========================================================================*/

#define DCTN 64

static float g_C[DCTN][DCTN];   /* DCT matrix, rows indexed by frequency k */
static int   g_C_ready = 0;

static void dct_init(void) {
    if (g_C_ready) return;       /* benign race: writers write identical values */
    const double N = DCTN;
    for (int k = 0; k < DCTN; k++) {
        double a = (k == 0) ? sqrt(1.0 / N) : sqrt(2.0 / N);
        for (int n = 0; n < DCTN; n++)
            g_C[k][n] = (float)(a * cos(M_PI * (2.0 * n + 1.0) * k / (2.0 * N)));
    }
    g_C_ready = 1;
}

/* out = M * in (all NxN row-major) */
static void matmul(const float *restrict M, const float *restrict in, float *restrict out) {
    for (int i = 0; i < DCTN; i++)
        for (int j = 0; j < DCTN; j++) {
            float s = 0.0f; const float *Mr = M + (size_t)i*DCTN;
            for (int n = 0; n < DCTN; n++) s += Mr[n] * in[(size_t)n*DCTN + j];
            out[(size_t)i*DCTN + j] = s;
        }
}
/* out = in * M^T : out[i][j] = sum_n in[i][n]*M[j][n] */
static void matmul_rt(const float *restrict in, const float *restrict M, float *restrict out) {
    for (int i = 0; i < DCTN; i++)
        for (int j = 0; j < DCTN; j++) {
            float s = 0.0f; const float *ir = in + (size_t)i*DCTN, *Mr = M + (size_t)j*DCTN;
            for (int n = 0; n < DCTN; n++) s += ir[n] * Mr[n];
            out[(size_t)i*DCTN + j] = s;
        }
}
/* out = M^T * in : out[i][j] = sum_n M[n][i]*in[n][j] */
static void matmul_lt(const float *restrict M, const float *restrict in, float *restrict out) {
    for (int i = 0; i < DCTN; i++)
        for (int j = 0; j < DCTN; j++) {
            float s = 0.0f;
            for (int n = 0; n < DCTN; n++) s += M[(size_t)n*DCTN + i] * in[(size_t)n*DCTN + j];
            out[(size_t)i*DCTN + j] = s;
        }
}
/* out = in * M : out[i][j] = sum_n in[i][n]*M[n][j] */
static void matmul_rr(const float *restrict in, const float *restrict M, float *restrict out) {
    for (int i = 0; i < DCTN; i++)
        for (int j = 0; j < DCTN; j++) {
            float s = 0.0f; const float *ir = in + (size_t)i*DCTN;
            for (int n = 0; n < DCTN; n++) s += ir[n] * M[(size_t)n*DCTN + j];
            out[(size_t)i*DCTN + j] = s;
        }
}
static void dct64_fwd(const float *restrict B, float *restrict F, float *restrict tmp) {
    matmul(&g_C[0][0], B, tmp);     /* tmp = C B     */
    matmul_rt(tmp, &g_C[0][0], F);  /* F   = tmp C^T */
}
static void dct64_inv(const float *restrict F, float *restrict B, float *restrict tmp) {
    matmul_lt(&g_C[0][0], F, tmp);  /* tmp = C^T F */
    matmul_rr(tmp, &g_C[0][0], B);  /* B   = tmp C */
}

/* ============================================================================
 * Adaptive binary range coder (carry-aware, byte-renormalized) + bypass bits.
 * ==========================================================================*/

typedef struct {
    uint8_t *buf; size_t cap, len;
    uint64_t low; uint32_t range; uint8_t cache; uint64_t cache_size; int oom;
} renc;
typedef struct { const uint8_t *buf; size_t len, pos; uint32_t code, range; } rdec;
typedef struct { uint16_t p0; } bmodel;   /* P(bit==0) in 1/4096 */

#define RC_TOP (1u<<24)

static void bm_init(bmodel *m) { m->p0 = 2048; }
static void renc_init(renc *e) {
    e->cap = 4096; e->len = 0; e->buf = malloc(e->cap);
    e->low = 0; e->range = 0xFFFFFFFFu; e->cache = 0; e->cache_size = 1; e->oom = e->buf ? 0 : 1;
}
static void renc_putbyte(renc *e, uint8_t b) {
    if (e->len >= e->cap) {
        size_t nc = e->cap * 2; uint8_t *nb = realloc(e->buf, nc);
        if (!nb) { e->oom = 1; return; }
        e->buf = nb; e->cap = nc;
    }
    e->buf[e->len++] = b;
}
static void renc_shift_low(renc *e) {
    if ((uint32_t)(e->low >> 32) != 0 || e->low < 0xFF000000ull) {
        uint8_t carry = (uint8_t)(e->low >> 32);
        do { renc_putbyte(e, (uint8_t)(e->cache + carry)); e->cache = 0xFF; } while (--e->cache_size);
        e->cache = (uint8_t)(e->low >> 24);
    }
    e->cache_size++; e->low = (e->low << 8) & 0xFFFFFFFFull;
}
static void renc_bit(renc *e, bmodel *m, int bit) {
    uint32_t r0 = (e->range >> 12) * m->p0;
    if (!bit) { e->range = r0; m->p0 = (uint16_t)(m->p0 + ((4096 - m->p0) >> 5)); }
    else      { e->low += r0; e->range -= r0; m->p0 = (uint16_t)(m->p0 - (m->p0 >> 5)); }
    while (e->range < RC_TOP) { renc_shift_low(e); e->range <<= 8; }
}
static void renc_bypass(renc *e, int bit) {
    e->range >>= 1; if (bit) e->low += e->range;
    while (e->range < RC_TOP) { renc_shift_low(e); e->range <<= 8; }
}
static void renc_bypass_n(renc *e, uint32_t v, int k) {
    while (k > 12) { k -= 12; renc_bypass_n(e, (v >> k) & 0xFFF, 12); }
    while (k-- > 0) renc_bypass(e, (v >> k) & 1);
}
static void renc_flush(renc *e) { for (int i = 0; i < 5; i++) renc_shift_low(e); }

static void rdec_init(rdec *d, const uint8_t *buf, size_t len) {
    d->buf = buf; d->len = len; d->pos = 0; d->code = 0; d->range = 0xFFFFFFFFu;
    for (int i = 0; i < 5; i++) { uint8_t b = (d->pos < d->len) ? d->buf[d->pos++] : 0; d->code = (d->code << 8) | b; }
}
static int rdec_bit(rdec *d, bmodel *m) {
    uint32_t r0 = (d->range >> 12) * m->p0; int bit;
    if (d->code < r0) { d->range = r0; bit = 0; m->p0 = (uint16_t)(m->p0 + ((4096 - m->p0) >> 5)); }
    else { d->code -= r0; d->range -= r0; bit = 1; m->p0 = (uint16_t)(m->p0 - (m->p0 >> 5)); }
    while (d->range < RC_TOP) { uint8_t b = (d->pos < d->len) ? d->buf[d->pos++] : 0; d->code = (d->code << 8) | b; d->range <<= 8; }
    return bit;
}
static int rdec_bypass(rdec *d) {
    d->range >>= 1; int bit = (d->code >= d->range); if (bit) d->code -= d->range;
    while (d->range < RC_TOP) { uint8_t b = (d->pos < d->len) ? d->buf[d->pos++] : 0; d->code = (d->code << 8) | b; d->range <<= 8; }
    return bit;
}
static uint32_t rdec_bypass_n(rdec *d, int k) {
    uint32_t v = 0;
    while (k > 12) { k -= 12; v = (v << 12) | rdec_bypass_n(d, 12); }
    while (k-- > 0) v = (v << 1) | rdec_bypass(d);
    return v;
}
static void enc_eg(renc *e, uint32_t v) {
    uint32_t nb = 0, t = v + 1; while (t > 1) { t >>= 1; nb++; }
    for (uint32_t i = 0; i < nb; i++) renc_bypass(e, 1);
    renc_bypass(e, 0);
    if (nb) renc_bypass_n(e, (v + 1) & ((1u << nb) - 1), (int)nb);
}
static uint32_t dec_eg(rdec *d) {
    uint32_t nb = 0; while (rdec_bypass(d)) nb++;
    if (!nb) return 0;
    return ((1u << nb) | rdec_bypass_n(d, (int)nb)) - 1;
}

/* ============================================================================
 * Zigzag scan + frequency bands for a 64x64 block.
 * ==========================================================================*/

#define NCOEF  (DCTN*DCTN)
#define NBANDS 8
static uint16_t g_scan[NCOEF];   /* scan order -> linear coef index (i*64+j) */
static uint8_t  g_band[NCOEF];   /* band of each scan position */
static int      g_scan_ready = 0;

static void scan_init(void) {
    if (g_scan_ready) return;
    int p = 0;
    for (int d = 0; d < 2*DCTN-1; d++) {     /* anti-diagonals, alternating direction */
        if ((d & 1) == 0)
            for (int i = (d < DCTN ? d : DCTN-1); i >= (d-DCTN+1 > 0 ? d-DCTN+1 : 0); i--)
                { int j = d - i; g_scan[p++] = (uint16_t)(i*DCTN + j); }
        else
            for (int i = (d-DCTN+1 > 0 ? d-DCTN+1 : 0); i <= (d < DCTN ? d : DCTN-1); i++)
                { int j = d - i; g_scan[p++] = (uint16_t)(i*DCTN + j); }
    }
    for (int s = 0; s < NCOEF; s++) {
        int idx = g_scan[s], i = idx / DCTN, j = idx % DCTN;
        int b = (i + j) * NBANDS / (2*DCTN - 1);
        g_band[s] = (uint8_t)(b < NBANDS ? b : NBANDS-1);
    }
    g_scan_ready = 1;
}

/* ============================================================================
 * Per-block coefficient coding: EOB + per-band significance + magnitude + sign.
 * ==========================================================================*/

#define MAGCTX 12
typedef struct { bmodel sig[NBANDS]; bmodel mag[MAGCTX]; bmodel eob[14]; } blkctx;
static void blkctx_init(blkctx *c) {
    for (int i = 0; i < NBANDS; i++) bm_init(&c->sig[i]);
    for (int i = 0; i < MAGCTX;  i++) bm_init(&c->mag[i]);
    for (int i = 0; i < 14;      i++) bm_init(&c->eob[i]);
}
static void enc_eob(renc *e, blkctx *c, uint32_t v) {
    int kmax = 0; while ((1u<<kmax) <= (uint32_t)NCOEF) kmax++;
    int k = 0; while ((1u<<k) <= v) k++;
    for (int i = 0; i < k; i++) renc_bit(e, &c->eob[i], 1);
    if (k < kmax) renc_bit(e, &c->eob[k], 0);
    if (k > 1) renc_bypass_n(e, v & ((1u<<(k-1))-1), k-1);
}
static uint32_t dec_eob(rdec *d, blkctx *c) {
    int kmax = 0; while ((1u<<kmax) <= (uint32_t)NCOEF) kmax++;
    int k = 0; while (k < kmax && rdec_bit(d, &c->eob[k])) k++;
    if (k == 0) return 0;
    if (k == 1) return 1;
    return (1u<<(k-1)) | rdec_bypass_n(d, k-1);
}
static void enc_mag(renc *e, blkctx *c, uint32_t m) {   /* m >= 1 */
    uint32_t v = m - 1, k = 0;
    while (k < (uint32_t)(MAGCTX-1) && v > 0) {
        renc_bit(e, &c->mag[k], 1); v--; k++;
        if (v == 0) { renc_bit(e, &c->mag[k], 0); return; }
    }
    if (v == 0) { renc_bit(e, &c->mag[k], 0); return; }
    renc_bit(e, &c->mag[k], 1);
    enc_eg(e, v);
}
static uint32_t dec_mag(rdec *d, blkctx *c) {
    uint32_t v = 0, k = 0;
    while (k < (uint32_t)(MAGCTX-1)) { if (rdec_bit(d, &c->mag[k])) { v++; k++; } else return v + 1; }
    if (!rdec_bit(d, &c->mag[k])) return v + 1;
    return v + dec_eg(d) + 1;
}
static void enc_block(renc *e, blkctx *c, const int32_t *q) {
    const uint16_t *scan = g_scan;
    uint32_t eob = 0;
    for (uint32_t p = NCOEF; p-- > 0; ) if (q[scan[p]] != 0) { eob = p + 1; break; }
    enc_eob(e, c, eob);
    for (uint32_t p = 0; p < eob; p++) {
        int32_t v = q[scan[p]];
        if (p != eob - 1) renc_bit(e, &c->sig[g_band[p]], v != 0);
        if (v == 0) continue;
        uint32_t m = (uint32_t)(v < 0 ? -(int64_t)v : v);
        enc_mag(e, c, m);
        renc_bypass(e, v < 0);
    }
}
static void dec_block(rdec *d, blkctx *c, int32_t *q) {
    const uint16_t *scan = g_scan;
    memset(q, 0, NCOEF * sizeof *q);
    uint32_t eob = dec_eob(d, c); if (eob > (uint32_t)NCOEF) eob = NCOEF;
    for (uint32_t p = 0; p < eob; p++) {
        int sig = (p == eob - 1) ? 1 : rdec_bit(d, &c->sig[g_band[p]]);
        if (!sig) continue;
        uint32_t m = dec_mag(d, c);
        int neg = rdec_bypass(d);
        q[scan[p]] = neg ? -(int32_t)m : (int32_t)m;
    }
}

/* 64-bit Exp-Golomb (bypass) for correction gaps/values. */
static void enc_eg64(renc *e, uint64_t v) {
    uint64_t nb = 0, t = v + 1; while (t > 1) { t >>= 1; nb++; }
    for (uint64_t i = 0; i < nb; i++) renc_bypass(e, 1);
    renc_bypass(e, 0);
    for (uint64_t i = 0; i < nb; i++) renc_bypass(e, (int)(((v + 1) >> (nb-1-i)) & 1));
}
static uint64_t dec_eg64(rdec *d) {
    uint64_t nb = 0; while (rdec_bypass(d)) nb++;
    if (!nb) return 0;
    uint64_t x = 1; for (uint64_t i = 0; i < nb; i++) x = (x << 1) | (uint64_t)rdec_bypass(d);
    return x - 1;
}
static void enc_sval(renc *e, int64_t v) { enc_eg64(e, ((uint64_t)v << 1) ^ (uint64_t)(v >> 63)); }
static int64_t dec_sval(rdec *d) { uint64_t u = dec_eg64(d); return (int64_t)(u >> 1) ^ -(int64_t)(u & 1); }

/* ============================================================================
 * Sample-type I/O barrier (the only place integers meet the float pipeline).
 * ==========================================================================*/

static float f16_to_f32(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1f, man = h & 0x3ff, f;
    if (exp == 0) {
        if (man == 0) f = sign;
        else { int e = -1; do { e++; man <<= 1; } while (!(man & 0x400)); man &= 0x3ff;
               f = sign | (uint32_t)((127 - 15 - e) << 23) | (man << 13); }
    } else if (exp == 0x1f) f = sign | 0x7f800000u | (man << 13);
    else f = sign | (uint32_t)((int)exp - 15 + 127) << 23 | (man << 13);
    float out; memcpy(&out, &f, 4); return out;
}
static uint16_t f32_to_f16(float v) {
    uint32_t x; memcpy(&x, &v, 4);
    uint32_t sign = (x >> 16) & 0x8000u, man = x & 0x7fffffu; int exp = (int)((x >> 23) & 0xff);
    if (exp == 0xff) return (uint16_t)(sign | 0x7c00u | (man ? 0x200u : 0));   /* inf/nan */
    int e = exp - 127 + 15;
    if (e >= 0x1f) return (uint16_t)(sign | 0x7c00u);                          /* overflow -> inf */
    if (e <= 0) {
        if (e < -10) return (uint16_t)sign;                                    /* underflow -> 0 */
        man |= 0x800000u; int shift = 14 - e; uint32_t h = man >> shift;
        if ((man >> (shift - 1)) & 1) h++;                                     /* round half up */
        return (uint16_t)(sign | h);
    }
    uint16_t h = (uint16_t)(sign | (uint32_t)(e << 10) | (man >> 13));
    if (man & 0x1000u) h++;                                                    /* round */
    return h;
}
static int    is_float_type(tiff_type t) { return t == TIFF_F16 || t == TIFF_F32; }
static double type_min(tiff_type t) {
    switch (t) { case TIFF_S8: return -128; case TIFF_S16: return -32768;
                 case TIFF_S32: return -2147483648.0; default: return 0; }
}
static double type_max(tiff_type t) {
    switch (t) { case TIFF_U8: return 255; case TIFF_U16: return 65535;
                 case TIFF_U32: return 4294967295.0; case TIFF_S8: return 127;
                 case TIFF_S16: return 32767; case TIFF_S32: return 2147483647.0;
                 default: return 0; }
}
static double load_d(const void *base, tiff_type t, size_t i) {
    switch (t) {
        case TIFF_U8:  return ((const uint8_t  *)base)[i];
        case TIFF_U16: return ((const uint16_t *)base)[i];
        case TIFF_U32: return ((const uint32_t *)base)[i];
        case TIFF_S8:  return ((const int8_t   *)base)[i];
        case TIFF_S16: return ((const int16_t  *)base)[i];
        case TIFF_S32: return ((const int32_t  *)base)[i];
        case TIFF_F16: return f16_to_f32(((const uint16_t *)base)[i]);
        case TIFF_F32: return ((const float    *)base)[i];
    }
    return 0;
}
static void store_d(void *base, tiff_type t, size_t i, double v) {
    if (is_float_type(t)) {
        if (t == TIFF_F16) ((uint16_t *)base)[i] = f32_to_f16((float)v);
        else               ((float    *)base)[i] = (float)v;
        return;
    }
    double r = nearbyint(v), lo = type_min(t), hi = type_max(t);
    if (r < lo) r = lo;
    if (r > hi) r = hi;
    switch (t) {
        case TIFF_U8:  ((uint8_t  *)base)[i] = (uint8_t )r; break;
        case TIFF_U16: ((uint16_t *)base)[i] = (uint16_t)r; break;
        case TIFF_U32: ((uint32_t *)base)[i] = (uint32_t)r; break;
        case TIFF_S8:  ((int8_t   *)base)[i] = (int8_t  )r; break;
        case TIFF_S16: ((int16_t  *)base)[i] = (int16_t )r; break;
        case TIFF_S32: ((int32_t  *)base)[i] = (int32_t )r; break;
        default: break;
    }
}
/* the float reconstruction a sample resolves to before corrections (must be
 * identical encoder/decoder): int types round+clamp; f16 rounds through half. */
static double recon_base(float rf, tiff_type t) {
    if (t == TIFF_F32) return rf;
    if (t == TIFF_F16) return f16_to_f32(f32_to_f16(rf));
    double r = nearbyint((double)rf), lo = type_min(t), hi = type_max(t);
    if (r < lo) r = lo;
    if (r > hi) r = hi;
    return r;
}

/* ============================================================================
 * Container byte buffer (write) + cursor reader.
 * ==========================================================================*/

typedef struct { uint8_t *p; size_t len, cap; int oom; } bbuf;
static void bb_init(bbuf *b) { b->cap = 4096; b->len = 0; b->p = malloc(b->cap); b->oom = b->p ? 0 : 1; }
static void bb_putn(bbuf *b, const void *src, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap : 1; while (nc < b->len + n) nc *= 2;
        uint8_t *np = realloc(b->p, nc); if (!np) { b->oom = 1; return; } b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->len, src, n); b->len += n;
}
static void bb_u8 (bbuf *b, uint8_t v)  { bb_putn(b, &v, 1); }
static void bb_u16(bbuf *b, uint16_t v) { uint8_t t[2] = { (uint8_t)v, (uint8_t)(v>>8) }; bb_putn(b, t, 2); }
static void bb_u32(bbuf *b, uint32_t v) { uint8_t t[4] = { (uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24) }; bb_putn(b, t, 4); }
static void bb_f32(bbuf *b, float f)    { uint32_t u; memcpy(&u, &f, 4); bb_u32(b, u); }

typedef struct { const uint8_t *p; size_t len, pos; int err; } brd;
static uint8_t  rd_u8 (brd *r) { if (r->pos + 1 > r->len) { r->err = 1; return 0; } return r->p[r->pos++]; }
static uint16_t rd_u16(brd *r) { uint16_t a = rd_u8(r), b = rd_u8(r); return (uint16_t)(a | (b<<8)); }
static uint32_t rd_u32(brd *r) { uint32_t a = rd_u16(r), b = rd_u16(r); return a | (b<<16); }
static float    rd_f32(brd *r) { uint32_t u = rd_u32(r); float f; memcpy(&f, &u, 4); return f; }

/* ============================================================================
 * Per-plane encode / decode.
 * ==========================================================================*/

/* Per-plane the encoder searches the quantization step over a geometric set and
 * keeps the smallest output. The correction pass enforces the tau bound for ANY
 * step, so this is exactly "maximize compression subject to max-error <= tau":
 * a coarse step shrinks the coefficient stream but grows corrections, and the
 * search finds the per-plane sweet spot instead of a fixed guess. */
#define STEP_NCAND 8     /* candidate steps: base * 2^k, k = 0 .. STEP_NCAND-1 */

/* reconstruct one block's spatial values from quantized levels (shared by
 * encoder's correction pass and the decoder, so they agree bit-for-bit). */
static void block_dequant_idct(const int32_t *q, float step, float *rblk, float *F, float *tmp) {
    for (int i = 0; i < NCOEF; i++) F[i] = (float)q[i] * step;
    dct64_inv(F, rblk, tmp);
}

/* Encode the centered plane at one `step` into a fresh range-coder buffer
 * (*buf,*len; caller frees). Blocks then the tau-bounded correction list. */
static int plane_stream(const float *plane, float *recon, const double *orig,
                        uint32_t W, uint32_t H, tiff_type t,
                        float step, float cq, float fmean, double tau_abs,
                        uint8_t **buf, size_t *len) {
    size_t n = (size_t)W * H;
    renc e; renc_init(&e);
    if (e.oom) { free(e.buf); return TIFF_EMEM; }
    blkctx c; blkctx_init(&c);
    float blk[NCOEF], F[NCOEF], tmp[NCOEF], rblk[NCOEF]; int32_t q[NCOEF];
    int bx = (int)((W + 63) / 64), by = (int)((H + 63) / 64);
    float rstep = 1.0f / step;
    for (int byi = 0; byi < by; byi++)
        for (int bxi = 0; bxi < bx; bxi++) {
            for (int r = 0; r < 64; r++) {
                int py = byi*64 + r; if (py >= (int)H) py = (int)H - 1;
                for (int cc = 0; cc < 64; cc++) {
                    int px = bxi*64 + cc; if (px >= (int)W) px = (int)W - 1;
                    blk[r*64 + cc] = plane[(size_t)py*W + px];
                }
            }
            dct64_fwd(blk, F, tmp);
            for (int i = 0; i < NCOEF; i++) q[i] = (int32_t)lrintf(F[i] * rstep);
            enc_block(&e, &c, q);
            block_dequant_idct(q, step, rblk, F, tmp);
            for (int r = 0; r < 64; r++) {
                int py = byi*64 + r; if (py >= (int)H) continue;
                for (int cc = 0; cc < 64; cc++) {
                    int px = bxi*64 + cc; if (px >= (int)W) continue;
                    recon[(size_t)py*W + px] = rblk[r*64 + cc] + fmean;
                }
            }
        }
    bmodel mflag; bm_init(&mflag);
    long long last = -1;
    for (size_t i = 0; i < n; i++) {
        double err = orig[i] - recon_base(recon[i], t);
        if (fabs(err) <= tau_abs) continue;
        int64_t eq = is_float_type(t) ? (int64_t)llrint(err / (double)cq) : (int64_t)llrint(err);
        if (eq == 0) continue;
        renc_bit(&e, &mflag, 1);
        enc_eg64(&e, (uint64_t)(i - (size_t)(last + 1)));
        enc_sval(&e, eq);
        last = (long long)i;
    }
    renc_bit(&e, &mflag, 0);
    renc_flush(&e);
    if (e.oom) { free(e.buf); return TIFF_EMEM; }
    *buf = e.buf; *len = e.len;
    return TIFF_OK;
}

static int encode_plane(bbuf *out, const void *base, tiff_type t,
                        size_t stride, size_t ch_off, uint32_t W, uint32_t H,
                        float tau_frac, float step_override) {
    size_t n = (size_t)W * H;
    double *orig = malloc(n * sizeof *orig);
    float  *plane = malloc(n * sizeof *plane);
    float  *recon = malloc(n * sizeof *recon);
    if (!orig || !plane || !recon) { free(orig); free(plane); free(recon); return TIFF_EMEM; }

    double mn = orig[0] = load_d(base, t, 0*stride + ch_off), mx = mn, mean = 0;
    for (size_t i = 0; i < n; i++) {
        double v = load_d(base, t, i*stride + ch_off);
        orig[i] = v; mean += v; if (v < mn) mn = v; if (v > mx) mx = v;
    }
    mean /= (double)n;
    double R = mx - mn;

    if (R <= 0.0) {                       /* constant plane */
        bb_u8(out, 1); bb_f32(out, (float)mean);
        free(orig); free(plane); free(recon);
        return out->oom ? TIFF_EMEM : TIFF_OK;
    }

    float fmean = (float)mean;
    for (size_t i = 0; i < n; i++) plane[i] = (float)(orig[i] - mean);
    double tau_abs = (double)tau_frac * R;
    float cq = is_float_type(t) ? (float)fmax(tau_abs, 1e-12) : 1.0f;

    dct_init(); scan_init();

    /* search the step; keep the smallest stream */
    float base_step = (float)fmax(tau_abs, 1e-9);
    uint8_t *best = NULL; size_t best_len = (size_t)-1; float best_step = base_step;
    int ncand = (step_override > 0) ? 1 : STEP_NCAND;
    int rc = TIFF_OK;
    for (int k = 0; k < ncand; k++) {
        float step = (step_override > 0) ? step_override : base_step * (float)(1u << k);
        uint8_t *buf; size_t len;
        rc = plane_stream(plane, recon, orig, W, H, t, step, cq, fmean, tau_abs, &buf, &len);
        if (rc != TIFF_OK) { free(best); break; }
        if (len < best_len) { free(best); best = buf; best_len = len; best_step = step; }
        else free(buf);
    }
    if (rc != TIFF_OK) { free(orig); free(plane); free(recon); return rc; }

    bb_u8(out, 0); bb_f32(out, fmean); bb_f32(out, best_step); bb_f32(out, cq);
    bb_u32(out, (uint32_t)best_len); bb_putn(out, best, best_len);

    free(best); free(orig); free(plane); free(recon);
    return out->oom ? TIFF_EMEM : TIFF_OK;
}

static int decode_plane(brd *r, void *base, tiff_type t,
                        size_t stride, size_t ch_off, uint32_t W, uint32_t H) {
    size_t n = (size_t)W * H;
    int flag = rd_u8(r);
    float fmean = rd_f32(r);
    if (r->err) return TIFF_EFORMAT;
    if (flag == 1) {                          /* constant plane */
        for (size_t i = 0; i < n; i++) store_d(base, t, i*stride + ch_off, fmean);
        return TIFF_OK;
    }
    float step = rd_f32(r), cq = rd_f32(r);
    uint32_t slen = rd_u32(r);
    if (r->err || r->pos + slen > r->len) return TIFF_EFORMAT;
    const uint8_t *stream = r->p + r->pos; r->pos += slen;

    float *recon = malloc(n * sizeof *recon);
    if (!recon) return TIFF_EMEM;

    dct_init(); scan_init();
    rdec d; rdec_init(&d, stream, slen); blkctx c; blkctx_init(&c);
    float F[NCOEF], tmp[NCOEF], rblk[NCOEF]; int32_t q[NCOEF];
    int bx = (int)((W + 63) / 64), by = (int)((H + 63) / 64);
    for (int byi = 0; byi < by; byi++)
        for (int bxi = 0; bxi < bx; bxi++) {
            dec_block(&d, &c, q);
            block_dequant_idct(q, step, rblk, F, tmp);
            for (int rr = 0; rr < 64; rr++) {
                int py = byi*64 + rr; if (py >= (int)H) continue;
                for (int cc = 0; cc < 64; cc++) {
                    int px = bxi*64 + cc; if (px >= (int)W) continue;
                    recon[(size_t)py*W + px] = rblk[rr*64 + cc] + fmean;
                }
            }
        }
    /* base reconstruction -> samples */
    for (size_t i = 0; i < n; i++)
        store_d(base, t, i*stride + ch_off, recon_base(recon[i], t));
    free(recon);

    /* apply corrections */
    bmodel mflag; bm_init(&mflag);
    size_t pos = 0;
    while (rdec_bit(&d, &mflag)) {
        pos += (size_t)dec_eg64(&d);
        int64_t eq = dec_sval(&d);
        if (pos >= n) return TIFF_EFORMAT;
        double cur = load_d(base, t, pos*stride + ch_off);
        double nv = is_float_type(t) ? cur + (double)eq * (double)cq : cur + (double)eq;
        store_d(base, t, pos*stride + ch_off, nv);
        pos++;
    }
    return TIFF_OK;
}

/* ============================================================================
 * Public API: container = header + per-channel plane blobs.
 * ==========================================================================*/

#define TZ_MAGIC0 'T'
#define TZ_MAGIC1 'Z'
#define TZ_MAGIC2 '2'
#define TZ_MAGIC3 'D'

int tiff_compress(const tiff_image *img, tiff_codec_params p,
                  uint8_t **out, size_t *outlen) {
    if (!img || !img->data || !out || !outlen) return TIFF_EINVAL;
    if (img->width == 0 || img->height == 0 || img->channels == 0) return TIFF_EINVAL;
    if (tiff_type_size(img->type) == 0) return TIFF_EINVAL;
    if (!(p.tau >= 0.0f)) return TIFF_EINVAL;

    bbuf b; bb_init(&b);
    if (b.oom) return TIFF_EMEM;
    bb_u8(&b, TZ_MAGIC0); bb_u8(&b, TZ_MAGIC1); bb_u8(&b, TZ_MAGIC2); bb_u8(&b, TZ_MAGIC3);
    bb_u8(&b, 1);                         /* version */
    bb_u8(&b, (uint8_t)img->type);
    bb_u16(&b, img->channels);
    bb_u32(&b, img->width);
    bb_u32(&b, img->height);

    int rc = TIFF_OK;
    for (uint16_t ch = 0; ch < img->channels && rc == TIFF_OK; ch++)
        rc = encode_plane(&b, img->data, img->type, img->channels, ch,
                          img->width, img->height, p.tau, p.step);

    if (rc != TIFF_OK || b.oom) { free(b.p); return rc == TIFF_OK ? TIFF_EMEM : rc; }
    *out = b.p; *outlen = b.len;
    return TIFF_OK;
}

int tiff_decompress(const uint8_t *in, size_t inlen, tiff_image *img) {
    if (!in || !img) return TIFF_EINVAL;
    memset(img, 0, sizeof *img);
    brd r = { in, inlen, 0, 0 };
    if (rd_u8(&r) != TZ_MAGIC0 || rd_u8(&r) != TZ_MAGIC1 ||
        rd_u8(&r) != TZ_MAGIC2 || rd_u8(&r) != TZ_MAGIC3) return TIFF_EFORMAT;
    if (rd_u8(&r) != 1) return TIFF_EUNSUPPORTED;          /* version */
    tiff_type type = (tiff_type)rd_u8(&r);
    uint16_t channels = rd_u16(&r);
    uint32_t W = rd_u32(&r), H = rd_u32(&r);
    if (r.err) return TIFF_EFORMAT;
    size_t ts = tiff_type_size(type);
    if (ts == 0 || channels == 0 || W == 0 || H == 0) return TIFF_EFORMAT;

    size_t n = (size_t)W * H * channels;
    void *data = malloc(n * ts);
    if (!data) return TIFF_EMEM;

    int rc = TIFF_OK;
    for (uint16_t ch = 0; ch < channels && rc == TIFF_OK; ch++)
        rc = decode_plane(&r, data, type, channels, ch, W, H);

    if (rc != TIFF_OK) { free(data); return rc; }
    img->width = W; img->height = H; img->channels = channels; img->type = type;
    img->data = data;
    return TIFF_OK;
}
