# Manga / CBZ Reader

> Status: Implemented.

## Overview

The manga reader (`VIEW_READER`) downloads CBZ archives from Jellyfin and renders them page by page on the 3DS screens. CBZ files are ZIP archives containing JPEG or PNG image files — one per page — sorted by filename.

The reader uses a state machine with two entry paths:
- **Online**: item selected from browse → download → open
- **Offline**: item selected from Downloads Manager → open local file directly

---

## Architecture

### Components

| Component | File | Role |
|-----------|------|------|
| `reader.c` | `src/ui/reader.c` | Download, CBZ parse, GPU upload, draw |
| `cbz.c` | `src/util/cbz.c` | ZIP central directory parse, page decompress |
| `stb_image` | vendored | JPEG/PNG decode |
| `ui.c` | `src/ui/ui.c` | Input handling, view transitions |

### State Machine

```
READER_IDLE
    │
    ├─ reader_open_book(session, item_id)   [online]
    │       ↓
    │  READER_DOWNLOADING
    │       │  curl → ITEMID.cbz → cbz_open()
    │       ↓
    │  READER_READY ──────── reader_load_page(idx) ──→ page visible
    │       │
    │  READER_ERROR (download or parse failed)
    │
    └─ reader_open_local(path)             [offline]
            ↓
       READER_READY immediately (no download needed)
```

---

## CBZ Download

When a book item is opened from browse:

1. `reader_open_book()` starts a background download thread
2. `curl` fetches `/Items/{id}/Download?api_key={token}` → streams to `sdmc:/3ds/jellyfin-3ds/cache/ITEMID.cbz`
3. Download progress is reported via `reader_dl_bytes()` / `reader_dl_total()` and shown on the bottom screen
4. On completion, `cbz_open()` parses the ZIP central directory and builds a sorted page index
5. State transitions to `READER_READY`

If the file already exists in cache, the download is skipped and `cbz_open()` is called immediately on the existing file.

---

## CBZ Parsing (`cbz.c`)

### ZIP Central Directory

CBZ files are standard ZIP archives. The parser:

1. Seeks to the End of Central Directory (EOCD) record at the end of the file
2. Reads the central directory at the offset specified in EOCD
3. For each entry: records `lhdr_off` (local file header offset), `comp_sz`, `decomp_sz`, `method`, and `name`
4. Filters to image files only (`.jpg`, `.jpeg`, `.png` extensions)
5. Sorts entries by filename to establish page order (most CBZ archives use `001.jpg`, `002.jpg`, etc.)

Up to 1024 pages are indexed (`CBZ_MAX_PAGES`).

### Page Decompression

`cbz_page_data(c, idx, &out_sz)` extracts a single page:

1. Seeks to the local file header at `lhdr_off`
2. Reads and validates the local header magic (`PK\x03\x04`)
3. Skips filename and extra field to reach compressed data
4. If `method == 0` (stored): copies data directly
5. If `method == 8` (deflate): decompresses with zlib `inflateInit2(..., -15)` (raw deflate, no zlib wrapper)
6. Returns a `malloc()`'d buffer; caller must `free()` it

---

## Page Rendering

### Loading a Page

`reader_load_page(idx, rotated)`:

1. Calls `cbz_page_data()` to decompress the image bytes
2. Passes bytes to `stb_image` (`stbi_load_from_memory`) → RGBA8 pixel buffer
3. Optionally bakes a 90° CCW rotation into the pixel data (for portrait → landscape mode)
4. Morton-tiles the RGBA8 data into a `C3D_Tex` GPU texture
5. Frees the decompressed and decoded buffers

This is synchronous but fast (~5–20ms depending on image size and JPEG complexity). The UI calls it on page change and shows the previous page until the new one is ready.

### Aspect-Fit with Zoom and Pan

`reader_draw(x, y, w, h, zoom, pan_x, pan_y)` renders the loaded page texture:

- At `zoom=1.0`: the image is scaled to fit entirely within the viewport (letterboxed/pillarboxed as needed)
- At `zoom>1.0`: the image is scaled up by that factor relative to the fit size
- `pan_x` / `pan_y`: shift the image by these pixel amounts after zoom scaling
- Drawn as a citro2d sprite with a `C2D_ImageTint` transform

### Split-Screen Mode

In split-screen mode the page spans both the top screen (400×240) and bottom screen (320×240) simultaneously, giving an effective 400×480 viewport for tall pages.

- `reader_draw_split_top(zoom, pan_x, pan_y)`: renders the upper portion of the page on the top screen, scaled to fit width=400px
- `reader_draw_split_bottom(zoom, pan_x, pan_y)`: renders the lower portion seamlessly, continuing from where the top screen left off

The split is calculated so that the image "breaks" at the exact pixel that falls at the boundary between the two screens at the current zoom and pan. Entering split mode automatically resets zoom to fit the full page height across both screens combined (480px).

---

## Controls

### Normal Mode

| Button | Action |
|--------|--------|
| B | Back to browse |
| L / R | Previous / next page |
| D-pad Left / Right | Previous / next page |
| D-pad Up / Down | Zoom in / out |
| Circle pad | Pan (horizontal and vertical) |
| SELECT | Toggle rotation (portrait ↔ landscape) |
| START | Enter split-screen mode |

### Split-Screen Mode

| Button | Action |
|--------|--------|
| B / START | Return to normal mode |
| L / R | Previous / next page |
| D-pad Left / Right | Previous / next page |
| D-pad Up / Down | Zoom in / out |
| Circle pad Up / Down | Scroll the page vertically |
| Circle pad Left / Right | Pan horizontally |

### Rotation

Pressing SELECT in normal mode toggles 90° CCW rotation. The rotation is baked into the GPU texture during `reader_load_page()` — no runtime GPU rotation is needed. This means switching rotation forces a page reload, which takes the normal ~5–20ms.

Rotation is useful for wide manga panels that benefit from the landscape orientation of the 3DS held sideways.

---

## Reading from Downloads Manager

Books that have already been downloaded show a `[D]` badge in browse and are listed in the Downloads Manager. Pressing A on a `.cbz` entry in the Downloads Manager calls `reader_open_local(path)` with the full SD card path, bypassing the download step entirely and going straight to `READER_READY`.

---

## Error Handling

| Condition | Result |
|-----------|--------|
| Download fails (network error) | `READER_ERROR` — message shown on bottom screen; B returns to browse |
| File is not a valid ZIP | `READER_ERROR` |
| Page has no image files | `READER_READY` but `reader_page_count() == 0` |
| `stb_image` fails to decode a page | Page is skipped; reader advances to next |
| CBZ has more than 1024 pages | Only first 1024 pages are indexed |

---

## Memory Notes

Page images are decompressed and decoded one at a time and freed immediately after upload to the GPU. Only one page is in RAM at a time (plus the GPU texture). A single 1080p manga page uncompressed is at most ~8MB (3840×2160 RGBA8) but typical manga pages are 1200×1800 and around 8MB uncompressed — this fits comfortably in the 3DS heap.

The GPU texture is a single `C3D_Tex` allocated at startup by `reader_init()` and reused for every page load. Its size is rounded up to the next power-of-two in both dimensions (required by the PICA200 GPU).
