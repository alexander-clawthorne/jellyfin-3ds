# Downloads & Offline Cache

> Status: Implemented.

## Overview

The download system lets you save video, audio, and manga (CBZ) files to your SD card for offline playback. Downloads run in the background — you can continue browsing and watching while a queue works through itself.

All files land in `sdmc:/3ds/jellyfin-3ds/cache/`.

---

## What Can Be Downloaded

| Type | Extension | How to Queue |
|------|-----------|--------------|
| Video (transcoded) | `.ts` | X in browse, X while watching (next ep) |
| Video + subtitle burn-in (transcoded) | `.ts` | ZL+X in browse or while watching |
| Video + subtitle burn-in (encoded stream) | `.ts` | ZL+A in browse or while watching |
| Subtitle file only | `.ass` | ZL+Y in browse or while watching |
| Manga / CBZ | `.cbz` | X on a book item in browse |
| Audio | `.mp3` | X on an audio track or album in browse |

---

## Download Combos Explained

### X — Standard Download

- **In browse**: downloads the selected item (video, audio, or CBZ).
- **While watching video**: finds the next episode in the current season that hasn't been downloaded yet and queues it. Searches the item list for the first episode after the current one without a `[D]` badge or an active queue entry.

### ZL+X — Download with Subtitle (Transcoded)

Downloads the next undownloaded episode (while watching) or the selected video (in browse) with the subtitle burned into the video by the Jellyfin server using its normal transcode pipeline. The subtitle stream index is determined by:
1. The currently active subtitle track (if one is selected while watching)
2. The first available subtitle track for the item (if none is selected or called from browse)

The resulting `.ts` file has subtitles permanently rendered into the picture. No companion `.ass` file is needed.

### ZL+A — Download with Subtitle (Encoded Stream)

Same as ZL+X in terms of what gets downloaded, but uses a different Jellyfin API path that appends `SubtitleMethod=Encode&StartTimeTicks=1`. This forces Jellyfin to encode the subtitle even for content it would otherwise direct-play. Use ZL+A if ZL+X produces a video without subtitles visible.

### ZL+Y — Download Subtitle File Only

Downloads just the `.ass` subtitle file for the selected or currently playing item. Saved as `ITEMID.ass` alongside any existing `.ts` file. Useful for adding subtitles to a video that was downloaded without them.

---

## Download Queue

The downloader is a single background thread that processes one item at a time. Multiple items can be queued and will be processed in order.

### Queue Capacity

Up to 16 items can be queued simultaneously (defined in `downloader.c`). `dl_queue_video()` returns false if the queue is full.

### Queue Management

Items are added by the UI and processed by `dl_process_queue()`, called once per main loop frame. When the active download completes (state transitions to `DL_DONE` or `DL_ERROR`), `dl_process_queue()` immediately starts the next queued item.

### Download State Machine

```
DL_IDLE   → dl_queue_*() + dl_process_queue()  → DL_ACTIVE
DL_ACTIVE → download complete                   → DL_DONE
DL_ACTIVE → network error / cancelled           → DL_ERROR
DL_DONE   → dl_process_queue()                  → DL_IDLE (or DL_ACTIVE for next item)
DL_ERROR  → dl_process_queue()                  → DL_IDLE (item dropped, next starts)
```

---

## File Naming

### SD Card Paths

```
sdmc:/3ds/jellyfin-3ds/cache/
├── {ITEM_ID}.ts        Completed video file
├── {ITEM_ID}.ts.part   In-progress video download (renamed on completion)
├── {ITEM_ID}.txt       Companion title file (plain text, contains item name)
├── {ITEM_ID}.ass       Companion subtitle file (if downloaded via ZL+Y)
├── {ITEM_ID}.mp3       Audio file
├── {ITEM_ID}.cbz       Manga/CBZ file
```

The `.part` suffix convention ensures only complete files are ever visible to the player. `cache_init()` at startup deletes any leftover `.part` files from a previous interrupted download.

### Companion `.txt` Files

Every video download also creates a `{ITEM_ID}.txt` file containing the human-readable item title (e.g. `"Breaking Bad / Season 1 / E01 - Pilot"`). The Downloads Manager uses this file to display the item name without needing a network connection to look it up.

### Download-Next Naming

When X or ZL+X queues the next episode while watching, the title is built as:
```
{Series Name} / Season {N} / E{##} - {Episode Name}
```

If the series name isn't available from the item metadata, the breadcrumb stack (parent folder names) is used as a fallback.

---

## Progress Tracking

While a download is active, `dl_bytes()` and `dl_total()` report the current progress. The Downloads Manager shows:

- **Progress bar**: `dl_bytes() / dl_total()` (or an animated indicator if `Content-Length` was omitted)
- **Download speed**: computed as bytes per second over a rolling window
- **Estimated size**: derived from `runtime_ticks` × bitrate if `Content-Length` is missing (server-side transcode streams often omit it)
- **ETA**: `remaining_bytes / current_speed`

---

## Cache Index

The in-memory cache index (`cache.c`) is built once at startup by scanning `sdmc:/3ds/jellyfin-3ds/cache/`. It allows `cache_has(item_id, ext)` to answer "is this item downloaded?" in O(1) without hitting the filesystem every frame.

The browse list queries `cache_has()` for each visible item to display the `[D]` downloaded badge. FAT32 `stat()` calls are far too slow for per-frame use (each takes ~1ms on 3DS).

The index is updated immediately when:
- A download completes (`cache_index_add()`)
- A file is deleted from the Downloads Manager (`cache_remove()`)

### Index Capacity

The index holds up to 512 items (configurable via `CACHE_MAX_ENTRIES` in `cache.c`). `cache_is_full()` returns true when the limit is reached; new downloads are blocked until files are deleted.

---

## Downloads Manager (VIEW_DOWNLOADS)

Access via **Settings → Manage Downloads**.

### Layout

- **Top screen**: active download (progress bar, speed, ETA, queue position) + numbered queue list
- **Bottom screen**: list of completed files on SD card, grouped by type → series → season → episode

### Controls

| Button | Action |
|--------|--------|
| A | Open selected file (CBZ → reader, .ts/.mp3 → player) |
| B | Back to settings |
| X | Delete selected file / remove queued item / cancel active download |
| Y | Go to Now Playing |
| D-pad Up/Down | Move cursor (hold to scroll) |
| D-pad Left/Right | Scroll long filename sideways (hold) |
| Circle pad Up/Down | Navigate the download queue on the top screen |

### File Grouping

Completed files are sorted and grouped:
1. By type: video (blue) → CBZ/books (red) → audio (purple)
2. Within video: by series name → season number → episode number
3. Title displayed is read from the companion `.txt` file

### Deleting Files

Pressing X on a completed file calls `cache_remove()`, which deletes the `.ts` / `.mp3` / `.cbz` file, any companion `.txt` and `.ass` files, and drops the entry from the in-memory index. The `[D]` badge disappears from browse on the next render.

Pressing X on the active download cancels it and deletes the `.part` file. Pressing X on a queued item removes it from the queue.

---

## API Reference

```c
// Queue a video (subtitle_url may be NULL for no companion sub)
bool dl_queue_video(const char *item_id, const char *item_name,
                    const char *url, const char *sub_track_name,
                    const char *subtitle_url, int64_t runtime_ticks);

// Queue subtitle file only
bool dl_queue_subtitle_only(const char *item_id, const char *item_name,
                            const char *subtitle_url);

// Queue a CBZ
bool dl_queue_book(const char *item_id, const char *display_name,
                   const char *url);

// Queue audio
bool dl_queue_audio(const char *item_id, const char *display_name,
                    const char *url);

// Check if already queued (avoids duplicates)
bool dl_queue_has_video(const char *item_id);

// Must be called each main loop frame
void dl_process_queue(void);

// Cancel active download
void dl_cancel(void);

// Status
dl_state_t  dl_get_state(void);
size_t      dl_bytes(void);
size_t      dl_total(void);
const char *dl_item_name(void);
const char *dl_sub_name(void);
int         dl_queue_count(void);
const char *dl_queue_item_name(int idx);
void        dl_queue_remove(int idx);
```
