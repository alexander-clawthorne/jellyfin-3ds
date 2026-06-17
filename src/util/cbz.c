/*
 * cbz.c - CBZ (ZIP) parser for manga/comic pages
 *
 * CBZ files are standard ZIP archives containing JPEG/PNG images.
 * We walk the ZIP central directory to build a sorted page index, then
 * decompress individual entries on demand using zlib raw-deflate mode.
 *
 * ZIP layout (little-endian):
 *   [Local File Headers + data]
 *   Central Directory (cd_count * 46+n bytes at cd_offset)
 *   End of Central Directory (22 bytes at/near end of file)
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strcasecmp */
#include <zlib.h>
#include "util/cbz.h"
#include "util/log.h"

/* ── ZIP field helpers (little-endian) ─────────────────────────────── */

static inline uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t le32(const uint8_t *p)
{
    return (uint32_t)(p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

/* ZIP signatures */
#define SIG_LFH   0x04034b50u   /* local file header */
#define SIG_CDFH  0x02014b50u   /* central directory file header */
#define SIG_EOCD  0x06054b50u   /* end of central directory */

/* ── Image file detection ───────────────────────────────────────────── */

static bool has_image_ext(const char *name)
{
    int n = (int)strlen(name);
    if (n < 4) return false;
    /* check last 5 chars, case-insensitive */
    char lo[6];
    int s = (n >= 5) ? n - 5 : 0;
    int l = n - s;
    for (int i = 0; i < l; i++) {
        char c = name[s + i];
        lo[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    lo[l] = '\0';
    return (strstr(lo, ".jpg")  != NULL ||
            strstr(lo, ".jpeg") != NULL ||
            strstr(lo, ".png")  != NULL);
}

/* ── Sort: compare page entries by filename ─────────────────────────── */

static int entry_cmp(const void *a, const void *b)
{
    return strcmp(((const cbz_entry_t *)a)->name,
                  ((const cbz_entry_t *)b)->name);
}

/* ── EOCD search ────────────────────────────────────────────────────── */

static bool find_eocd(FILE *fp, uint32_t *out_cd_off, uint32_t *out_count)
{
    if (fseek(fp, 0, SEEK_END) != 0) return false;
    long fsz = ftell(fp);
    if (fsz < 22) return false;

    /* EOCD fits in the last 65535+22 bytes; scan backward for its signature */
    long limit = (fsz > 65557) ? fsz - 65557 : 0;

    for (long pos = fsz - 22; pos >= limit; pos--) {
        if (fseek(fp, pos, SEEK_SET) != 0) return false;
        uint8_t buf[22];
        if (fread(buf, 1, 22, fp) < 22) return false;
        if (le32(buf) == SIG_EOCD) {
            *out_count  = le16(buf + 10);
            *out_cd_off = le32(buf + 16);
            return true;
        }
    }
    return false;
}

/* ── Public API ─────────────────────────────────────────────────────── */

bool cbz_open(cbz_t *c, const char *path)
{
    memset(c, 0, sizeof(*c));
    c->fp = fopen(path, "rb");
    if (!c->fp) {
        log_write("CBZ: cannot open %s", path);
        return false;
    }

    uint32_t cd_off = 0, cd_count = 0;
    if (!find_eocd(c->fp, &cd_off, &cd_count)) {
        log_write("CBZ: EOCD not found in %s", path);
        goto fail;
    }
    log_write("CBZ: cd_off=%u cd_count=%u", cd_off, cd_count);

    if (fseek(c->fp, (long)cd_off, SEEK_SET) != 0) goto fail;

    for (uint32_t i = 0; i < cd_count && c->count < CBZ_MAX_PAGES; i++) {
        uint8_t hdr[46];
        if (fread(hdr, 1, 46, c->fp) < 46) break;
        if (le32(hdr) != SIG_CDFH) break;

        uint16_t fname_len   = le16(hdr + 28);
        uint16_t extra_len   = le16(hdr + 30);
        uint16_t comment_len = le16(hdr + 32);
        uint16_t method      = le16(hdr + 10);
        uint32_t comp_sz     = le32(hdr + 20);
        uint32_t decomp_sz   = le32(hdr + 24);
        uint32_t lhdr_off    = le32(hdr + 42);

        /* Read filename */
        char name[128] = {0};
        int  nread = (fname_len < 127) ? fname_len : 127;
        if (fread(name, 1, (size_t)nread, c->fp) < (size_t)nread) break;
        /* Skip the rest of the variable fields */
        fseek(c->fp,
              (long)(extra_len + comment_len + (fname_len - nread)),
              SEEK_CUR);

        /* Skip directories and non-image files */
        if (name[nread - 1] == '/' || name[nread - 1] == '\\') continue;
        if (!has_image_ext(name)) continue;

        cbz_entry_t *e = &c->entries[c->count++];
        e->lhdr_off = lhdr_off;
        e->comp_sz  = comp_sz;
        e->decomp_sz = decomp_sz;
        e->method   = method;
        strncpy(e->name, name, 127);
        e->name[127] = '\0';
    }

    if (c->count == 0) {
        log_write("CBZ: no image entries in %s", path);
        goto fail;
    }

    /* Sort by filename for correct page order (assumes zero-padded names) */
    qsort(c->entries, (size_t)c->count, sizeof(cbz_entry_t), entry_cmp);
    log_write("CBZ: opened with %d pages", c->count);
    return true;

fail:
    fclose(c->fp);
    c->fp = NULL;
    return false;
}

void cbz_close(cbz_t *c)
{
    if (c->fp) { fclose(c->fp); c->fp = NULL; }
    c->count = 0;
}

u8 *cbz_page_data(cbz_t *c, int idx, size_t *out_sz)
{
    if (!c->fp || idx < 0 || idx >= c->count) return NULL;
    cbz_entry_t *e = &c->entries[idx];

    /* Seek to local file header */
    if (fseek(c->fp, (long)e->lhdr_off, SEEK_SET) != 0) return NULL;

    uint8_t lhdr[30];
    if (fread(lhdr, 1, 30, c->fp) < 30) return NULL;
    if (le32(lhdr) != SIG_LFH) {
        log_write("CBZ: bad LFH sig at offset %u", e->lhdr_off);
        return NULL;
    }

    uint16_t fname_len = le16(lhdr + 26);
    uint16_t extra_len = le16(lhdr + 28);
    /* Skip filename + extra in local header (may differ from central dir) */
    if (fseek(c->fp, (long)(fname_len + extra_len), SEEK_CUR) != 0) return NULL;

    if (e->method == 0) {
        /* Stored (no compression) — read directly */
        u8 *buf = malloc(e->comp_sz);
        if (!buf) return NULL;
        if (fread(buf, 1, e->comp_sz, c->fp) < e->comp_sz) { free(buf); return NULL; }
        *out_sz = e->comp_sz;
        return buf;
    }

    if (e->method == 8) {
        /* Deflate — decompress with zlib in raw mode (no zlib header) */
        u8 *comp = malloc(e->comp_sz);
        if (!comp) return NULL;
        if (fread(comp, 1, e->comp_sz, c->fp) < e->comp_sz) { free(comp); return NULL; }

        /* If decomp_sz is unknown (data descriptor), allocate a large buffer */
        uint32_t decomp_sz = e->decomp_sz > 0 ? e->decomp_sz : e->comp_sz * 6;
        u8 *out = malloc(decomp_sz);
        if (!out) { free(comp); return NULL; }

        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        zs.next_in   = comp;
        zs.avail_in  = e->comp_sz;
        zs.next_out  = out;
        zs.avail_out = decomp_sz;

        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) { free(comp); free(out); return NULL; }
        int ret = inflate(&zs, Z_FINISH);
        size_t actual = decomp_sz - zs.avail_out;
        inflateEnd(&zs);
        free(comp);

        if (ret != Z_STREAM_END && ret != Z_OK) {
            log_write("CBZ: inflate failed page %d (ret=%d)", idx, ret);
            free(out); return NULL;
        }
        *out_sz = actual;
        return out;
    }

    log_write("CBZ: unsupported compression method %u for page %d", e->method, idx);
    return NULL;
}
