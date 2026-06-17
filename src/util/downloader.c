/*
 * downloader.c - Background media file downloader
 *
 * Downloads Jellyfin stream URLs to the SD card cache directory.
 * Uses .part → atomic rename discipline so cache_init() can safely
 * sweep interrupted downloads on next launch.
 */

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "util/downloader.h"
#include "util/cache.h"
#include "util/log.h"

#define VDL_STACK    (48 * 1024)
#define VDL_PRIORITY 0x3C
#define VDL_WBUF_SZ  (512 * 1024)

static struct {
    Thread          thread;
    volatile int    state;          /* dl_state_t */
    volatile size_t bytes;
    volatile size_t total;
    volatile size_t estimated_total; /* computed from bitrate×duration when Content-Length absent */
    volatile size_t resume_offset;   /* bytes already in .part at start of current attempt */
    volatile int    retry_count;     /* 0 = first attempt, 1-2 = retries */
    volatile bool   cancel;

    char item_id[64];           /* item id — used with cache API */
    char ext[8];                /* "ts", "cbz", "mp3" */
    char save_path[192];        /* .part path during download */
    char final_path[192];       /* final cache path after rename */
    char meta_path[192];        /* companion .txt path */
    char url[2048];
    char item_name[256];
    char sub_name[128];         /* subtitle track name being burned in */
    char subtitle_url[2048];    /* ASS subtitle URL to download, "" = none */
    bool is_video;
    int64_t runtime_ticks;      /* item duration for size estimation */
} s_vdl;

/* ── Download queue ─────────────────────────────────────────────────── */

#define DL_QUEUE_MAX 10

typedef struct {
    char    item_id[64];
    char    ext[8];             /* "ts", "cbz", "mp3" */
    char    item_name[256];
    char    url[2048];
    char    sub_name[128];
    char    subtitle_url[2048]; /* ASS download URL, "" = none */
    bool    is_video;
    int64_t runtime_ticks;
} dl_q_item_t;

static dl_q_item_t s_queue[DL_QUEUE_MAX];
static int s_q_count = 0;

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
    /* For resumed downloads (HTTP 206), curl reports remaining bytes only.
     * Add resume_offset so the UI shows progress relative to the full file. */
    if (total > 0) s_vdl.total = (size_t)total + s_vdl.resume_offset;
    return s_vdl.cancel ? 1 : 0;
}

/* ── Thread ─────────────────────────────────────────────────────────── */

static void vdl_thread_func(void *arg)
{
    (void)arg;

    /* Book/CBZ or audio: if already in cache, just write .txt and done */
    if (!s_vdl.is_video && cache_has(s_vdl.item_id, s_vdl.ext)) {
        log_write("VDL: %s.%s already cached", s_vdl.item_id, s_vdl.ext);
        FILE *meta = fopen(s_vdl.meta_path, "w");
        if (meta) { fputs(s_vdl.item_name, meta); fclose(meta); }
        s_vdl.state = DL_DONE;
        return;
    }

    log_write("VDL: downloading %s", s_vdl.url);

    CURL *curl = curl_easy_init();
    if (!curl) { s_vdl.state = DL_ERROR; return; }

    curl_easy_setopt(curl, CURLOPT_URL,             s_vdl.url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,    vdl_write_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, vdl_progress_cb);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,       0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,   1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,   0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,   30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,          0L);  /* transcode can take a while */
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE,       65536L);

    static char s_wbuf[VDL_WBUF_SZ];
    CURLcode rc       = CURLE_FAILED_INIT;
    long     http_code = 0;

    for (int attempt = 0; attempt < 3 && !s_vdl.cancel; attempt++) {
        s_vdl.retry_count = attempt;
        if (attempt > 0)
            svcSleepThread(3000000000LL); /* 3 s back-off between retries */

        /* Check for an existing .part file — resume from it if present */
        long offset = 0;
        {
            FILE *probe = fopen(s_vdl.save_path, "rb");
            if (probe) {
                fseek(probe, 0, SEEK_END);
                offset = ftell(probe);
                fclose(probe);
            }
        }
        s_vdl.resume_offset = (size_t)offset;

        FILE *fp = fopen(s_vdl.save_path, offset > 0 ? "ab" : "wb");
        if (!fp) {
            log_write("VDL: fopen failed (attempt %d)", attempt + 1);
            continue;
        }
        setvbuf(fp, s_wbuf, _IOFBF, VDL_WBUF_SZ);

        /* Progress counter starts at the already-downloaded offset */
        s_vdl.bytes = (size_t)offset;
        s_vdl.total = 0;

        curl_easy_setopt(curl, CURLOPT_WRITEDATA,         fp);
        /* NOTE: Jellyfin live-transcoding URLs (VideoBitRate=... style) do NOT
         * support HTTP Range requests — the server transcodes in real-time and
         * cannot seek mid-stream.  Those endpoints return HTTP 200 regardless
         * of the Range header, which we detect and handle below by deleting the
         * partial file and restarting from byte 0.  True byte-offset resume only
         * works for direct-file downloads (e.g. CBZ) from a server that honours
         * Range and responds with HTTP 206 Partial Content. */
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)offset);

        rc = curl_easy_perform(curl);
        http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        fclose(fp);

        log_write("VDL: attempt %d/3 curl=%d http=%ld bytes=%zu offset=%ld",
                  attempt + 1, (int)rc, http_code, s_vdl.bytes, offset);

        if (s_vdl.cancel) break;

        if (rc == CURLE_OK && http_code >= 200 && http_code < 300) {
            /* Server returned 200 on a Range request — it ignored the Range
             * header (typical for Jellyfin transcoding streams) and sent from
             * byte 0 into our append-mode file, corrupting it.  Delete the
             * partial file and restart clean on the next attempt. */
            if (offset > 0 && http_code == 200) {
                log_write("VDL: server ignored Range (200 on resume) — restarting fresh");
                remove(s_vdl.save_path);
                s_vdl.bytes = 0;
                rc = CURLE_HTTP_RETURNED_ERROR;
                continue;
            }
            break; /* success */
        }

        if (http_code == 416) {
            /* Range Not Satisfiable — .part may be corrupt; restart fresh */
            log_write("VDL: 416 RNS — restarting fresh");
            remove(s_vdl.save_path);
            s_vdl.bytes = 0;
            continue;
        }

        /* Keep .part for next retry — resume will pick it up */
        log_write("VDL: attempt %d failed, .part kept for retry resume", attempt + 1);
    }

    curl_easy_cleanup(curl);

    if (s_vdl.cancel) {
        remove(s_vdl.save_path);
        s_vdl.state = DL_IDLE;
        return;
    }

    bool ok = (rc == CURLE_OK &&
               http_code >= 200 && http_code < 300 &&
               s_vdl.bytes >= 512);
    if (!ok) {
        /* Leave .part on disk — user can re-queue the same item to resume */
        log_write("VDL: all 3 attempts failed; .part kept for re-queue resume");
        s_vdl.state = DL_ERROR;
        return;
    }

    /* Atomic rename: .part → final path, then register in in-memory index */
    if (rename(s_vdl.save_path, s_vdl.final_path) != 0) {
        log_write("VDL: rename failed %s -> %s", s_vdl.save_path, s_vdl.final_path);
        remove(s_vdl.save_path);
        s_vdl.state = DL_ERROR;
        return;
    }
    cache_index_add(s_vdl.item_id, s_vdl.ext);

    FILE *meta = fopen(s_vdl.meta_path, "w");
    if (meta) { fputs(s_vdl.item_name, meta); fclose(meta); }

    /* Download companion ASS subtitle file if a URL was provided */
    if (s_vdl.subtitle_url[0]) {
        char ass_path[192];
        if (cache_path(s_vdl.item_id, "ass", ass_path, sizeof(ass_path))) {
            CURL *scurl = curl_easy_init();
            if (scurl) {
                FILE *afp = fopen(ass_path, "wb");
                if (afp) {
                    curl_easy_setopt(scurl, CURLOPT_URL,           s_vdl.subtitle_url);
                    curl_easy_setopt(scurl, CURLOPT_WRITEFUNCTION, vdl_write_cb);
                    curl_easy_setopt(scurl, CURLOPT_WRITEDATA,     afp);
                    curl_easy_setopt(scurl, CURLOPT_FOLLOWLOCATION,1L);
                    curl_easy_setopt(scurl, CURLOPT_SSL_VERIFYPEER,0L);
                    curl_easy_setopt(scurl, CURLOPT_TIMEOUT,       30L);
                    CURLcode src = curl_easy_perform(scurl);
                    fclose(afp);
                    if (src == CURLE_OK)
                        log_write("VDL: saved %s.ass", s_vdl.item_id);
                    else {
                        log_write("VDL: ASS download failed: %s", curl_easy_strerror(src));
                        remove(ass_path);
                    }
                }
                curl_easy_cleanup(scurl);
            }
        }
    }

    s_vdl.state = DL_DONE;
    log_write("VDL: saved %s.%s (%zu bytes)", s_vdl.item_id, s_vdl.ext, s_vdl.bytes);
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

bool dl_queue_video(const char *item_id, const char *item_name,
                    const char *url, const char *sub_track_name,
                    const char *subtitle_url, int64_t runtime_ticks)
{
    if (s_q_count >= DL_QUEUE_MAX) return false;
    dl_q_item_t *q = &s_queue[s_q_count++];
    strncpy(q->item_id, item_id, sizeof(q->item_id)-1);
    q->item_id[sizeof(q->item_id)-1] = '\0';
    strncpy(q->item_name, item_name, sizeof(q->item_name)-1);
    q->item_name[sizeof(q->item_name)-1] = '\0';
    strncpy(q->url, url, sizeof(q->url)-1);
    q->url[sizeof(q->url)-1] = '\0';
    strncpy(q->sub_name, sub_track_name ? sub_track_name : "", sizeof(q->sub_name)-1);
    q->sub_name[sizeof(q->sub_name)-1] = '\0';
    strncpy(q->subtitle_url, subtitle_url ? subtitle_url : "", sizeof(q->subtitle_url)-1);
    q->subtitle_url[sizeof(q->subtitle_url)-1] = '\0';
    strncpy(q->ext, "ts", sizeof(q->ext)-1);
    q->is_video      = true;
    q->runtime_ticks = runtime_ticks;
    if (s_vdl.state == DL_IDLE) dl_process_queue();
    return true;
}

bool dl_queue_book(const char *item_id, const char *display_name, const char *url)
{
    if (s_q_count >= DL_QUEUE_MAX) return false;
    dl_q_item_t *q = &s_queue[s_q_count++];
    strncpy(q->item_id, item_id, sizeof(q->item_id)-1);
    q->item_id[sizeof(q->item_id)-1] = '\0';
    strncpy(q->item_name, display_name, sizeof(q->item_name)-1);
    q->item_name[sizeof(q->item_name)-1] = '\0';
    strncpy(q->url, url, sizeof(q->url)-1);
    q->url[sizeof(q->url)-1] = '\0';
    q->sub_name[0] = '\0';
    strncpy(q->ext, "cbz", sizeof(q->ext)-1);
    q->is_video      = false;
    q->runtime_ticks = 0;
    if (s_vdl.state == DL_IDLE) dl_process_queue();
    return true;
}

bool dl_queue_audio(const char *item_id, const char *display_name, const char *url)
{
    if (s_q_count >= DL_QUEUE_MAX) return false;
    dl_q_item_t *q = &s_queue[s_q_count++];
    strncpy(q->item_id, item_id, sizeof(q->item_id)-1);
    q->item_id[sizeof(q->item_id)-1] = '\0';
    strncpy(q->item_name, display_name, sizeof(q->item_name)-1);
    q->item_name[sizeof(q->item_name)-1] = '\0';
    strncpy(q->url, url, sizeof(q->url)-1);
    q->url[sizeof(q->url)-1] = '\0';
    q->sub_name[0] = '\0';
    strncpy(q->ext, "mp3", sizeof(q->ext)-1);
    q->is_video      = false;
    q->runtime_ticks = 0;
    if (s_vdl.state == DL_IDLE) dl_process_queue();
    return true;
}

void dl_process_queue(void)
{
    if (s_vdl.state != DL_IDLE && s_vdl.state != DL_DONE && s_vdl.state != DL_ERROR)
        return;
    if (s_q_count == 0) return;

    join_thread();  /* clean up previous thread handle */
    s_vdl.state = DL_IDLE;  /* reset for fresh start */

    dl_q_item_t *q = &s_queue[0];

    strncpy(s_vdl.item_id,   q->item_id,   sizeof(s_vdl.item_id)-1);
    s_vdl.item_id[sizeof(s_vdl.item_id)-1] = '\0';
    strncpy(s_vdl.ext,       q->ext,        sizeof(s_vdl.ext)-1);
    s_vdl.ext[sizeof(s_vdl.ext)-1] = '\0';
    strncpy(s_vdl.item_name, q->item_name, sizeof(s_vdl.item_name)-1);
    s_vdl.item_name[sizeof(s_vdl.item_name)-1] = '\0';
    strncpy(s_vdl.url,       q->url,        sizeof(s_vdl.url)-1);
    s_vdl.url[sizeof(s_vdl.url)-1] = '\0';
    strncpy(s_vdl.sub_name,    q->sub_name,    sizeof(s_vdl.sub_name)-1);
    s_vdl.sub_name[sizeof(s_vdl.sub_name)-1] = '\0';
    strncpy(s_vdl.subtitle_url, q->subtitle_url, sizeof(s_vdl.subtitle_url)-1);
    s_vdl.subtitle_url[sizeof(s_vdl.subtitle_url)-1] = '\0';
    s_vdl.is_video      = q->is_video;
    s_vdl.runtime_ticks = q->runtime_ticks;

    /* Build paths using cache API */
    cache_part_path(s_vdl.item_id, s_vdl.ext, s_vdl.save_path,  sizeof(s_vdl.save_path));
    cache_path     (s_vdl.item_id, s_vdl.ext, s_vdl.final_path, sizeof(s_vdl.final_path));
    cache_path     (s_vdl.item_id, "txt",      s_vdl.meta_path,  sizeof(s_vdl.meta_path));

    /* Estimate total bytes from URL bitrate params × duration. */
    size_t est = 0;
    if (q->runtime_ticks > 0) {
        const char *vp = strstr(s_vdl.url, "VideoBitRate=");
        const char *ap = strstr(s_vdl.url, "AudioBitRate=");
        long vbr = vp ? strtol(vp + 13, NULL, 10) : 0;
        long abr = ap ? strtol(ap + 13, NULL, 10) : 0;
        if (vbr + abr > 0) {
            long dur_s = (long)(q->runtime_ticks / 10000000LL);
            est = (size_t)((vbr + abr) / 8 * dur_s);
        }
    }

    /* Shift queue */
    s_q_count--;
    for (int i = 0; i < s_q_count; i++) s_queue[i] = s_queue[i+1];

    s_vdl.bytes           = 0;
    s_vdl.total           = 0;
    s_vdl.estimated_total = est;
    s_vdl.resume_offset   = 0;
    s_vdl.retry_count     = 0;
    s_vdl.cancel          = false;
    s_vdl.state  = DL_ACTIVE;
    s_vdl.thread = threadCreate(vdl_thread_func, NULL, VDL_STACK, VDL_PRIORITY, -2, false);
    if (!s_vdl.thread) {
        log_write("VDL: threadCreate failed");
        s_vdl.state = DL_ERROR;
    }
}

bool dl_queue_has_video(const char *item_id)
{
    if (s_vdl.state == DL_ACTIVE && s_vdl.is_video &&
            strcmp(s_vdl.item_id, item_id) == 0)
        return true;
    for (int i = 0; i < s_q_count; i++) {
        if (s_queue[i].is_video && strcmp(s_queue[i].item_id, item_id) == 0)
            return true;
    }
    return false;
}

int         dl_queue_count(void)              { return s_q_count; }
const char *dl_queue_item_name(int idx)       { return (idx >= 0 && idx < s_q_count) ? s_queue[idx].item_name : ""; }
void        dl_queue_remove(int idx)
{
    if (idx < 0 || idx >= s_q_count) return;
    s_q_count--;
    for (int i = idx; i < s_q_count; i++) s_queue[i] = s_queue[i+1];
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

dl_state_t  dl_get_state(void)    { return (dl_state_t)s_vdl.state; }
size_t      dl_bytes(void)        { return s_vdl.bytes; }
size_t      dl_total(void)        { return s_vdl.total > 0 ? s_vdl.total : s_vdl.estimated_total; }
int         dl_retry_count(void)  { return s_vdl.retry_count; }
const char *dl_item_name(void)    { return s_vdl.item_name; }
const char *dl_sub_name(void)     { return s_vdl.sub_name; }
