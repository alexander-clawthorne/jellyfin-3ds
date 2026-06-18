# Subtitles

> Status: Implemented. Client-side ASS rendering during streaming; server-side burn-in for downloads.

## Overview

Subtitles work differently depending on the context:

| Context | Method | How |
|---------|--------|-----|
| Streaming (online) | Client-side render | .ass file downloaded separately; cues rendered on GPU |
| Downloaded video (ZL+X) | Server burn-in (transcoded) | Server bakes subtitle into the TS transcode |
| Downloaded video (ZL+A) | Server burn-in (encoded stream) | Server bakes subtitle using `SubtitleMethod=Encode` |
| Subtitle-only download (ZL+Y) | .ass file download | Raw .ass file saved to SD card alongside video |
| Offline playback | Client-side render | Companion .ass loaded from SD card if present |

---

## Stream Selection

### Fetching Available Tracks

When a video item is opened, `jfin_get_subtitle_streams()` fetches the available subtitle tracks:

```
GET /Videos/{id}/PlaybackInfo
```

The response includes `MediaStreams[]` filtered to `Type == "Subtitle"`. Each track exposes:
- `Index` — stream index passed to the server to identify the track
- `Language` — ISO 639-2 code (e.g. `"eng"`)
- `DisplayTitle` — human-readable label
- `IsDefault`, `IsForced`

Up to 10 tracks are stored in `jfin_subtitle_list_t`.

### Selecting a Track

Press **Y** while watching video to cycle through available subtitle tracks (Off → track 1 → track 2 → … → Off). Selecting a track:

1. Requests the .ass subtitle URL from the stream info
2. Calls `video_player_load_subtitles(url)` to fetch and parse the .ass file
3. Sets the language preference in `state->subtitle_lang_pref` for sticky tracking

Pressing Y when the last track is active returns to Off (`video_player_clear_subtitles()`).

### Sticky Language Preference

Once a subtitle track is selected, the ISO 639-2 language code is saved as `subtitle_lang_pref`. On the next episode in auto-advance, the player searches the new episode's subtitle list for a track matching that language and activates it automatically. This persists across the session (not saved to config.ini).

---

## Client-Side ASS Rendering

### ASS File Format

Jellyfin serves subtitle tracks as .ass (Advanced SubStation Alpha) files. The player downloads the full file before playback begins (subtitle pre-roll) and parses it into a cue table.

### Parsing

The parser (`src/video/video_player.c`) reads the `[Events]` section and extracts `Dialogue:` lines. From each line:

- `Start` / `End` timestamps → converted to Jellyfin 100ns ticks
- `Text` field → stripped of `{\...}` override tags (color, position, etc.) with basic style application:
  - `\an<N>` → ASS numpad alignment (1–9, default 2 = bottom-center)
  - `\pos(x,y)` → absolute screen position (scaled to 400×240)
  - `\c&H<BBGGRR>&` → text color (converted to C2D_Color32 RGBA8)
  - `\N` / `\n` → line break
- All unrecognized tags are stripped, text content is preserved

Up to 500 cues are stored in a static table. Memory use: ~50KB.

### Rendering

Each frame, `video_player_get_subtitles()` returns all cues whose `[start_ticks, end_ticks)` window contains the current playback position (adjusted for the seek offset used when the stream was requested).

The main thread draws each cue in `ui_render_now_playing()`:

- Alignment 2 (bottom-center): text drawn at bottom of top screen, centered
- Alignment 7 (top-left) and others: positioned using `screen_x` / `screen_y`
- Multi-line cues: each `\n`-separated segment drawn on its own line
- Color: `rgba(cue->color)` if non-zero, otherwise white (`COLOR_TEXT_PRIMARY`)
- Font size: 0.45f scale (smaller than UI text to avoid obscuring picture)

### Auto-Retry on Transcode Startup

When a video is started with subtitles enabled, Jellyfin may take 20–30 seconds to begin transcoding. During this window the player can fail to find the subtitle track because the media info isn't fully available yet. The player retries up to 3 times with a short delay:

```c
state->video_retry_count   // 0 = first attempt, max 3
state->video_retry_timer   // countdown frames before next retry
state->video_retry_ticks   // seek position to restore on retry
```

On retry, the stream is re-requested from the same position. If all 3 retries fail, playback continues without subtitles.

---

## Subtitle Downloads

### ZL+X — Transcoded Burn-in

Queues a video download where the subtitle is burned into the video stream by the Jellyfin server using its normal transcode pipeline. The server re-encodes the video with the subtitle rendered into the picture.

URL includes `SubtitleStreamIndex={idx}` (no `SubtitleMethod` parameter — Jellyfin defaults to its transcoded path). This uses `jfin_get_video_stream()` with the subtitle index set.

### ZL+A — Encoded Stream Burn-in

Queues a video download using `jfin_get_video_stream_encoded()`, which appends:

```
&SubtitleStreamIndex={idx}
&SubtitleMethod=Encode
&StartTimeTicks=1
```

`StartTimeTicks=1` is critical: without it, Jellyfin detects that no seek is needed and may return a direct stream URL that bypasses the transcode pipeline, causing the subtitle encode instruction to be ignored.

The practical difference from ZL+X is that this path forces Jellyfin to use its "encode" subtitle method even when the source file could otherwise be direct-played. Choose ZL+A when ZL+X produces a video without subtitles burned in.

### ZL+Y — Subtitle File Only

Downloads just the .ass subtitle file for the selected or currently playing item. The file is saved as `sdmc:/3ds/jellyfin-3ds/cache/ITEMID.ass`. Useful when a video was previously downloaded without a subtitle and you want to add one without re-downloading the entire video.

When an offline video is played, the player checks for a companion `.ass` file at `ITEMID.ass` and loads it automatically.

---

## Offline Subtitle Playback

When playing a video from the Downloads Manager (offline mode), the player checks for `ITEMID.ass` in the cache directory. If found, it calls `video_player_load_subtitles(path)` with the local path.

The subtitle toggle (Y key equivalent) in offline mode switches between the loaded .ass file and off. Since the offline video does not carry audio PTS, subtitle timing is based on the local file position derived from the seek offset stored at download time.

---

## API Reference

```c
// Fetch available subtitle tracks
bool jfin_get_subtitle_streams(const jfin_session_t *session,
                               const char *item_id,
                               jfin_subtitle_list_t *out);

// Get stream URL with subtitle URL populated (client-side rendering)
bool jfin_get_video_stream(const jfin_session_t *session,
                           const char *item_id, int64_t start_ticks,
                           bool is_3d, int subtitle_stream_index,
                           jfin_stream_t *out);
// out->subtitle_url is the .ass download URL

// Get encoded stream URL with subtitle burned in (for ZL+A downloads)
bool jfin_get_video_stream_encoded(const jfin_session_t *session,
                                   const char *item_id,
                                   int subtitle_stream_index,
                                   jfin_stream_t *out);
// out->subtitle_url is always empty — subtitle is in the video

// Runtime subtitle control
void video_player_load_subtitles(const char *url_or_path);
void video_player_clear_subtitles(void);
int  video_player_get_subtitles(vp_subtitle_t *out, int max_count);
```
