# Offline Playback

> Status: Implemented for video and CBZ. Audio offline not yet wired up.

## Overview

Once content is downloaded to the SD card, it can be played without a network connection. The Downloads Manager (`VIEW_DOWNLOADS`) is the entry point for all offline playback.

---

## What Can Be Played Offline

| Type | File | Player |
|------|------|--------|
| Video | `ITEMID.ts` | Video player (same as streaming) |
| CBZ / Manga | `ITEMID.cbz` | Manga reader |
| Audio | `ITEMID.mp3` | *(downloaded, but offline audio path not yet wired up)* |

---

## Starting Offline Playback

From the Downloads Manager, navigate to a file and press **A**:

- `.cbz` → calls `reader_open_local(path)` → manga reader opens immediately
- `.ts` → sets `state->now_playing_offline = true`, `state->now_playing_local_path = path`, transitions to `VIEW_NOW_PLAYING`, and starts `video_player_play(local_path, sub_path, ...)`

The `sdmc:` path is passed directly to the video player. FFmpeg's `avformat_open_input` handles local file I/O via the `file://` protocol shim in the 3DS filesystem layer.

---

## Video Offline Playback

### Player Startup

The video player receives a local file path (`sdmc:/3ds/jellyfin-3ds/cache/ITEMID.ts`) instead of an HTTP URL. The network thread is not started. FFmpeg reads directly from the SD card via the `file` protocol. All other pipeline stages (demux, MVD decode, convert, render) are identical to streaming.

### Companion Subtitle

At startup, the player checks for a companion `.ass` file:

```
sdmc:/3ds/jellyfin-3ds/cache/ITEMID.ass
```

If found, `video_player_load_subtitles(sub_path)` is called immediately, and the subtitle cue table is populated before the first frame is rendered. The `now_playing_sub_path` field in UI state holds this path.

If no `.ass` file is found, playback starts without subtitles (`subtitle_url` is empty). Y (subtitle cycle) is not available in offline mode because there is no Jellyfin session to query for additional tracks.

### Subtitle Toggle

Offline mode exposes a simple on/off toggle for the companion subtitle:

- If `offline_subs_on == true` and a `.ass` file exists: subtitles are rendered
- If `offline_subs_on == false`: `video_player_clear_subtitles()` is called

The toggle state (`offline_subs_on`) persists for the duration of the offline playback session.

### Seeking

Seeking in offline mode works identically to streaming — the video player re-opens the file at a byte offset corresponding to the seek position. Because local SD card access is much faster than network buffering, offline seeks are near-instant (no transcode startup delay).

The seek offset is calculated from the file's known bitrate and the target timestamp. There is no exact keyframe index, so FFmpeg may need to scan a short distance to find the nearest IDR frame.

---

## CBZ Offline Playback

CBZ offline playback goes through `reader_open_local(path)`:

1. Calls `cbz_open(c, path)` directly — no download step
2. State immediately transitions to `READER_READY`
3. Page navigation, zoom, pan, split-screen, and rotation all work identically to online mode

See [MANGA_READER.md](MANGA_READER.md) for full details on the reader.

---

## Cache and Index

The offline file index is built at startup by `cache_init()`, which scans `sdmc:/3ds/jellyfin-3ds/cache/` for all `.ts`, `.mp3`, `.cbz`, and `.ass` files. This index:

- Powers the `[D]` downloaded badge in browse lists (per-frame `cache_has()` check)
- Is used by `dl_queue_has_video()` to avoid queuing an item that's already downloaded
- Is updated immediately when files are added (`cache_index_add`) or deleted (`cache_remove`)

Stale `.part` files (incomplete downloads from a previous session) are deleted by `cache_init()` at startup.

---

## UI State for Offline Mode

In `ui_state_t`:

```c
bool  now_playing_offline;           // true when playing from SD card
char  now_playing_local_path[192];   // sdmc: path to the .ts file
char  now_playing_sub_path[192];     // sdmc: path to the .ass, or ""
bool  offline_subs_on;               // subtitle toggle state
```

These are set when opening a file from the Downloads Manager and cleared when playback ends.

---

## What's Not Yet Offline

- **Audio** (`.mp3` files): downloaded correctly to `ITEMID.mp3` but there's no code path yet to play them without a network URL. The audio player currently always expects an HTTP URL.
- **Playback reporting**: progress is not reported to Jellyfin during offline playback (no network connection assumed). The "Continue Watching" position is not updated.
