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

/**
 * Start a background video download.
 * item_id   : Jellyfin item ID (used for the filename).
 * item_name : Human-readable title (saved to companion .txt).
 * url       : Full stream URL — api_key must be embedded in the query string.
 * If a download is already active it is cancelled first.
 */
void dl_start_video(const char *item_id, const char *item_name, const char *url);

/** Cancel the active download (aborts curl, removes partial file). */
void dl_cancel(void);

/** Free thread handle.  Call on shutdown. */
void dl_cleanup(void);

dl_state_t  dl_get_state(void);
size_t      dl_bytes(void);
size_t      dl_total(void);
const char *dl_item_name(void);

#ifdef __cplusplus
}
#endif

#endif /* JFIN_DOWNLOADER_H */
