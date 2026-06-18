/* fuzz_tiff — fuzz the TIFF reader against arbitrary/corrupted file bytes.
 *
 * tiff_read parses an UNTRUSTED file and, on success, hands back an owned buffer
 * of width*height*channels*type_size bytes. The contract: for ANY input bytes,
 * read either rejects (rc < 0) or returns a self-consistent buffer of exactly
 * that size — never a crash, never a short buffer. This is the executable form
 * of that contract (enforced under ASan+UBSan): on accept we touch every
 * advertised byte, so any size drift faults here.
 *
 *   AFL_USE_ASAN=1 AFL_USE_UBSAN=1 afl-clang-fast \
 *     fuzz_tiff.c tiff.c -O1 -g -o fuzz_tiff
 *   afl-fuzz -i corpus -o findings -- ./fuzz_tiff @@
 *
 * Standalone replay (CI smoke / crash replay): -DTIFF_FUZZ_STANDALONE.
 */
#include "tiff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

static void exercise(const uint8_t *data, size_t size) {
    if (size < 8 || size > (1u<<24)) return;

    char path[] = "/tmp/fuzz_tiff_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return;
    if (write(fd, data, size) != (ssize_t)size) { close(fd); unlink(path); return; }
    close(fd);

    tiff_image t;
    if (tiff_read(path, &t) == TIFF_OK) {
        if (t.width == 0 || t.height == 0 || t.channels < 1) abort();
        if (!t.data) abort();
        size_t want = (size_t)t.width * t.height * t.channels * tiff_type_size(t.type);
        volatile uint64_t sink = 0;
        const uint8_t *px = t.data;
        for (size_t i = 0; i < want; ++i) sink ^= px[i];
        (void)sink;
        tiff_free(&t);
    }
    unlink(path);
}

#ifdef TIFF_FUZZ_STANDALONE
static int replay_file(const char *path) {
    FILE *f = path ? fopen(path, "rb") : stdin;
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { if (path) fclose(f); return 0; }
    uint8_t *buf = malloc((size_t)n);
    size_t got = fread(buf, 1, (size_t)n, f);
    if (path) fclose(f);
    exercise(buf, got);
    free(buf);
    return 0;
}
int main(int argc, char **argv) {
    if (argc < 2) return replay_file(NULL);
    for (int i = 1; i < argc; ++i) replay_file(argv[i]);
    printf("fuzz_tiff: replayed %d input(s), no crash\n", argc - 1);
    return 0;
}
#else
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    exercise(data, size);
    return 0;
}
#endif
