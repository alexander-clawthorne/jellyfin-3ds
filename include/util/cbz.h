/**
 * cbz.h - Minimal CBZ (ZIP) parser for manga/comic pages
 *
 * CBZ files are ZIP archives containing JPEG/PNG images, one per page.
 * We parse the ZIP central directory to build a sorted page index and
 * decompress individual entries on demand using zlib (libz).
 */

#ifndef JFIN_CBZ_H
#define JFIN_CBZ_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <3ds.h>

#define CBZ_MAX_PAGES 1024

typedef struct {
    uint32_t lhdr_off;   /* byte offset of local file header in ZIP */
    uint32_t comp_sz;    /* compressed data size */
    uint32_t decomp_sz;  /* uncompressed data size */
    uint16_t method;     /* 0 = stored,  8 = deflate */
    char     name[128];  /* filename inside archive (used for sort order) */
} cbz_entry_t;

typedef struct {
    FILE       *fp;
    cbz_entry_t entries[CBZ_MAX_PAGES];
    int         count;   /* number of image entries */
} cbz_t;

/**
 * Open and index a CBZ file. Builds a sorted page list from the ZIP
 * central directory. Returns false if the file is not a valid ZIP or
 * contains no image files.
 */
bool cbz_open(cbz_t *c, const char *path);

/** Close and release file handle. */
void cbz_close(cbz_t *c);

/**
 * Decompress and return the raw image bytes for page[idx].
 * *out_sz receives the byte count. Caller must free() the result.
 * Returns NULL on error.
 */
u8 *cbz_page_data(cbz_t *c, int idx, size_t *out_sz);

#endif /* JFIN_CBZ_H */
