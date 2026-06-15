/*
 * reader.c - Manga/comic CBZ reader
 *
 * Jellyfin never serves individual CBZ pages through its Images API.
 * The correct approach (matching the web client) is to download the full
 * CBZ archive via GET /Items/{id}/Download and parse it as a ZIP locally.
 *
 * Download happens in a background thread so the frame loop keeps running
 * and can show a progress indicator. Once the download finishes the thread
 * calls cbz_open() to index the pages and sets state to READER_READY.
 * From then on, reader_load_page() extracts pages synchronously (fast —
 * the CBZ is already on the SD card so it's just fseek + zlib inflate).
 *
 * Cache: the last downloaded CBZ is kept at:
 *   sdmc:/3ds/jellyfin-3ds/book.cbz
 * Re-opening the same book re-downloads if the file is missing; if the
 * file exists from a previous session it is re-parsed immediately without
 * downloading again (fast path).
 */

#include <3ds.h>
#include <citro2d.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <curl/curl.h>

#include "ui/reader.h"
#include "util/cbz.h"
#include "util/stb_image.h"
#include "util/log.h"

/* ── Texture dimensions ─────────────────────────────────────────────── */

#define PAGE_TEX_W   512
#define PAGE_TEX_H   512
#define CACHE_PATH   "sdmc:/3ds/jellyfin-3ds/book.cbz"
#define DL_STACK     (48 * 1024)   /* download thread stack */
#define DL_PRIORITY  0x3C          /* slightly below main thread */

/* ── State ──────────────────────────────────────────────────────────── */

static struct {
    /* GPU texture */
    C3D_Tex           tex;
    C2D_Image         img;
    Tex3DS_SubTexture subtex;
    bool              tex_init;
    bool              page_ready;
    int               pw, ph;

    /* CBZ index */
    cbz_t             cbz;
    bool              cbz_open_flag;

    /* Download thread */
    Thread              dl_thread;
    volatile int        state;       /* reader_state_t */
    volatile size_t     dl_bytes;
    volatile size_t     dl_total;
    volatile bool       cancel;

    /* Params set by reader_open_book() before thread launch */
    char server_url[JFIN_MAX_URL];
    char access_token[JFIN_MAX_TOKEN];
    char device_id[JFIN_MAX_ID];
    char item_id[JFIN_MAX_ID];
} s_rdr;

/* ── Morton tiler (same formula as album_art.c) ─────────────────────── */

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
    size_t written = fwrite(ptr, 1, n, fp);
    s_rdr.dl_bytes += written;
    return written;
}

static int dl_progress_cb(void *ud, curl_off_t total, curl_off_t now,
                          curl_off_t u, curl_off_t v)
{
    (void)ud; (void)u; (void)v;
    if (total > 0) s_rdr.dl_total = (size_t)total;
    return s_rdr.cancel ? 1 : 0;   /* non-zero aborts transfer */
}

/* ── Download thread ────────────────────────────────────────────────── */

static void dl_thread_func(void *arg)
{
    (void)arg;

    /* Compose download URL */
    char url[JFIN_URL_BUF];
    snprintf(url, sizeof(url), "%s/Items/%s/Download",
             s_rdr.server_url, s_rdr.item_id);

    char auth[640];
    snprintf(auth, sizeof(auth),
             "Authorization: MediaBrowser Client=\"Jellyfin 3DS\","
             " Device=\"Nintendo 3DS\","
             " DeviceId=\"%s\","
             " Version=\"" JFIN_VERSION "\","
             " Token=\"%s\"",
             s_rdr.device_id, s_rdr.access_token);

    log_write("CBZ: downloading %s", url);

    /* Ensure cache directory exists */
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/jellyfin-3ds", 0777);

    FILE *fp = fopen(CACHE_PATH, "wb");
    if (!fp) {
        log_write("CBZ: cannot create " CACHE_PATH);
        s_rdr.state = READER_ERROR;
        return;
    }

    CURL *curl = curl_easy_init();
    if (!curl) { fclose(fp); remove(CACHE_PATH); s_rdr.state = READER_ERROR; return; }

    struct curl_slist *hdrs = curl_slist_append(NULL, auth);
    curl_easy_setopt(curl, CURLOPT_URL,               url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,         hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,      dl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,          fp);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,   dl_progress_cb);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,         0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,     1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,     0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,     30L);
    /* No overall timeout — CBZ files can be large */

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
        remove(CACHE_PATH);
        s_rdr.state = READER_IDLE;
        return;
    }
    if (rc != CURLE_OK) {
        log_write("CBZ: curl error: %s", curl_easy_strerror(rc));
        remove(CACHE_PATH);
        s_rdr.state = READER_ERROR;
        return;
    }
    if (http_code < 200 || http_code >= 300) {
        log_write("CBZ: HTTP %ld — check auth / item ID", http_code);
        remove(CACHE_PATH);
        s_rdr.state = READER_ERROR;
        return;
    }
    if (s_rdr.dl_bytes < 22) {
        log_write("CBZ: downloaded file too small (%zu bytes)", s_rdr.dl_bytes);
        remove(CACHE_PATH);
        s_rdr.state = READER_ERROR;
        return;
    }

    /* Parse ZIP central directory */
    if (s_rdr.cbz_open_flag) {
        cbz_close(&s_rdr.cbz);
        s_rdr.cbz_open_flag = false;
    }
    if (!cbz_open(&s_rdr.cbz, CACHE_PATH)) {
        log_write("CBZ: failed to parse ZIP structure");
        s_rdr.state = READER_ERROR;
        return;
    }
    s_rdr.cbz_open_flag = true;
    s_rdr.page_ready    = false;
    s_rdr.state = READER_READY;
    log_write("CBZ: ready — %d pages", s_rdr.cbz.count);
}

/* ── Thread lifecycle helpers ───────────────────────────────────────── */

static void join_dl_thread(void)
{
    if (s_rdr.dl_thread) {
        threadJoin(s_rdr.dl_thread, (u64)30e9);  /* 30s timeout */
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
    /* Cancel and join any existing download */
    if (s_rdr.state == READER_DOWNLOADING) {
        s_rdr.cancel = true;
        join_dl_thread();
        s_rdr.cancel = false;
    } else {
        join_dl_thread();
    }

    /* Reset state */
    s_rdr.dl_bytes   = 0;
    s_rdr.dl_total   = 0;
    s_rdr.page_ready = false;

    /* Copy params for the thread */
    strncpy(s_rdr.server_url,   session->server_url,   JFIN_MAX_URL   - 1);
    strncpy(s_rdr.access_token, session->access_token, JFIN_MAX_TOKEN - 1);
    strncpy(s_rdr.device_id,    session->device_id,    JFIN_MAX_ID    - 1);
    strncpy(s_rdr.item_id,      item_id,               JFIN_MAX_ID    - 1);
    s_rdr.server_url[JFIN_MAX_URL - 1]   = '\0';
    s_rdr.access_token[JFIN_MAX_TOKEN - 1] = '\0';
    s_rdr.device_id[JFIN_MAX_ID - 1]     = '\0';
    s_rdr.item_id[JFIN_MAX_ID - 1]       = '\0';

    s_rdr.state = READER_DOWNLOADING;
    s_rdr.dl_thread = threadCreate(dl_thread_func, NULL, DL_STACK,
                                   DL_PRIORITY, -2, false);
    if (!s_rdr.dl_thread) {
        log_write("CBZ: threadCreate failed");
        s_rdr.state = READER_ERROR;
    }
}

void reader_cancel(void)
{
    if (s_rdr.state == READER_DOWNLOADING)
        s_rdr.cancel = true;
}

bool reader_load_page(int page_index)
{
    if (!s_rdr.tex_init || s_rdr.state != READER_READY || !s_rdr.cbz_open_flag)
        return false;
    s_rdr.page_ready = false;

    size_t data_sz = 0;
    u8 *data = cbz_page_data(&s_rdr.cbz, page_index, &data_sz);
    if (!data) {
        log_write("CBZ: cbz_page_data failed for page %d", page_index);
        return false;
    }

    int w, h, ch;
    u8 *rgb = stbi_load_from_memory(data, (int)data_sz, &w, &h, &ch, 3);
    free(data);
    if (!rgb) {
        log_write("CBZ: stbi decode failed page %d", page_index);
        return false;
    }

    if (w > PAGE_TEX_W) w = PAGE_TEX_W;
    if (h > PAGE_TEX_H) h = PAGE_TEX_H;

    /* RGB888 → RGB565 */
    u16 *rgb565 = malloc((size_t)(w * h * 2));
    if (!rgb565) { stbi_image_free(rgb); return false; }
    for (int i = 0; i < w * h; i++) {
        u8 r = rgb[i * 3], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
        rgb565[i] = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    stbi_image_free(rgb);

    memset(s_rdr.tex.data, 0, PAGE_TEX_W * PAGE_TEX_H * 2);
    upload_page(rgb565, w, h);
    free(rgb565);

    s_rdr.subtex.width  = (u16)w;
    s_rdr.subtex.height = (u16)h;
    s_rdr.subtex.left   = 0.0f;
    s_rdr.subtex.top    = 1.0f;
    s_rdr.subtex.right  = (float)w / PAGE_TEX_W;
    s_rdr.subtex.bottom = 1.0f - (float)h / PAGE_TEX_H;

    s_rdr.pw         = w;
    s_rdr.ph         = h;
    s_rdr.page_ready = true;
    log_write("CBZ: page %d loaded (%dx%d)", page_index, w, h);
    return true;
}

reader_state_t reader_get_state(void)  { return (reader_state_t)s_rdr.state; }
size_t         reader_dl_bytes(void)   { return s_rdr.dl_bytes; }
size_t         reader_dl_total(void)   { return s_rdr.dl_total; }
int            reader_page_count(void) { return s_rdr.cbz_open_flag ? s_rdr.cbz.count : 0; }
bool           reader_page_ready(void) { return s_rdr.page_ready; }

void reader_draw(float x, float y, float w, float h)
{
    if (!s_rdr.page_ready) return;
    float sx = w / (float)s_rdr.pw;
    float sy = h / (float)s_rdr.ph;
    float sc = sx < sy ? sx : sy;
    float dw = s_rdr.pw * sc;
    float dh = s_rdr.ph * sc;
    C2D_DrawImageAt(s_rdr.img,
                    x + (w - dw) * 0.5f,
                    y + (h - dh) * 0.5f,
                    0.5f, NULL, sc, sc);
}
