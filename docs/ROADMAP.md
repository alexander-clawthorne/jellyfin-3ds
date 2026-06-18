# Roadmap

## Vision

The first native Jellyfin client for Nintendo 3DS — stream, download, read, and control from your handheld.

## Status (as of 2026-06-18)

**Fully working on real hardware.** All core features are complete and production-stable.

---

## Completed Milestones

### M0 — Build Infrastructure
- [x] devkitPro Docker build (`devkitpro/devkitarm:latest`)
- [x] FFmpeg cross-compile with minimal codec set (H.264 + AAC + MPEG-TS demux)
- [x] fake_pthread shim (POSIX → libctru)
- [x] cJSON, mbedTLS, zlib, libcurl, mpg123 vendored
- [x] CI: GitHub Actions release workflow — tag push builds and attaches `.3dsx`

### M1 — Connect & Browse
- [x] Login with username / password (swkbd keyboard)
- [x] Session persistence — credentials saved to SD card, restored on next launch
- [x] Library browsing (Music, Movies, TV, Books — all item types)
- [x] Pagination (L/R)
- [x] Search across all libraries (SELECT in browse)
- [x] Breadcrumb navigation (back stack)
- [x] Touch scrolling with drag momentum

### M2 — Audio Playback
- [x] MP3 streaming via mpg123 + NDSP
- [x] 44.1kHz / 48kHz auto-detection
- [x] Pause, resume, stop
- [x] Seek (L/R: ±30 seconds)
- [x] ZL/ZR track skip (New 3DS)
- [x] Shuffle mode
- [x] Repeat (Off / One / All)
- [x] Auto-advance to next track
- [x] Playback reporting to Jellyfin dashboard

### M3 — Album Art
- [x] Album art fetch and display on now-playing screen
- [x] JPEG/PNG decode via stb_image
- [x] GPU texture cache (avoids re-fetch on track change within same album)
- [x] Fallback: album art → artist art → no image

### M4 — Video Playback
- [x] H.264 hardware decode via MVD (New 3DS only)
- [x] FFmpeg MPEG-TS demux
- [x] AAC audio via FFmpeg + swr_convert → NDSP
- [x] Audio-master A/V synchronization
- [x] Pause, resume, stop
- [x] Seek (L: −15s, R: +30s) — re-requests TS stream from new position
- [x] Stereoscopic 3D SBS (HSBS + FSBS) with 3D slider integration
- [x] Playback reporting to Jellyfin dashboard
- [x] Old 3DS graceful fallback (video options hidden)
- [x] Configurable video bitrate (500K–8000K)

### M5 — Subtitles
- [x] ASS subtitle list fetch from Jellyfin per item
- [x] Client-side ASS rendering (position anchoring, multi-line, color, alignment)
- [x] Subtitle track selection (Y key)
- [x] Subtitle language sticky across episodes (ISO 639-2 preference)
- [x] Subtitle pre-roll: downloads .ass file before playback starts
- [x] Auto-retry on transcode startup delay (up to 3 retries)
- [x] Subtitle toggle while playing (load/clear without restart)
- [x] Offline subtitle support (plays companion .ass from SD card)

### M6 — Manga / CBZ Reader
- [x] CBZ download from Jellyfin `/Items/{id}/Download` endpoint
- [x] ZIP central directory parsing (cbz.c)
- [x] Per-page zlib decompression + stb_image decode
- [x] Morton-tiled GPU texture upload
- [x] Aspect-fit rendering with zoom (D-pad Up/Down) and pan (circle pad)
- [x] Portrait / landscape rotation toggle (SELECT)
- [x] Dual-screen split mode: page spans top + bottom screens (480px combined)
- [x] Cached to SD card — re-opening skips download
- [x] Play directly from Downloads Manager

### M7 — Downloads & Offline Cache
- [x] Background download queue (video, audio, CBZ)
- [x] ZL+X: download video with subtitle burned in (transcoded)
- [x] ZL+A: download video with subtitle burned into encoded stream
- [x] ZL+Y: download subtitle .ass file only
- [x] X: download next undownloaded episode while watching
- [x] Live progress: speed, estimated file size, ETA, progress bar
- [x] Queue display (top screen in Downloads Manager)
- [x] Download-next naming (Series / Season N / E## - Title)
- [x] [D] badge on browse items that are cached
- [x] Downloads Manager: grouped by series/season, color-coded by type
- [x] Open downloaded files directly from manager
- [x] Delete files from manager
- [x] Stale .part file sweep at startup

### M8 — Offline Playback
- [x] Play .ts video files from SD card without network
- [x] Seek in offline mode (local file seeking)
- [x] Companion .ass subtitle auto-load for offline video
- [x] Subtitle toggle in offline mode
- [x] Play .cbz from SD card (reader_open_local)
- [x] Audio playback not yet supported offline (audio files download but need offline audio path)

### M9 — Settings & UX
- [x] Settings screen (audio bitrate, video bitrate, auto-advance, theme, manage downloads)
- [x] Background theme: Dark / Black / White / Grey
- [x] Watch mode (hide bottom screen — D-pad Down / Up)
- [x] Toast notifications (download queued, subtitle loaded)
- [x] Y → Now Playing shortcut from any menu
- [x] QuickConnect flow (API implemented; UI entry point exists)
- [x] Configurable audio bitrate (64–320 kbps)

### M10 — Power Management
- [x] ZR+START: instant system shutdown from any screen
- [x] ZR+SELECT: sleep timer popup (HH:MM:SS, default 00:05:00)
- [x] Timer fires shutdown from any screen once set

---

## Known Issues

| Issue | Severity | Notes |
|-------|----------|-------|
| Audio offline playback not wired up | Low | .mp3 files download fine; no offline audio player path yet |
| QuickConnect has no UI entry | Low | API functions work; needs a login screen button |
| No "Continue Watching" row on home | Low | API (`jfin_get_resume`) exists; not wired into browse view |
| Morton tiling is CPU-bound | Low | Could use DMA for video frame copy; not a bottleneck in practice |
| Old 3DS: no video | By design | MVD hardware is New 3DS only |

---

## Next Milestones

### Near-term
- [ ] Offline audio playback (wire `.mp3` cache files through a local-file player path)
- [ ] QuickConnect login UI button on the login screen
- [ ] "Continue Watching / Listening" row on the libraries screen

### Future
- [ ] Volume control (NDSP output mix)
- [ ] HLS adaptive streaming (better error recovery, segment-level seeking)
- [ ] Custom app icon (.bnr / .smdh branding)
- [ ] Universal-Updater listing
- [ ] Top-and-Bottom (TAB) 3D mode (HTAB SBS variant)
- [ ] Live TV / DVR (out of scope for now)
- [ ] SyncPlay (out of scope for now)

---

## Non-Goals

- Old 3DS video playback (MVD is hardware-only on New 3DS)
- Local media files (Jellyfin server is required)
- CIA format (`.3dsx` via Homebrew Launcher only)
- SyncPlay or multi-device sync
- Live TV / DVR recording
