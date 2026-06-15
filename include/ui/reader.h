/**
 * reader.h - Manga/comic page reader
 *
 * Downloads and displays pages from Book (CBZ) items via Jellyfin's
 * Images API (GET /Items/{id}/Images/Primary?imageIndex=N).
 * Pages are decoded with stb_image and rendered on the top screen
 * using citro2d, scaled to fit while preserving aspect ratio.
 */

#ifndef JFIN_READER_H
#define JFIN_READER_H

#include <stdbool.h>
#include "api/jellyfin.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Allocate the GPU page texture. Call once at startup after C3D_Init.
 * Returns false on out-of-memory.
 */
bool reader_init(void);

/**
 * Free the GPU texture and reset state.
 */
void reader_cleanup(void);

/**
 * Download and upload one page. Blocking — may take several hundred ms.
 * Returns false on network or decode failure (previous page stays valid).
 */
bool reader_load_page(const jfin_session_t *session,
                      const char *item_id, int page_index);

/**
 * Draw the current page centered within the given screen rectangle.
 * No-op if no page is loaded.
 */
void reader_draw(float x, float y, float w, float h);

/**
 * True when a page has been loaded successfully and is ready to draw.
 */
bool reader_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* JFIN_READER_H */
