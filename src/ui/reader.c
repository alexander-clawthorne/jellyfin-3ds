/**
 * reader.c - Manga/comic page renderer for CBZ items
 *
 * Downloads individual pages from Jellyfin via:
 *   GET /Items/{id}/Images/Primary?imageIndex=N&maxWidth=512&maxHeight=512
 * Decodes with stb_image and uploads to a 512x512 RGB565 GPU texture.
 * The texture is kept alive between pages; only the pixel data is replaced.
 *
 * Morton tiling: the 3DS GPU stores textures in 8x8 Z-order tiles.
 * The upload_page() function converts from linear scanline order using the
 * standard Z-order (Morton code) formula — identical logic to album_art.c
 * but written for the larger 512x512 texture size.
 */

#include <3ds.h>
#include <citro2d.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <curl/curl.h>

#include "ui/reader.h"
#include "api/jellyfin.h"
#include "util/stb_image.h"
#include "util/log.h"

#define PAGE_TEX_W    512
#define PAGE_TEX_H    512
#define PAGE_DL_MAX   (1024 * 1024)  /* 1 MB per page download */

/* ── Download buffer ───────────────────────────────────────────────── */

typedef struct { u8 *data; size_t size, cap; } dl_buf_t;

static size_t page_dl_cb(void *ptr, size_t sz, size_t nmemb, void *ud)
{
    dl_buf_t *b = (dl_buf_t *)ud;
    size_t n = sz * nmemb;
    if (b->size + n >= b->cap) {
        size_t nc = (b->cap + n) * 2;
        if (nc > PAGE_DL_MAX) return 0;
        u8 *nd = realloc(b->data, nc);
        if (!nd) return 0;
        b->data = nd;
        b->cap  = nc;
    }
    memcpy(b->data + b->size, ptr, n);
    b->size += n;
    return n;
}

/* ── State ─────────────────────────────────────────────────────────── */

static struct {
    C3D_Tex           tex;
    C2D_Image         img;
    Tex3DS_SubTexture subtex;
    bool              init;
    bool              ready;
    int               pw, ph;  /* actual page dimensions within texture */
} s_rdr;

/* ── Morton-tiled upload ────────────────────────────────────────────── */

static void upload_page(const u16 *pixels, int sw, int sh)
{
    u16 *td = (u16 *)s_rdr.tex.data;
    const int bpr = PAGE_TEX_W / 8;  /* blocks per row */

    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            int bx = x / 8, by = y / 8;
            int ix = x % 8, iy = y % 8;
            /* Z-order code within 8x8 block */
            int z = 0;
            for (int b = 0; b < 3; b++) {
                z |= ((ix >> b) & 1) << (2 * b);
                z |= ((iy >> b) & 1) << (2 * b + 1);
            }
            td[(by * bpr + bx) * 64 + z] = pixels[y * sw + x];
        }
    }
    C3D_TexFlush(&s_rdr.tex);
}

/* ── Public API ────────────────────────────────────────────────────── */

bool reader_init(void)
{
    if (s_rdr.init) return true;
    if (!C3D_TexInit(&s_rdr.tex, PAGE_TEX_W, PAGE_TEX_H, GPU_RGB565))
        return false;
    C3D_TexSetFilter(&s_rdr.tex, GPU_LINEAR, GPU_LINEAR);
    memset(s_rdr.tex.data, 0, PAGE_TEX_W * PAGE_TEX_H * 2);
    s_rdr.img.tex    = &s_rdr.tex;
    s_rdr.img.subtex = &s_rdr.subtex;
    s_rdr.init  = true;
    s_rdr.ready = false;
    return true;
}

void reader_cleanup(void)
{
    if (!s_rdr.init) return;
    C3D_TexDelete(&s_rdr.tex);
    s_rdr.init = s_rdr.ready = false;
}

bool reader_load_page(const jfin_session_t *session,
                      const char *item_id, int page_index)
{
    if (!s_rdr.init) return false;
    s_rdr.ready = false;

    /* Build image URL — request up to 512x512 so the server picks the
     * best scale for the native page aspect ratio */
    char url[JFIN_URL_BUF];
    snprintf(url, sizeof(url),
             "%s/Items/%s/Images/Primary"
             "?imageIndex=%d&maxWidth=%d&maxHeight=%d"
             "&format=Jpg&quality=85",
             session->server_url, item_id, page_index,
             PAGE_TEX_W, PAGE_TEX_H);

    /* Auth header */
    char auth[600];
    snprintf(auth, sizeof(auth),
             "Authorization: MediaBrowser Client=\"Jellyfin 3DS\","
             " Device=\"Nintendo 3DS\","
             " DeviceId=\"%s\","
             " Version=\"" JFIN_VERSION "\","
             " Token=\"%s\"",
             session->device_id, session->access_token);

    /* Download */
    dl_buf_t dl = { malloc(64 * 1024), 0, 64 * 1024 };
    if (!dl.data) return false;

    CURL *curl = curl_easy_init();
    if (!curl) { free(dl.data); return false; }

    struct curl_slist *hdrs = curl_slist_append(NULL, auth);
    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  page_dl_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &dl);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK || dl.size == 0) {
        log_write("READER: download failed (page %d): %s",
                  page_index, curl_easy_strerror(rc));
        free(dl.data);
        return false;
    }

    /* Decode JPEG/PNG */
    int w, h, ch;
    u8 *rgb = stbi_load_from_memory(dl.data, (int)dl.size, &w, &h, &ch, 3);
    free(dl.data);
    if (!rgb) {
        log_write("READER: decode failed (page %d)", page_index);
        return false;
    }

    /* Clamp to texture bounds */
    if (w > PAGE_TEX_W) w = PAGE_TEX_W;
    if (h > PAGE_TEX_H) h = PAGE_TEX_H;

    /* RGB888 → RGB565 */
    u16 *rgb565 = malloc(w * h * 2);
    if (!rgb565) { stbi_image_free(rgb); return false; }
    for (int i = 0; i < w * h; i++) {
        u8 r = rgb[i * 3], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
        rgb565[i] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }
    stbi_image_free(rgb);

    /* Upload to GPU */
    memset(s_rdr.tex.data, 0, PAGE_TEX_W * PAGE_TEX_H * 2);
    upload_page(rgb565, w, h);
    free(rgb565);

    /* Subtexture UV — same convention as album_art.c */
    s_rdr.subtex.width  = (u16)w;
    s_rdr.subtex.height = (u16)h;
    s_rdr.subtex.left   = 0.0f;
    s_rdr.subtex.top    = 1.0f;
    s_rdr.subtex.right  = (float)w / PAGE_TEX_W;
    s_rdr.subtex.bottom = 1.0f - (float)h / PAGE_TEX_H;

    s_rdr.pw    = w;
    s_rdr.ph    = h;
    s_rdr.ready = true;
    log_write("READER: page %d loaded (%dx%d)", page_index, w, h);
    return true;
}

void reader_draw(float x, float y, float w, float h)
{
    if (!s_rdr.ready) return;
    float sx = w / (float)s_rdr.pw;
    float sy = h / (float)s_rdr.ph;
    float sc = sx < sy ? sx : sy;  /* fit within bounds */
    float dw = s_rdr.pw * sc;
    float dh = s_rdr.ph * sc;
    C2D_DrawImageAt(s_rdr.img,
                    x + (w - dw) * 0.5f,
                    y + (h - dh) * 0.5f,
                    0.5f, NULL, sc, sc);
}

bool reader_is_ready(void)
{
    return s_rdr.ready;
}
