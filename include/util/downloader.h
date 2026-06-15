/**
 * downloader.h - Background video file downloader
 *
 * Downloads a pre-built Jellyfin stream URL to the SD card.
 * The caller constructs the URL (e.g. via jfin_get_video_stream with
 * SubtitleMethod=Encode) and passes it here.  api_key is already embedded
 * in the URL so no separate auth header is needed.
 *
 * Saves to: sdmc:/3ds/jellyfin-3ds/video_ITEMID.ts
 * Metadata: sdmc:/3ds/jellyfin-3ds/video_ITEMID.txt  (human-readable title)
 */

#ifndef JFIN_DOWNLOADER_H
#define JFIN_DOWNLOADER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VDL_DIR "sdmc:/3ds/jellyfin-3ds"

typedef enum {
    DL_IDLE,
    DL_ACTIVE,
    DL_DONE,
    DL_ERROR,
} dl_state_t;

/* Queue a video download. Returns true if added, false if queue full. */
bool dl_queue_video(const char *item_id, const char *item_name,
                    const char *url, const char *sub_track_name);

/* Queue a book/CBZ download. url must include api_key.
 * Skips download if the file already exists (reader cache hit). */
bool dl_queue_book(const char *item_id, const char *display_name, const char *url);

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

/** Free thread handle.  Call on shutdown. */
void dl_cleanup(void);

dl_state_t  dl_get_state(void);
size_t      dl_bytes(void);
size_t      dl_total(void);
const char *dl_item_name(void);
const char *dl_sub_name(void);

#ifdef __cplusplus
}
#endif

#endif /* JFIN_DOWNLOADER_H */
