/* tiff.h — minimal, dependency-free TIFF reader/writer.
 *
 * Scope: write and read plain *uncompressed* (Compression=1) baseline TIFF so
 * the files open in any normal viewer (ImageJ/Fiji, GIMP, macOS Preview,
 * Photoshop, libtiff-based tools, ...). No upstream codecs (LZW/Deflate/PackBits
 * etc.) are implemented — a custom codec lives outside this library.
 *
 * Pixel model: row-major, chunky (interleaved) samples, one or more channels
 * per pixel, all channels the same sample type. Supported sample types:
 *
 *     u8  u16  u32        (SampleFormat = unsigned integer)
 *     s8  s16  s32        (SampleFormat = signed integer)
 *     f16 f32             (SampleFormat = IEEE float)
 *
 * The writer emits a single uncompressed strip, little-endian, one IFD, with the
 * tags a baseline reader needs. Photometric is BlackIsZero for 1 channel, RGB
 * for 3/4 channels (4th = alpha), and BlackIsZero + ExtraSamples otherwise.
 *
 * The reader parses real-world uncompressed TIFFs: either byte order, single or
 * multi-strip, SHORT or LONG offset/count tags. It rejects what it can't safely
 * hand back: compressed, tiled, planar (PlanarConfiguration=2), BigTIFF, mixed
 * bit depths, or unsupported (BitsPerSample, SampleFormat) combinations.
 *
 * All read/write is via stdio (no mmap). tiff_read allocates img->data; release
 * it with tiff_free.
 */
#ifndef TIFF_H
#define TIFF_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TIFF_U8  = 0,   /* 8-bit  unsigned int   */
    TIFF_U16 = 1,   /* 16-bit unsigned int   */
    TIFF_U32 = 2,   /* 32-bit unsigned int   */
    TIFF_S8  = 3,   /* 8-bit  signed int     */
    TIFF_S16 = 4,   /* 16-bit signed int     */
    TIFF_S32 = 5,   /* 32-bit signed int     */
    TIFF_F16 = 6,   /* 16-bit IEEE half float */
    TIFF_F32 = 7,   /* 32-bit IEEE float     */
} tiff_type;

/* Return codes: 0 on success, negative on failure. */
enum {
    TIFF_OK          =  0,
    TIFF_EOPEN       = -1,   /* could not open/create the file            */
    TIFF_EIO         = -2,   /* short read/write or stdio error           */
    TIFF_EFORMAT     = -3,   /* not a TIFF / malformed structure          */
    TIFF_EUNSUPPORTED= -4,   /* valid TIFF but a layout we don't handle   */
    TIFF_EMEM        = -5,   /* allocation failed                         */
    TIFF_EINVAL      = -6,   /* bad argument to the API                   */
};

typedef struct {
    uint32_t  width;     /* pixels per row                                */
    uint32_t  height;    /* number of rows                                */
    uint16_t  channels;  /* samples per pixel (>= 1)                      */
    tiff_type type;      /* sample type, shared by all channels           */
    void     *data;      /* width*height*channels samples, row-major chunky */
} tiff_image;

/* Bytes in one sample of `t` (1, 2, or 4). */
size_t tiff_type_size(tiff_type t);

/* Human-readable name for a return code. */
const char *tiff_strerror(int code);

/* Write `img` as a single uncompressed little-endian strip. img->data must hold
 * width*height*channels*tiff_type_size(type) bytes. Returns TIFF_OK or < 0. */
int tiff_write(const char *path, const tiff_image *img);

/* Read an uncompressed TIFF. On success fills *img and allocates img->data
 * (free with tiff_free). On failure *img is zeroed and returns < 0. */
int tiff_read(const char *path, tiff_image *img);

/* Free img->data and zero the struct. Safe on a zeroed/already-freed image. */
void tiff_free(tiff_image *img);

/* ---- multi-page volume read (z-stack of equally sized pages) ----------------
 * Reads a multi-IFD TIFF where each IFD is one z-slice, into a single z-major
 * volume: data holds depth*height*width*channels samples, slice-major then
 * row-major chunky, i.e. idx = ((z*height + y)*width + x)*channels + c. Supports
 * baseline stripped TIFF with Compression = 1 (none) or 5 (LZW, MSB-first,
 * Predictor=none), single PlanarConfiguration, either byte order — the layout the
 * Vesuvius surface-detection volumes use. All pages must share
 * width/height/channels/type. Allocates vol->data; free with tiff_volume_free. */
typedef struct {
    uint32_t  width;     /* x, fastest                                    */
    uint32_t  height;    /* y                                             */
    uint32_t  depth;     /* z, slowest (number of pages / IFDs)           */
    uint16_t  channels;  /* samples per pixel (>= 1)                      */
    tiff_type type;      /* sample type, shared by all pages              */
    void     *data;      /* depth*height*width*channels samples           */
} tiff_volume;

int  tiff_read_volume(const char *path, tiff_volume *vol);
void tiff_volume_free(tiff_volume *vol);

/* ===========================================================================
 * Custom 2D near-lossless codec (NOT standard TIFF; not viewable in a normal
 * viewer). Per-plane compression on a 64x64 float DCT: all transform math is
 * float, integers appear only at the sample-type I/O barrier and as quantized
 * coefficient levels. Every reconstructed sample is within `tau` of the
 * original; within that bound the codec compresses as hard as the data allows.
 * Output is a self-described blob decodable only by tiff_decompress.
 * =========================================================================*/

typedef struct {
    /* Hard ceiling on per-sample reconstruction error, as a FRACTION of each
     * plane's value range (max-min). e.g. 0.05 == within 5% of the plane range.
     * Typical presets: 0.01 0.02 0.05 0.08 0.16 0.32 0.64. Achieved ratio
     * depends on the data (smooth planes compress far harder than noisy ones). */
    float tau;
    /* Manual DCT quantization step. 0 = derive automatically from tau (normal
     * mode). Nonzero pins the step; the tau bound is still enforced. */
    float step;
} tiff_codec_params;

/* 5% error bound, automatic step. */
#define TIFF_CODEC_DEFAULTS ((tiff_codec_params){ .tau = 0.05f, .step = 0.0f })

/* Compress `img` into a freshly malloc'd buffer (*out, *outlen; caller frees
 * *out). Returns TIFF_OK or a negative tiff error code. */
int tiff_compress(const tiff_image *img, tiff_codec_params p,
                  uint8_t **out, size_t *outlen);

/* Decompress a tiff_compress blob into *img (allocates img->data; free with
 * tiff_free). Returns TIFF_OK or a negative tiff error code. */
int tiff_decompress(const uint8_t *in, size_t inlen, tiff_image *img);

#ifdef __cplusplus
}
#endif

#endif /* TIFF_H */
