/*
 * downloader.c - Background video file downloader
 *
 * Uses the same curl + 512 KB write-buffer technique as reader.c.
 * The stream URL already embeds the api_key, so no auth header is needed.
 */

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <curl/curl.h>

#include "util/downloader.h"
#include "util/log.h"

#define VDL_STACK    (48 * 1024)
#define VDL_PRIORITY 0x3C
#define VDL_WBUF_SZ  (512 * 1024)

static struct {
    Thread          thread;
    volatile int    state;      /* dl_state_t */
    volatile size_t bytes;
    volatile size_t total;
    volatile bool   cancel;

    char save_path[192];        /* sdmc:/.../video_ITEMID.ts  */
    char meta_path[192];        /* sdmc:/.../video_ITEMID.txt */
    char url[2048];
    char item_name[256];
} s_vdl;

/* ── Callbacks ──────────────────────────────────────────────────────── */

static size_t vdl_write_cb(void *ptr, size_t sz, size_t nmemb, void *ud)
{
    FILE *fp = (FILE *)ud;
    size_t n = fwrite(ptr, sz, nmemb, fp);
    s_vdl.bytes += n * sz;
    return n * sz;
}

static int vdl_progress_cb(void *ud, curl_off_t total, curl_off_t now,
                           curl_off_t u, curl_off_t v)
{
    (void)ud; (void)now; (void)u; (void)v;
    if (total > 0) s_vdl.total = (size_t)total;
    return s_vdl.cancel ? 1 : 0;
}

/* ── Thread ─────────────────────────────────────────────────────────── */

static void vdl_thread_func(void *arg)
{
    (void)arg;

    mkdir("sdmc:/3ds", 0777);
    mkdir(VDL_DIR, 0777);

    log_write("VDL: downloading %s", s_vdl.url);

    FILE *fp = fopen(s_vdl.save_path, "wb");
    if (!fp) {
        log_write("VDL: cannot create %s", s_vdl.save_path);
        s_vdl.state = DL_ERROR;
        return;
    }

    static char s_wbuf[VDL_WBUF_SZ];
    setvbuf(fp, s_wbuf, _IOFBF, VDL_WBUF_SZ);

    CURL *curl = curl_easy_init();
    if (!curl) { fclose(fp); remove(s_vdl.save_path); s_vdl.state = DL_ERROR; return; }

    curl_easy_setopt(curl, CURLOPT_URL,             s_vdl.url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,    vdl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,        fp);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, vdl_progress_cb);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,       0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,   1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,   0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,   30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,          0L);  /* transcode can take a while */
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE,       65536L);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    fclose(fp);

    log_write("VDL: curl=%d http=%ld bytes=%zu", (int)rc, http_code, s_vdl.bytes);

    if (s_vdl.cancel) {
        remove(s_vdl.save_path);
        s_vdl.state = DL_IDLE;
        return;
    }
    if (rc != CURLE_OK || http_code < 200 || http_code >= 300 || s_vdl.bytes < 512) {
        remove(s_vdl.save_path);
        s_vdl.state = DL_ERROR;
        return;
    }

    FILE *meta = fopen(s_vdl.meta_path, "w");
    if (meta) { fputs(s_vdl.item_name, meta); fclose(meta); }

    s_vdl.state = DL_DONE;
    log_write("VDL: saved %s (%zu bytes)", s_vdl.save_path, s_vdl.bytes);
}

/* ── Thread helpers ─────────────────────────────────────────────────── */

static void join_thread(void)
{
    if (s_vdl.thread) {
        threadJoin(s_vdl.thread, (u64)5e9);
        threadFree(s_vdl.thread);
        s_vdl.thread = NULL;
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

void dl_start_video(const char *item_id, const char *item_name, const char *url)
{
    if (s_vdl.state == DL_ACTIVE) {
        s_vdl.cancel = true;
        join_thread();
        s_vdl.cancel = false;
    } else {
        join_thread();
    }

    s_vdl.bytes  = 0;
    s_vdl.total  = 0;
    s_vdl.cancel = false;

    strncpy(s_vdl.item_name, item_name, sizeof(s_vdl.item_name) - 1);
    s_vdl.item_name[sizeof(s_vdl.item_name) - 1] = '\0';
    strncpy(s_vdl.url, url, sizeof(s_vdl.url) - 1);
    s_vdl.url[sizeof(s_vdl.url) - 1] = '\0';

    snprintf(s_vdl.save_path, sizeof(s_vdl.save_path),
             VDL_DIR "/video_%s.ts", item_id);
    snprintf(s_vdl.meta_path, sizeof(s_vdl.meta_path),
             VDL_DIR "/video_%s.txt", item_id);

    s_vdl.state  = DL_ACTIVE;
    s_vdl.thread = threadCreate(vdl_thread_func, NULL, VDL_STACK,
                                VDL_PRIORITY, -2, false);
    if (!s_vdl.thread) {
        log_write("VDL: threadCreate failed");
        s_vdl.state = DL_ERROR;
    }
}

void dl_cancel(void)
{
    if (s_vdl.state == DL_ACTIVE)
        s_vdl.cancel = true;
}

void dl_cleanup(void)
{
    dl_cancel();
    join_thread();
}

dl_state_t  dl_get_state(void)  { return (dl_state_t)s_vdl.state; }
size_t      dl_bytes(void)       { return s_vdl.bytes; }
size_t      dl_total(void)       { return s_vdl.total; }
const char *dl_item_name(void)   { return s_vdl.item_name; }
