# Contributing to jellyfin-3ds

## Project Organization

We use a simple milestone-based workflow. No heavy methodology — just clear ownership and a shared roadmap.

### Repository Structure

```
docs/
  ROADMAP.md              Milestones and feature status (source of truth)
  CONTRIBUTING.md         This file
  ARCHITECTURE.md         Technical design, module map, data flows
  SUBTITLES.md            Subtitle system (rendering, sticky tracking, download)
  DOWNLOADS.md            Download queue, offline cache, ZL combo variants
  OFFLINE_PLAYBACK.md     Playing cached files from SD card
  MANGA_READER.md         CBZ reader, split-screen, zoom/pan
  VIDEO_STREAMING.md      Video pipeline reference (MVD, FFmpeg, A/V sync)
  STEREOSCOPIC_3D.md      SBS 3D implementation notes
  POWER_MANAGEMENT.md     Shutdown and sleep timer

src/
  main.c                  Entry point, main loop, global input handling
  api/jellyfin.c          Jellyfin REST API client
  audio/player.c          MP3/AAC audio streaming (mpg123 + NDSP)
  video/video_player.c    Video orchestrator (TS stream, MVD, ASS subtitles)
  video/ffmpeg_demux.c    MPEG-TS demux, packet routing
  video/mvd_decode.c      Hardware H.264 decoder
  video/fake_pthread.c    POSIX thread shim for FFmpeg
  ui/ui.c                 All views, input handling, rendering
  ui/reader.c             Manga/CBZ reader and GPU texture upload
  ui/album_art.c          Album art fetcher and texture cache
  util/downloader.c       Background download queue
  util/cache.c            SD card cache index and file paths
  util/cbz.c              ZIP parser for CBZ archives
  util/config.c           INI-style config persistence
  util/log.c              Debug log to SD card
```

### Module Ownership

Each module has a clear API boundary (header in `include/`). Modules communicate through these headers only — no reaching into another module's internals.

| Module | Header | Depends On |
|--------|--------|------------|
| API client | `include/api/jellyfin.h` | libcurl, cJSON |
| Audio player | `include/audio/player.h` | libcurl, mpg123, libctru (NDSP) |
| Video player | `include/video/video_player.h` | FFmpeg, MVD, NDSP, libcurl |
| FFmpeg demux | `include/video/ffmpeg_demux.h` | FFmpeg (libavformat, libavcodec) |
| MVD decode | `include/video/mvd_decode.h` | libctru (mvd service) |
| UI | `include/ui/ui.h` | citro2d, api types, audio/video status |
| Manga reader | `include/ui/reader.h` | citro2d, cbz, stb_image, downloader |
| Downloader | `include/util/downloader.h` | libcurl, cache |
| Cache | `include/util/cache.h` | libctru (filesystem) |
| CBZ parser | `include/util/cbz.h` | zlib, stdio |
| Config | `include/util/config.h` | libctru (filesystem) |
| Main | `src/main.c` | All of the above |

---

## Workflow

1. **Pick a task** from the current milestone in `ROADMAP.md`
2. **Branch** from `main`: `feat/audio-playback`, `fix/login-timeout`, `docs/update-readme`, etc.
3. **Build & test** — real hardware for anything touching networking, audio, or video; Citra for UI layout changes
4. **PR** with a short description of what changed and how to test it
5. **Check off** the task in `ROADMAP.md` when merged

### Commit Format

```
type: brief imperative summary
```

Types: `feat`, `fix`, `docs`, `refactor`, `chore`, `build`

No emojis. No long body unless explaining a non-obvious constraint or workaround.

### Branch Naming

```
feat/<short-description>    New functionality
fix/<short-description>     Bug fix
build/<short-description>   Build system / CI
docs/<short-description>    Documentation only
```

---

## Code Style

- **C11** for all core modules — keeps binary small, matches libctru
- **4-space indent**, no tabs
- `snake_case` for functions and variables; `UPPER_CASE` for constants and macros
- **Symbol prefixes**: `jfin_` (API), `audio_player_` (audio), `video_player_` (video), `ui_` (UI), `dl_` (downloader), `cache_` (cache), `cbz_` (CBZ parser), `config_` (config), `reader_` (manga reader)
- Comments explain **why**, not what — well-named identifiers handle the what
- Keep functions short. If it doesn't fit on a screen, split it.

---

## Build

### Docker (recommended — no local toolchain needed)

```bash
docker run --rm -v "$(pwd):/src/jellyfin-3ds" devkitpro/devkitarm:latest \
    bash -c "cd /src/jellyfin-3ds && make"
```

FFmpeg static libs are bootstrapped automatically on first build via `lib/ffmpeg/build-ffmpeg.sh`.

### Native (devkitPro)

```bash
sudo dkp-pacman -S 3ds-dev 3ds-curl 3ds-mbedtls 3ds-zlib \
    3ds-libmpg123 3ds-libopus 3ds-opusfile 3ds-libvorbisidec 3ds-libogg
make
```

### Deploy to Console

```bash
# WiFi (3dslink)
3dslink jellyfin-3ds.3dsx

# FTP
./deploy-ftp.sh <3DS_IP> 5000
```

---

## Testing Strategy

| Method | Use For |
|--------|---------|
| Citra emulator | UI layout, input handling, browse navigation |
| Real 3DS (WiFi) | Networking, audio/video streaming, subtitle rendering |
| Real 3DS (SD card) | Offline playback, download queue, config persistence |
| 3dslink console | `printf()` debug output appears on the host PC terminal |
| `debug.log` on SD | Available at `sdmc:/3ds/jellyfin-3ds/debug.log` if enabled |

Always test the full golden path on real hardware before merging any change to the network or playback stack.

---

## Adding a New View

1. Add an entry to `ui_view_t` in `include/ui/ui.h`
2. Add input handling in the `switch(state->current_view)` block in `ui_update()` (in `src/ui/ui.c`)
3. Add a render function `ui_render_<name>()` and call it from `ui_render()`
4. Wire up navigation from an existing view (set `state->current_view = VIEW_YOUR_VIEW`)

Views follow the pattern: input block (all key handling, then `break`) → render function (draw to bottom screen, sometimes top screen too).
