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
    volatile size_t estimated_total; /* computed from bitrate×duration when Content-Length absent */
    volatile bool   cancel;

    char save_path[192];        /* sdmc:/.../video_ITEMID.ts or cbz_ITEMID.cbz */
    char meta_path[192];        /* sdmc:/.../video_ITEMID.txt */
    char url[2048];
    char item_name[256];
    char sub_name[128];         /* subtitle track name being burned in */
    bool is_video;
    int64_t runtime_ticks;      /* item duration for size estimation */
} s_vdl;

/* ── Download queue ─────────────────────────────────────────────────── */

#define DL_QUEUE_MAX 10

typedef struct {
    char    item_id[64];
    char    item_name[256];
    char    url[2048];
    char    save_path[192];
    char    meta_path[192];
    char    sub_name[128];
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
    if (total > 0) s_vdl.total = (size_t)total;
    return s_vdl.cancel ? 1 : 0;
}

/* ── Thread ─────────────────────────────────────────────────────────── */

static void vdl_thread_func(void *arg)
{
    (void)arg;

    mkdir("sdmc:/3ds", 0777);
    mkdir(VDL_DIR, 0777);

    /* Book/CBZ: if already cached by the reader, just write .txt and done */
    if (!s_vdl.is_video) {
        FILE *test = fopen(s_vdl.save_path, "rb");
        if (test) {
            fclose(test);
            log_write("VDL: book already cached %s", s_vdl.save_path);
            FILE *meta = fopen(s_vdl.meta_path, "w");
            if (meta) { fputs(s_vdl.item_name, meta); fclose(meta); }
            s_vdl.state = DL_DONE;
            return;
        }
    }

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

bool dl_queue_video(const char *item_id, const char *item_name,
                    const char *url, const char *sub_track_name,
                    int64_t runtime_ticks)
{
    if (s_q_count >= DL_QUEUE_MAX) return false;
    dl_q_item_t *q = &s_queue[s_q_count++];
    strncpy(q->item_name, item_name, sizeof(q->item_name)-1);
    q->item_name[sizeof(q->item_name)-1] = '\0';
    strncpy(q->url, url, sizeof(q->url)-1);
    q->url[sizeof(q->url)-1] = '\0';
    strncpy(q->sub_name, sub_track_name ? sub_track_name : "", sizeof(q->sub_name)-1);
    q->sub_name[sizeof(q->sub_name)-1] = '\0';
    snprintf(q->save_path, sizeof(q->save_path), VDL_DIR "/video_%s.ts", item_id);
    snprintf(q->meta_path, sizeof(q->meta_path), VDL_DIR "/video_%s.txt", item_id);
    strncpy(q->item_id, item_id, sizeof(q->item_id)-1);
    q->item_id[sizeof(q->item_id)-1] = '\0';
    q->is_video       = true;
    q->runtime_ticks  = runtime_ticks;
    if (s_vdl.state == DL_IDLE) dl_process_queue();
    return true;
}

bool dl_queue_book(const char *item_id, const char *display_name, const char *url)
{
    if (s_q_count >= DL_QUEUE_MAX) return false;
    dl_q_item_t *q = &s_queue[s_q_count++];
    strncpy(q->item_name, display_name, sizeof(q->item_name)-1);
    q->item_name[sizeof(q->item_name)-1] = '\0';
    strncpy(q->url, url, sizeof(q->url)-1);
    q->url[sizeof(q->url)-1] = '\0';
    q->sub_name[0] = '\0';
    snprintf(q->save_path, sizeof(q->save_path), VDL_DIR "/cbz_%s.cbz", item_id);
    snprintf(q->meta_path, sizeof(q->meta_path), VDL_DIR "/cbz_%s.txt", item_id);
    strncpy(q->item_id, item_id, sizeof(q->item_id)-1);
    q->item_id[sizeof(q->item_id)-1] = '\0';
    q->is_video = false;
    if (s_vdl.state == DL_IDLE) dl_process_queue();
    return true;
}

bool dl_queue_audio(const char *item_id, const char *display_name, const char *url)
{
    if (s_q_count >= DL_QUEUE_MAX) return false;
    dl_q_item_t *q = &s_queue[s_q_count++];
    strncpy(q->item_name, display_name, sizeof(q->item_name)-1);
    q->item_name[sizeof(q->item_name)-1] = '\0';
    strncpy(q->url, url, sizeof(q->url)-1);
    q->url[sizeof(q->url)-1] = '\0';
    q->sub_name[0] = '\0';
    snprintf(q->save_path, sizeof(q->save_path), VDL_DIR "/audio_%s.mp3", item_id);
    snprintf(q->meta_path, sizeof(q->meta_path), VDL_DIR "/audio_%s.txt", item_id);
    strncpy(q->item_id, item_id, sizeof(q->item_id)-1);
    q->item_id[sizeof(q->item_id)-1] = '\0';
    q->is_video = false;
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

    strncpy(s_vdl.item_name, q->item_name, sizeof(s_vdl.item_name)-1);
    s_vdl.item_name[sizeof(s_vdl.item_name)-1] = '\0';
    strncpy(s_vdl.url, q->url, sizeof(s_vdl.url)-1);
    s_vdl.url[sizeof(s_vdl.url)-1] = '\0';
    strncpy(s_vdl.save_path, q->save_path, sizeof(s_vdl.save_path)-1);
    s_vdl.save_path[sizeof(s_vdl.save_path)-1] = '\0';
    strncpy(s_vdl.meta_path, q->meta_path, sizeof(s_vdl.meta_path)-1);
    s_vdl.meta_path[sizeof(s_vdl.meta_path)-1] = '\0';
    strncpy(s_vdl.sub_name, q->sub_name, sizeof(s_vdl.sub_name)-1);
    s_vdl.sub_name[sizeof(s_vdl.sub_name)-1] = '\0';
    s_vdl.is_video      = q->is_video;
    s_vdl.runtime_ticks = q->runtime_ticks;

    /* Estimate total bytes from URL bitrate params × duration.
     * Jellyfin transcoded streams omit Content-Length; this gives a
     * reasonable progress bar even before any data arrives. */
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
    char expected[192];
    snprintf(expected, sizeof(expected), VDL_DIR "/video_%s.ts", item_id);
    if (s_vdl.state == DL_ACTIVE && strcmp(s_vdl.save_path, expected) == 0)
        return true;
    for (int i = 0; i < s_q_count; i++) {
        if (strcmp(s_queue[i].save_path, expected) == 0)
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

dl_state_t  dl_get_state(void)  { return (dl_state_t)s_vdl.state; }
size_t      dl_bytes(void)       { return s_vdl.bytes; }
size_t      dl_total(void)       { return s_vdl.total > 0 ? s_vdl.total : s_vdl.estimated_total; }
const char *dl_item_name(void)   { return s_vdl.item_name; }
const char *dl_sub_name(void)    { return s_vdl.sub_name; }
