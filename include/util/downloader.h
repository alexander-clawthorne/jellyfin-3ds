/**
 * downloader.h - Background media file downloader
 *
 * Downloads a pre-built Jellyfin stream URL to the SD card cache.
 * The caller constructs the URL (e.g. via jfin_get_video_stream with
 * SubtitleMethod=Encode) and passes it here.  api_key is already embedded
 * in the URL so no separate auth header is needed.
 *
 * Files are saved to sdmc:/3ds/jellyfin-3ds/cache/ using the upstream
 * cache API (cache_path / cache_part_path / cache_index_add).
 * Naming: CACHE_DIR/ITEMID.ts, CACHE_DIR/ITEMID.cbz, CACHE_DIR/ITEMID.mp3
 * Companion title: CACHE_DIR/ITEMID.txt
 */

#ifndef JFIN_DOWNLOADER_H
#define JFIN_DOWNLOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DL_IDLE,
    DL_ACTIVE,
    DL_DONE,
    DL_ERROR,
} dl_state_t;

/* Queue a video download. Returns true if added, false if queue full.
 * subtitle_url: ASS subtitle URL to download alongside the video (or NULL).
 * runtime_ticks: item duration in 100ns ticks (used to estimate download
 * size when the server omits Content-Length). Pass 0 if unknown. */
bool dl_queue_video(const char *item_id, const char *item_name,
                    const char *url, const char *sub_track_name,
                    const char *subtitle_url, int64_t runtime_ticks);

/* Queue a subtitle-only download (.ass). Use when the video is already cached
 * but has no companion subtitle, or to replace a bad one. */
bool dl_queue_subtitle_only(const char *item_id, const char *item_name,
                            const char *subtitle_url);

/* Queue a book/CBZ download. url must include api_key.
 * Skips download if the file already exists in the cache. */
bool dl_queue_book(const char *item_id, const char *display_name, const char *url);

/* Queue an audio download. url should be a Jellyfin audio stream URL (MP3). */
bool dl_queue_audio(const char *item_id, const char *display_name, const char *url);

/* Returns true if item_id is already active or queued for video download.
 * Used to skip items already in-flight when searching for the next to queue. */
bool dl_queue_has_video(const char *item_id);

/* Start the next queued item if the downloader is idle. Call from main loop. */
void dl_process_queue(void);

/* Cancel the active download. */
void dl_cancel(void);

/* Number of items waiting in queue (not counting active download). */
int dl_queue_count(void);

/* Name of queued item at position idx (0 = next to download). */
const char *dl_queue_item_name(int idx);

/* Remove a queued item by index. */
void dl_queue_remove(int idx);

/** Free thread handle. Call on shutdown. */
void dl_cleanup(void);

dl_state_t  dl_get_state(void);
size_t      dl_bytes(void);
size_t      dl_total(void);
int         dl_retry_count(void);  /* 0 = first attempt, 1 or 2 = retry */
const char *dl_item_name(void);
const char *dl_sub_name(void);

#ifdef __cplusplus
}
#endif

#endif /* JFIN_DOWNLOADER_H */
