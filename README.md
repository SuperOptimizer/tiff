# tiff

A minimal, dependency-free TIFF reader/writer in one `.h` + `.c`.

Writes plain **uncompressed** baseline TIFF that opens in any normal viewer
(ImageJ/Fiji, GIMP, Preview, Photoshop, libtiff tools). No upstream codecs
(LZW/Deflate/PackBits) — a custom codec lives outside this library.

## Pixel model

Row-major, chunky (interleaved) samples, one or more channels per pixel, all
channels the same sample type:

| type | SampleFormat | bits |
|------|--------------|------|
| `TIFF_U8` `TIFF_U16` `TIFF_U32` | unsigned int | 8 / 16 / 32 |
| `TIFF_S8` `TIFF_S16` `TIFF_S32` | signed int   | 8 / 16 / 32 |
| `TIFF_F16` `TIFF_F32`           | IEEE float   | 16 / 32     |

Photometric is BlackIsZero for 1 channel, RGB for 3/4 channels (4th = alpha),
and BlackIsZero + ExtraSamples otherwise.

## API

```c
#include "tiff.h"

tiff_image img = { .width=W, .height=H, .channels=C, .type=TIFF_F32, .data=px };
tiff_write("out.tif", &img);                 // single uncompressed strip, LE

tiff_image in;
if (tiff_read("in.tif", &in) == TIFF_OK) {   // allocates in.data
    ... use in.data (W*H*C samples, row-major chunky) ...
    tiff_free(&in);
}
```

The reader accepts real-world uncompressed TIFFs (either byte order, single or
multi-strip, SHORT/LONG tags) and cleanly rejects what it won't decode:
compressed, tiled, planar, BigTIFF, mixed bit depths.

## Custom 2D near-lossless codec

Separate from the viewable TIFF path, `tiff_compress` / `tiff_decompress`
implement a private per-plane codec on a **64×64 float DCT**. All transform math
is float; integers appear only at the sample-type I/O barrier and as quantized
coefficient levels. It is **not** standard TIFF — the output is a self-described
blob a normal viewer can't read.

```c
#include "tiff.h"

tiff_codec_params p = { .tau = 0.05f, .step = 0.0f };  /* 5% error bound, auto step */
uint8_t *blob; size_t n;
tiff_compress(&img, p, &blob, &n);

tiff_image out;
tiff_decompress(blob, n, &out);   /* allocates out.data; tiff_free when done */
free(blob);
```

`tau` is a **hard ceiling** on per-sample error, expressed as a fraction of each
plane's value range (max−min): every reconstructed sample is within `tau·range`
of the original. Typical presets `0.01 0.02 0.05 0.08 0.16 0.32 0.64`; achieved
ratio depends on the data (smooth planes hit 100×–1000×+, noisy planes far less).
All eight sample types are supported; the error bound is enforced by a correction
pass, so even `u32`/`s32` data whose low bits don't survive the float transform
round-trips within `tau`.

Within the bound the codec compresses as hard as the data allows: per plane the
encoder **searches the quantization step** (geometric sweep) and keeps the
smallest result — a coarse step shrinks the coefficient stream but grows
corrections, and the search finds the sweet spot rather than a fixed guess. This
costs up to ~8× on encode (decode is single-pass and unaffected). Set
`params.step` nonzero to pin the step and skip the search.

## Build & test

Standalone:

```sh
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

That runs the round-trip test (all types × channel counts) and the reader
hardening test. Under ASan/UBSan: configure with
`-DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all"`.

As a dependency: `add_subdirectory(tiff)` then link the `tiff` target (this puts
`tiff.h` on the include path).

`fuzz_tiff.c` / `fuzz_tiff_seed.c` are an AFL++/libFuzzer harness + corpus seed
for the untrusted-input reader path.
