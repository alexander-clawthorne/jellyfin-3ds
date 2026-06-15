/**
 * reader.h - Manga/comic CBZ reader
 *
 * Jellyfin does not expose individual CBZ pages via the Images API.
 * The web client downloads the entire CBZ archive via /Items/{id}/Download
 * and extracts pages with JSZip. We do the same: download asynchronously
 * to the SD card, parse the ZIP central directory, then decompress pages
 * on demand with zlib.
 *
 * State machine:
 *   READER_IDLE → (reader_open_book) → READER_DOWNLOADING
 *   READER_DOWNLOADING → (download + cbz_open) → READER_READY
 *   READER_DOWNLOADING → (error) → READER_ERROR
 *   READER_READY → (reader_load_page) → page visible
 *   any → (reader_cancel or reader_cleanup) → READER_IDLE
 */

#ifndef JFIN_READER_H
#define JFIN_READER_H

#include <stddef.h>
#include <stdbool.h>
#include "api/jellyfin.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    READER_IDLE,
    READER_DOWNLOADING,  /* CBZ download in progress (background thread) */
    READER_READY,        /* CBZ parsed, pages accessible */
    READER_ERROR,        /* download or parse failed */
} reader_state_t;

/** Allocate GPU texture. Call once after C3D_Init. */
bool reader_init(void);

/** Free GPU texture. */
void reader_cleanup(void);

/**
 * Start an async download of item_id's CBZ from Jellyfin.
 * Saves to sdmc:/3ds/jellyfin-3ds/book.cbz, then parses the ZIP.
 * State advances to READER_READY (or READER_ERROR) on completion.
 * If a download is already running, it is cancelled first.
 */
void reader_open_book(const jfin_session_t *session, const char *item_id);

/** Cancel an in-progress download. State returns to READER_IDLE. */
void reader_cancel(void);

/**
 * Extract, decode, and upload page[idx] to the GPU.
 * Pass rotated=true to bake a 90° CCW rotation into the pixel data so
 * reader_draw can always do a simple aspect-fit (no GPU rotation needed).
 * Call only when reader_get_state() == READER_READY.
 * Synchronous but fast (~5–20 ms for decompress + Morton tile).
 */
bool reader_load_page(int page_index, bool rotated);

/** Current state. */
reader_state_t reader_get_state(void);

/** Bytes downloaded so far (READER_DOWNLOADING only). */
size_t reader_dl_bytes(void);

/** Total file size from Content-Length (0 if unknown). */
size_t reader_dl_total(void);

/** Number of image pages in the open CBZ (valid when >= READER_READY). */
int reader_page_count(void);

/** True when a page has been loaded and is ready to draw. */
bool reader_page_ready(void);

/**
 * Draw the current page using a simple aspect-fit.
 * Rotation is baked into the texture by reader_load_page — no angle needed here.
 */
void reader_draw(float x, float y, float w, float h);

#ifdef __cplusplus
}
#endif

#endif /* JFIN_READER_H */
