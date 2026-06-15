/*
 * reader.c - Manga/comic CBZ reader
 *
 * Downloads the full CBZ archive from Jellyfin via /Items/{id}/Download
 * (matching the web client which uses JSZip to extract pages client-side).
 * Pages are cached on SD card by item ID so repeated opens are instant.
 *
 * Performance notes:
 *   - SD card write buffer is set to 512 KB via setvbuf() so libcurl's
 *     write callbacks return immediately without waiting for disk I/O.
 *     This prevents TCP receive-buffer back-pressure on the Jellyfin side.
 *   - CURLOPT_BUFFERSIZE = 64 KB reduces callback frequency.
 *   - Page turns: fseek + zlib inflate + nearest-neighbor scale + Morton
 *     tile ~5–20 ms.  No network I/O after the first open.
 */

#include <3ds.h>
#include <citro2d.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <math.h>
#include <curl/curl.h>

#include "ui/reader.h"
#include "util/cbz.h"
#include "util/stb_image.h"
#include "util/log.h"

#define PAGE_TEX_W   1024
#define PAGE_TEX_H   1024
#define CACHE_DIR    "sdmc:/3ds/jellyfin-3ds"
#define DL_STACK     (48 * 1024)
#define DL_PRIORITY  0x3C
#define DL_WBUF_SZ   (512 * 1024)  /* stdio write buffer — keeps fwrite fast */

/* ── State ──────────────────────────────────────────────────────────── */

static struct {
    C3D_Tex           tex;
    C2D_Image         img;
    Tex3DS_SubTexture subtex;
    bool              tex_init;
    bool              page_ready;
    int               pw, ph;       /* pixel dimensions of current page */

    cbz_t             cbz;
    bool              cbz_open_flag;

    Thread              dl_thread;
    volatile int        state;       /* reader_state_t */
    volatile size_t     dl_bytes;
    volatile size_t     dl_total;
    volatile bool       cancel;

    /* params set before thread launch */
    char server_url[JFIN_MAX_URL];
    char access_token[JFIN_MAX_TOKEN];
    char device_id[JFIN_MAX_ID];
    char cache_path[160];           /* sdmc:/.../cbz_ITEMID.cbz */
} s_rdr;

/* ── Nearest-neighbour scale ─────────────────────────────────────────── */

static u8 *scale_rgb(const u8 *src, int sw, int sh, int dw, int dh)
{
    u8 *dst = malloc((size_t)(dw * dh * 3));
    if (!dst) return NULL;
    for (int dy = 0; dy < dh; dy++) {
        int sy = dy * sh / dh;
        for (int dx = 0; dx < dw; dx++) {
            int sx = dx * sw / dw;
            const u8 *s = src + (sy * sw + sx) * 3;
            u8       *d = dst + (dy * dw + dx) * 3;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
        }
    }
    return dst;
}

/* ── Morton tiler (Z-order, 8×8 tiles) ─────────────────────────────── */

static void upload_page(const u16 *pixels, int sw, int sh)
{
    u16 *td = (u16 *)s_rdr.tex.data;
    const int bpr = PAGE_TEX_W / 8;

    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            int bx = x / 8, by = y / 8;
            int ix = x % 8, iy = y % 8;
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

/* ── Download callbacks ─────────────────────────────────────────────── */

static size_t dl_write_cb(void *ptr, size_t sz, size_t nmemb, void *ud)
{
    FILE *fp = (FILE *)ud;
    size_t n = sz * nmemb;
    /* fwrite returns immediately to stdio buffer; actual disk write is
     * deferred until the 512 KB buffer fills — prevents TCP back-pressure */
    size_t written = fwrite(ptr, 1, n, fp);
    s_rdr.dl_bytes += written;
    return written;
}

static int dl_progress_cb(void *ud, curl_off_t total, curl_off_t now,
                          curl_off_t u, curl_off_t v)
{
    (void)ud; (void)u; (void)v;
    if (total > 0) s_rdr.dl_total = (size_t)total;
    return s_rdr.cancel ? 1 : 0;
}

/* ── Download thread ────────────────────────────────────────────────── */

static void dl_thread_func(void *arg)
{
    (void)arg;

    /* Check cache: if file already exists and is a valid ZIP, skip download */
    {
        FILE *test = fopen(s_rdr.cache_path, "rb");
        if (test) {
            fclose(test);
            log_write("CBZ: found cached %s", s_rdr.cache_path);
            if (s_rdr.cbz_open_flag) { cbz_close(&s_rdr.cbz); s_rdr.cbz_open_flag = false; }
            if (cbz_open(&s_rdr.cbz, s_rdr.cache_path)) {
                s_rdr.cbz_open_flag = true;
                s_rdr.page_ready    = false;
                s_rdr.state         = READER_READY;
                log_write("CBZ: cache hit — %d pages", s_rdr.cbz.count);
                return;
            }
            /* Corrupt or incomplete — delete and re-download */
            log_write("CBZ: cache invalid, re-downloading");
            remove(s_rdr.cache_path);
        }
    }

    char url[JFIN_URL_BUF];
    snprintf(url, sizeof(url), "%s/Items/%.*s/Download",
             s_rdr.server_url,
             /* item id is embedded in cache_path after the prefix */
             0, "");
    /* Re-derive item_id from cache_path: "sdmc:/.../cbz_XXXX.cbz" */
    {
        const char *base = strrchr(s_rdr.cache_path, '/');
        base = base ? base + 1 : s_rdr.cache_path;
        /* base = "cbz_ITEMID.cbz"; skip "cbz_" prefix */
        char item_id[JFIN_MAX_ID] = {0};
        if (strncmp(base, "cbz_", 4) == 0) {
            const char *id_start = base + 4;
            const char *dot = strrchr(id_start, '.');
            int len = dot ? (int)(dot - id_start) : (int)strlen(id_start);
            if (len > 0 && len < JFIN_MAX_ID) {
                memcpy(item_id, id_start, (size_t)len);
            }
        }
        snprintf(url, sizeof(url), "%s/Items/%s/Download",
                 s_rdr.server_url, item_id);
    }

    char auth[640];
    snprintf(auth, sizeof(auth),
             "Authorization: MediaBrowser Client=\"Jellyfin 3DS\","
             " Device=\"Nintendo 3DS\","
             " DeviceId=\"%s\","
             " Version=\"" JFIN_VERSION "\","
             " Token=\"%s\"",
             s_rdr.device_id, s_rdr.access_token);

    log_write("CBZ: downloading %s", url);

    mkdir("sdmc:/3ds", 0777);
    mkdir(CACHE_DIR, 0777);

    FILE *fp = fopen(s_rdr.cache_path, "wb");
    if (!fp) {
        log_write("CBZ: cannot create %s", s_rdr.cache_path);
        s_rdr.state = READER_ERROR;
        return;
    }

    /* Large stdio write buffer — critical for download speed on 3DS.
     * Without this each 16 KB curl chunk triggers a direct SD card write,
     * causing TCP receive-buffer stalls and ~20 KB/s throughput instead of
     * the expected 500+ KB/s. */
    static char s_dl_wbuf[DL_WBUF_SZ];
    setvbuf(fp, s_dl_wbuf, _IOFBF, DL_WBUF_SZ);

    CURL *curl = curl_easy_init();
    if (!curl) { fclose(fp); remove(s_rdr.cache_path); s_rdr.state = READER_ERROR; return; }

    struct curl_slist *hdrs = curl_slist_append(NULL, auth);
    curl_easy_setopt(curl, CURLOPT_URL,              url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,        hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,     dl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,         fp);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,  dl_progress_cb);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,        0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,    1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,    0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,    30L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE,        65536L);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    fclose(fp);

    log_write("CBZ: download curl=%d http=%ld bytes=%zu",
              (int)rc, http_code, s_rdr.dl_bytes);

    if (s_rdr.cancel) {
        remove(s_rdr.cache_path);
        s_rdr.state = READER_IDLE;
        return;
    }
    if (rc != CURLE_OK) {
        log_write("CBZ: curl error: %s", curl_easy_strerror(rc));
        remove(s_rdr.cache_path);
        s_rdr.state = READER_ERROR;
        return;
    }
    if (http_code < 200 || http_code >= 300) {
        log_write("CBZ: server returned HTTP %ld", http_code);
        remove(s_rdr.cache_path);
        s_rdr.state = READER_ERROR;
        return;
    }
    if (s_rdr.dl_bytes < 22) {
        log_write("CBZ: file too small (%zu bytes)", s_rdr.dl_bytes);
        remove(s_rdr.cache_path);
        s_rdr.state = READER_ERROR;
        return;
    }

    if (s_rdr.cbz_open_flag) { cbz_close(&s_rdr.cbz); s_rdr.cbz_open_flag = false; }
    if (!cbz_open(&s_rdr.cbz, s_rdr.cache_path)) {
        log_write("CBZ: ZIP parse failed");
        s_rdr.state = READER_ERROR;
        return;
    }
    s_rdr.cbz_open_flag = true;
    s_rdr.page_ready    = false;
    s_rdr.state         = READER_READY;
    log_write("CBZ: ready — %d pages", s_rdr.cbz.count);
}

/* ── Thread helpers ─────────────────────────────────────────────────── */

static void join_dl_thread(void)
{
    if (s_rdr.dl_thread) {
        threadJoin(s_rdr.dl_thread, (u64)30e9);
        threadFree(s_rdr.dl_thread);
        s_rdr.dl_thread = NULL;
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

bool reader_init(void)
{
    if (s_rdr.tex_init) return true;
    if (!C3D_TexInit(&s_rdr.tex, PAGE_TEX_W, PAGE_TEX_H, GPU_RGB565))
        return false;
    C3D_TexSetFilter(&s_rdr.tex, GPU_LINEAR, GPU_LINEAR);
    memset(s_rdr.tex.data, 0, PAGE_TEX_W * PAGE_TEX_H * 2);
    s_rdr.img.tex    = &s_rdr.tex;
    s_rdr.img.subtex = &s_rdr.subtex;
    s_rdr.tex_init   = true;
    s_rdr.state      = READER_IDLE;
    return true;
}

void reader_cleanup(void)
{
    reader_cancel();
    join_dl_thread();
    if (s_rdr.cbz_open_flag) { cbz_close(&s_rdr.cbz); s_rdr.cbz_open_flag = false; }
    if (s_rdr.tex_init) { C3D_TexDelete(&s_rdr.tex); s_rdr.tex_init = false; }
    s_rdr.page_ready = false;
    s_rdr.state = READER_IDLE;
}

void reader_open_book(const jfin_session_t *session, const char *item_id)
{
    if (s_rdr.state == READER_DOWNLOADING) {
        s_rdr.cancel = true;
        join_dl_thread();
        s_rdr.cancel = false;
    } else {
        join_dl_thread();
    }

    s_rdr.dl_bytes   = 0;
    s_rdr.dl_total   = 0;
    s_rdr.page_ready = false;

    strncpy(s_rdr.server_url,   session->server_url,   JFIN_MAX_URL   - 1);
    strncpy(s_rdr.access_token, session->access_token, JFIN_MAX_TOKEN - 1);
    strncpy(s_rdr.device_id,    session->device_id,    JFIN_MAX_ID    - 1);
    s_rdr.server_url[JFIN_MAX_URL - 1]     = '\0';
    s_rdr.access_token[JFIN_MAX_TOKEN - 1] = '\0';
    s_rdr.device_id[JFIN_MAX_ID - 1]       = '\0';

    /* Cache file named by item ID so different books don't overwrite each other */
    snprintf(s_rdr.cache_path, sizeof(s_rdr.cache_path),
             CACHE_DIR "/cbz_%s.cbz", item_id);

    s_rdr.state = READER_DOWNLOADING;
    s_rdr.dl_thread = threadCreate(dl_thread_func, NULL, DL_STACK,
                                   DL_PRIORITY, -2, false);
    if (!s_rdr.dl_thread) {
        log_write("CBZ: threadCreate failed");
        s_rdr.state = READER_ERROR;
    }
}

void reader_open_local(const char *path)
{
    if (s_rdr.state == READER_DOWNLOADING) {
        s_rdr.cancel = true;
        join_dl_thread();
        s_rdr.cancel = false;
    } else {
        join_dl_thread();
    }

    s_rdr.dl_bytes   = 0;
    s_rdr.dl_total   = 0;
    s_rdr.page_ready = false;

    if (s_rdr.cbz_open_flag) { cbz_close(&s_rdr.cbz); s_rdr.cbz_open_flag = false; }

    if (cbz_open(&s_rdr.cbz, path)) {
        s_rdr.cbz_open_flag = true;
        s_rdr.state         = READER_READY;
        log_write("CBZ: opened local %s — %d pages", path, s_rdr.cbz.count);
    } else {
        log_write("CBZ: failed to open local %s", path);
        s_rdr.state = READER_ERROR;
    }
}

void reader_cancel(void)
{
    if (s_rdr.state == READER_DOWNLOADING)
        s_rdr.cancel = true;
}

bool reader_load_page(int page_index, bool rotated)
{
    if (!s_rdr.tex_init || s_rdr.state != READER_READY || !s_rdr.cbz_open_flag)
        return false;
    s_rdr.page_ready = false;

    size_t data_sz = 0;
    u8 *data = cbz_page_data(&s_rdr.cbz, page_index, &data_sz);
    if (!data) {
        log_write("CBZ: cbz_page_data failed page %d", page_index);
        return false;
    }

    int img_w, img_h, ch;
    u8 *rgb = stbi_load_from_memory(data, (int)data_sz, &img_w, &img_h, &ch, 3);
    free(data);
    if (!rgb) {
        log_write("CBZ: stbi decode failed page %d (%zu bytes)", page_index, data_sz);
        return false;
    }

    /* Scale to fit within PAGE_TEX_W × PAGE_TEX_H maintaining aspect ratio.
     * Without this we were passing the wrong stride to the RGB565 loop:
     * a 1800-wide image's first 512×512 pixels span ~145 rows of the
     * original, producing a garbled/static-noise result. */
    int tex_w = img_w, tex_h = img_h;
    bool was_scaled = false;

    if (img_w > PAGE_TEX_W || img_h > PAGE_TEX_H) {
        float scale = fminf((float)PAGE_TEX_W / img_w, (float)PAGE_TEX_H / img_h);
        tex_w = (int)(img_w * scale);
        tex_h = (int)(img_h * scale);
        if (tex_w < 1) tex_w = 1;
        if (tex_h < 1) tex_h = 1;
        if (tex_w > PAGE_TEX_W) tex_w = PAGE_TEX_W;
        if (tex_h > PAGE_TEX_H) tex_h = PAGE_TEX_H;

        u8 *scaled = scale_rgb(rgb, img_w, img_h, tex_w, tex_h);
        stbi_image_free(rgb);
        rgb = scaled;
        was_scaled = true;
        if (!rgb) return false;
    }

    /* RGB888 → RGB565 */
    u16 *rgb565 = malloc((size_t)(tex_w * tex_h * 2));
    if (!rgb565) {
        if (was_scaled) free(rgb); else stbi_image_free(rgb);
        return false;
    }
    for (int i = 0; i < tex_w * tex_h; i++) {
        u8 r = rgb[i * 3], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
        rgb565[i] = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    if (was_scaled) free(rgb); else stbi_image_free(rgb);

    /* Bake 90° CCW rotation into pixel data so reader_draw is a simple
     * aspect-fit regardless of orientation.  C2D_DrawImage rotation is
     * broken on 3DS (always produces a black screen). */
    if (rotated) {
        int rw = tex_h, rh = tex_w;
        u16 *rd = malloc((size_t)(rw * rh * 2));
        if (rd) {
            for (int dy = 0; dy < rh; dy++)
                for (int dx = 0; dx < rw; dx++)
                    rd[dy * rw + dx] = rgb565[dx * tex_w + (tex_w - 1 - dy)];
            free(rgb565);
            rgb565 = rd;
            int tmp = tex_w; tex_w = tex_h; tex_h = tmp;
        }
    }

    /* Upload to GPU texture */
    memset(s_rdr.tex.data, 0, PAGE_TEX_W * PAGE_TEX_H * 2);
    upload_page(rgb565, tex_w, tex_h);
    free(rgb565);

    s_rdr.subtex.width  = (u16)tex_w;
    s_rdr.subtex.height = (u16)tex_h;
    s_rdr.subtex.left   = 0.0f;
    s_rdr.subtex.top    = 1.0f;
    s_rdr.subtex.right  = (float)tex_w / PAGE_TEX_W;
    s_rdr.subtex.bottom = 1.0f - (float)tex_h / PAGE_TEX_H;

    s_rdr.pw         = tex_w;
    s_rdr.ph         = tex_h;
    s_rdr.page_ready = true;
    log_write("CBZ: page %d loaded (orig %dx%d → tex %dx%d)",
              page_index, img_w, img_h, tex_w, tex_h);
    return true;
}

reader_state_t reader_get_state(void)  { return (reader_state_t)s_rdr.state; }
size_t         reader_dl_bytes(void)   { return s_rdr.dl_bytes; }
size_t         reader_dl_total(void)   { return s_rdr.dl_total; }
int            reader_page_count(void) { return s_rdr.cbz_open_flag ? s_rdr.cbz.count : 0; }
bool           reader_page_ready(void) { return s_rdr.page_ready; }
int            reader_page_width(void) { return s_rdr.pw; }
int            reader_page_height(void){ return s_rdr.ph; }

void reader_draw(float x, float y, float w, float h,
                 float zoom, float pan_x, float pan_y)
{
    if (!s_rdr.page_ready) return;
    float sx = w / (float)s_rdr.pw;
    float sy = h / (float)s_rdr.ph;
    float sc = (sx < sy ? sx : sy) * zoom;
    float dw = s_rdr.pw * sc;
    float dh = s_rdr.ph * sc;
    C2D_DrawImageAt(s_rdr.img,
                    x + (w - dw) * 0.5f + pan_x,
                    y + (h - dh) * 0.5f + pan_y,
                    0.5f, NULL, sc, sc);
}

void reader_draw_split_top(float zoom, float pan_x, float pan_y)
{
    if (!s_rdr.page_ready) return;
    float sc = (400.0f / (float)s_rdr.pw) * zoom;
    float dw = s_rdr.pw * sc;
    float ix  = (400.0f - dw) * 0.5f + pan_x;
    C2D_DrawImageAt(s_rdr.img, ix, pan_y, 0.5f, NULL, sc, sc);
}

void reader_draw_split_bottom(float zoom, float pan_x, float pan_y)
{
    if (!s_rdr.page_ready) return;
    float sc  = (400.0f / (float)s_rdr.pw) * zoom;
    float dw  = s_rdr.pw * sc;
    /* Centre horizontally within the 320px bottom screen so the image
     * doesn't appear shifted right relative to the narrower screen. */
    float ix  = (320.0f - dw) * 0.5f + pan_x;
    /* Vertical: continue 240px below where the top screen started */
    float iy  = pan_y - 240.0f;
    C2D_DrawImageAt(s_rdr.img, ix, iy, 0.5f, NULL, sc, sc);
}
