# Architecture

## System Diagram

```
┌──────────────────────────────────────────────────────────────────┐
│                          main.c                                  │
│   init → [ input → update → render → vblank ] loop → cleanup    │
├─────────────┬───────────────┬──────────────┬────────────────────┤
│  UI System  │  Audio Player │  Video Player│  API Client        │
│  citro2d    │               │  (New 3DS)   │  libcurl + cJSON   │
│  touch/dpad │  ┌──────────┐ │  ┌─────────┐ │                    │
│             │  │Net thread│ │  │Net thrd  │ │  Downloads         │
│  Subtitle   │  │curl ─────┤ │  │curl ─────┤ │  Background curl   │
│  Renderer   │  │ ↓ ring   │ │  │ ↓ ring   │ │                    │
│             │  │Dec thread│ │  │FFmpeg TS │ │  Cache             │
│  Manga/CBZ  │  │mpg123    │ │  │demux     │ │  SD card index     │
│  Reader     │  │ ↓ PCM16  │ │  │ ↓ MVD    │ │                    │
│             │  │NDSP DMA  │ │  │H.264 HW  │ │  Config            │
│  Downloads  │  └──────────┘ │  │ ↓ GPU tex│ │  SD card INI       │
│  Manager    │               │  └─────────┘ │                    │
├─────────────┴───────────────┴──────────────┴────────────────────┤
│                  libctru (3DS OS services)                       │
│   gfx · soc · ndsp · httpc · fs · hid · apt · swkbd · ptmsysm  │
├──────────────────────────────────────────────────────────────────┤
│                     3DS Hardware                                 │
│   ARM11 · PICA200 GPU · MVD · DSP · WiFi · Touchscreen          │
└──────────────────────────────────────────────────────────────────┘
```

## Module Map

| Module | Headers | Source | Purpose |
|--------|---------|--------|---------|
| API client | `include/api/jellyfin.h` | `src/api/jellyfin.c` | Auth, browse, stream URLs, playback reporting |
| Audio player | `include/audio/player.h` | `src/audio/player.c` | MP3/AAC streaming, NDSP, pause/seek |
| Video player | `include/video/video_player.h` | `src/video/video_player.c` | TS stream, MVD H.264, ASS subtitles, 3D SBS |
| FFmpeg demux | `include/video/ffmpeg_demux.h` | `src/video/ffmpeg_demux.c` | TS demux, AAC/H.264 packet routing |
| MVD decode | `include/video/mvd_decode.h` | `src/video/mvd_decode.c` | Hardware H.264 decode, frame buffer management |
| UI | `include/ui/ui.h` | `src/ui/ui.c` | All views, input handling, rendering |
| Manga reader | `include/ui/reader.h` | `src/ui/reader.c` | CBZ download, page decode, GPU upload |
| Album art | `include/ui/album_art.h` | `src/ui/album_art.c` | JPEG/PNG fetch and texture cache |
| Downloader | `include/util/downloader.h` | `src/util/downloader.c` | Background download queue, progress tracking |
| Cache | `include/util/cache.h` | `src/util/cache.c` | SD card index, path helpers, offline file registry |
| CBZ parser | `include/util/cbz.h` | `src/util/cbz.c` | ZIP central directory parsing, page decompression |
| Config | `include/util/config.h` | `src/util/config.c` | INI persistence, device ID, settings |
| Log | `include/util/log.h` | `src/util/log.c` | Debug log to SD card |
| fake_pthread | — | `src/video/fake_pthread.c` | POSIX thread shim over libctru primitives |

## Threading Model

The 3DS has limited threading (Old 3DS: 2 ARM cores, New 3DS: 4 ARM cores). DSP runs independently on its own coprocessor.

| Thread | Core | Purpose |
|--------|------|---------|
| Main | 0 | Input polling, UI update, GPU rendering, subtitle render |
| Audio Network | 1 | libcurl HTTP into audio ring buffer |
| Audio Decode | 1 | mpg123 decode from ring buffer → NDSP wave buffers |
| Video Network | 2 | libcurl HTTP into video ring buffer |
| Video Decode | 2 | FFmpeg TS demux → MVD H.264 → frame queue |
| Video Convert | 0/1 | BGR565 → Morton-tiled GPU texture, A/V sync |
| Download | 1 | Background single-file curl download for the queue |
| NDSP | DSP | Audio output, consumes PCM16 buffers via DMA |

Video threads only run when the video player is active. Audio and video players do not run simultaneously in normal use.

## Data Flows

### Audio Playback

```
1. User presses A on audio item
2. ui_update() → jfin_get_audio_stream() → builds MP3 stream URL
3. audio_player_play(url) spawns net_thread + decode_thread
4. net_thread: curl → ring buffer (256KB prefetch before decode starts)
5. decode_thread: ring buffer → mpg123_feed/read → PCM16 → ndspWaveBuf → NDSP
6. Main thread: audio_player_get_status() → progress bar, position display
7. Every ~5s: jfin_report_progress() updates Jellyfin dashboard
```

### Video Playback

```
1. User presses A on movie/episode
2. ui_update() → jfin_get_video_stream() → TS URL + ASS subtitle URL
3. video_player_play(url, subtitle_url) spawns net/decode/convert threads
4. net_thread: curl → ring buffer
5. decode_thread: avformat demux → AVPacket queues (video + audio)
   - Video packets: AVCC→Annex B → MVD → BGR565 frame
   - Audio packets: FFmpeg AAC decode → swr_convert PCM16 → NDSP
6. convert_thread: Morton-tile BGR565 → GPU texture, A/V sync wait
7. Main thread: video_player_render_frame() → citro2d draw on top screen
8. Main thread: video_player_get_subtitles() → draw ASS cues on top screen
```

### CBZ/Manga Read

```
1. User presses A on book item or opens from Downloads
2. reader_open_book() or reader_open_local() — spawns download thread if needed
3. download thread: curl → sdmc:/3ds/jellyfin-3ds/cache/ITEMID.cbz
4. cbz_open() builds sorted page index from ZIP central directory
5. Per page request: cbz_page_data() → zlib inflate → stb_image decode
6. reader_load_page() → Morton-tile → upload to GPU texture
7. reader_draw() → citro2d sprite with zoom/pan transform
```

### File Download

```
1. User presses X / ZL+X / ZL+A / ZL+Y
2. dl_queue_video/book/audio/subtitle_only() — adds to queue
3. dl_process_queue() called each frame — starts next item if idle
4. download_thread: curl → ITEMID.part → rename → ITEMID.ext
5. cache_index_add() registers completion in memory index
6. [D] badge appears next to item in browse list
```

## Memory Budget (New 3DS — 64MB)

### Linear Memory (required for GPU/MVD/DMA — limited to ~30MB)

| Component | Size | Notes |
|-----------|------|-------|
| MVD work buffer | ~10 MB | H.264 Level 3.1, 400×240, 4 ref frames |
| MVD input buffer | 256 KB | Per-NAL unit staging |
| MVD output frames (×2) | 384 KB | 400×240×2 bytes × 2 (double buffer) |
| Video ring buffer | 512 KB | Network prefetch for video |
| NDSP PCM buffers | 128 KB | 4 × 4096 samples × 2ch × 16bit |
| GPU textures (video, art, manga) | ~2 MB | Various C3D_Tex allocations |
| **Total linear** | **~13 MB** | ~17 MB headroom |

### Heap Memory

| Component | Size | Notes |
|-----------|------|-------|
| FFmpeg static libs (code) | ~4 MB | libavformat, libavcodec, libavutil, libswresample |
| FFmpeg internal state | ~2 MB | Codec contexts, packet buffers |
| Application + UI | ~2 MB | String buffers, item lists, stack |
| Audio ring buffer | 512 KB | Network prefetch for audio |
| Subtitle cue table | ~50 KB | Up to 500 parsed ASS cues |
| Download progress | ~50 KB | Queue structs, curl handles |
| **Total heap** | **~9 MB** |  |

**Combined: ~22 MB** on New 3DS. Old 3DS (64MB) is fine for audio-only; video requires New 3DS.

## API Authentication

Every request carries a `MediaBrowser` authorization header:

```
Authorization: MediaBrowser Client="Jellyfin 3DS", Device="Nintendo 3DS",
  DeviceId="<unique>", Version="1.0.1"[, Token="<access-token>"]
```

Login via `POST /Users/AuthenticateByName` returns an `AccessToken` persisted in `config.ini`. The device ID is derived from the 3DS serial number (or a random UUID on first launch) and is stable across sessions — important for Jellyfin's playback session management.

## SD Card Layout

```
sdmc:/3ds/jellyfin-3ds/
├── jellyfin-3ds.3dsx      Application binary
├── config.ini             Server URL, credentials, settings
├── debug.log              Debug output (if enabled)
└── cache/
    ├── ITEMID.ts          Downloaded video (complete)
    ├── ITEMID.ts.part     In-progress video download
    ├── ITEMID.txt         Companion title file for video
    ├── ITEMID.ass         Companion subtitle file
    ├── ITEMID.mp3         Downloaded audio
    ├── ITEMID.cbz         Downloaded manga/CBZ
    └── ...
```

Files with `.part` suffix are incomplete and swept at startup by `cache_init()`. The in-memory index allows O(1) "is this cached?" lookups per browse frame without hitting the FAT32 filesystem.

## Design Decisions

**Why C instead of C++?**
Smaller binary, no RTTI/exceptions/STL overhead, matches libctru's C API, and all dependencies (FFmpeg, libcurl, mpg123, cJSON, zlib) are C libraries.

**Why libcurl instead of libctru httpc?**
Connection pooling, redirect following, chunked transfer, and battle-tested streaming performance (proven by ThirdTube). The httpc service doesn't support keep-alive or streaming cleanly.

**Why MP3 (mpg123) for music, FFmpeg for video audio?**
mpg123 is extremely lightweight and handles music streaming with minimal CPU. Video playback already pulls in FFmpeg for H.264 demux, so AAC decode via FFmpeg adds no extra dependency for that path.

**Why progressive TS over HLS for video?**
No m3u8 parser needed, single HTTP request, reuses the ring buffer pattern from audio, streamable from byte 0. Seeking re-issues the request with `StartTimeTicks`.

**Why client-side ASS subtitle rendering instead of server burn-in?**
Burn-in forces full server transcoding on every seek. Client-side rendering downloads a small .ass file once and renders cues on the GPU in the main thread — zero seek penalty. Server burn-in is only used for downloads (ZL+X / ZL+A) where it's baked once.

**Why osGetTime() for the shutdown timer?**
`osGetTime()` returns milliseconds since a fixed epoch — it's immune to media seek position or playback state. NDSP sample position (used for A/V sync) would drift with seeks.

**Why a separate download queue instead of in-line curl?**
Downloads are long-running (many minutes for a full episode). Running them in the main thread would block input and rendering. A separate background thread with a queue lets the user continue browsing and watching while downloads progress.
