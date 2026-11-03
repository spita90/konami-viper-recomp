/* chdextract: the two chdman commands the build uses, on libchdr (third_party/libchdr).
 *
 *   chdextract info FILE          prints "SHA1: <hex>" (the CHD's SHA1, as `chdman info`)
 *   chdextract extracthd FILE OUT writes the raw hard disk image (as `chdman extracthd`)
 *
 * Used by the release launcher (dist/launcher.py), which cannot count on chdman being
 * installed; tools/game.py and tools/extract.py use it when VIPER_CHDEXTRACT names it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libchdr/chd.h"

static int fail(const char *what, const char *file, chd_error err)
{
    fprintf(stderr, "chdextract: %s %s: %s\n", what, file, chd_error_string(err));
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 3 || (strcmp(argv[1], "info") && strcmp(argv[1], "extracthd")) ||
        (!strcmp(argv[1], "extracthd") && argc != 4)) {
        fprintf(stderr, "usage: chdextract info FILE\n       chdextract extracthd FILE OUT\n");
        return 2;
    }
    chd_file *chd;
    chd_error err = chd_open(argv[2], CHD_OPEN_READ, NULL, &chd);
    if (err != CHDERR_NONE) return fail("cannot open", argv[2], err);
    const chd_header *h = chd_get_header(chd);

    if (!strcmp(argv[1], "info")) {
        printf("SHA1: ");
        for (int i = 0; i < 20; i++) printf("%02x", h->sha1[i]);
        printf("\nLogical size: %llu\nHunk size: %u\n", (unsigned long long)h->logicalbytes, h->hunkbytes);
        chd_close(chd);
        return 0;
    }

    FILE *out = fopen(argv[3], "wb");
    if (!out) {
        perror(argv[3]);
        chd_close(chd);
        return 1;
    }
    unsigned char *buf = malloc(h->hunkbytes);
    unsigned long long left = h->logicalbytes;
    int rc = 0;
    for (uint32_t hunk = 0; left && buf; hunk++) {
        if ((err = chd_read(chd, hunk, buf)) != CHDERR_NONE) {
            rc = fail("cannot read", argv[2], err);
            break;
        }
        size_t n = left < h->hunkbytes ? (size_t)left : h->hunkbytes;
        if (fwrite(buf, 1, n, out) != n) {
            perror(argv[3]);
            rc = 1;
            break;
        }
        left -= n;
    }
    if (!buf) rc = 1;
    free(buf);
    if (fclose(out)) rc = 1;
    chd_close(chd);
    if (rc) remove(argv[3]);
    return rc;
}
