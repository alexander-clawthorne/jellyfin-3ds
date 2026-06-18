# Video Streaming

> Status: **Implemented and working** on New 3DS.
> Last updated: 2026-06-18

## Overview

Video playback streams H.264 video from a Jellyfin server as an MPEG-TS file, demuxes it with FFmpeg, decodes H.264 via the New 3DS MVD hardware decoder, and renders each frame as a citro2d sprite on the top screen. Audio is decoded from the same TS stream via FFmpeg AAC and played through NDSP.

Old 3DS does not have MVD. The video player detects this at init time and returns false from `video_player_init()`, hiding video options in the UI.

---

## Jellyfin Transcoding

### Stream Request

```
GET /Videos/{id}/stream.ts
    ?VideoCodec=h264
    &AudioCodec=aac
    &VideoBitrate={bitrate}        (from config, default 1500000 bps)
    &AudioBitrate=128000
    &MaxWidth=400
    &MaxHeight=240
    &Profile=Baseline
    &Level=31
    &MaxRefFrames=4
    &TranscodingMaxAudioChannels=2
    &MediaSourceId={id}
    &StartTimeTicks={ticks}        (0 for start, seek position for resume)
    &api_key={token}
```

For stereoscopic 3D content (SBS), `MaxWidth=800` is used instead, producing a side-by-side frame the client splits into left/right eye textures.

### Subtitle Download (client-side rendering)

Subtitles are **not** burned into the video stream during streaming. Instead, the .ass file is fetched separately and rendered on the 3DS in the main thread. See [SUBTITLES.md](SUBTITLES.md) for details.

### Key Transcoding Parameters

| Parameter | Value | Reason |
|-----------|-------|--------|
| `Profile=Baseline` | Constrained Baseline | No B-frames or CABAC — simplest for MVD |
| `Level=31` | H.264 Level 3.1 | Supports 400×240 with headroom; without this, Jellyfin may default to Level 4.1 |
| `MaxRefFrames=4` | ≤4 reference frames | Reduces MVD work buffer size |
| `MaxWidth=400` | Top screen width | Server aspect-corrects height automatically |
| `StartTimeTicks=1` (encoded downloads only) | Forces server to transcode | Prevents Jellyfin from returning a direct stream URL that ignores subtitle encode |

---

## Pipeline

### Threads

| Thread | Core | Purpose |
|--------|------|---------|
| Net | 2 | libcurl HTTP streaming into ring buffer |
| Decode | 2 | FFmpeg TS demux; MVD H.264 decode; FFmpeg AAC decode → NDSP |
| Convert | 0/1 | BGR565 → Morton-tiled GPU texture; A/V sync |
| Main | 0 | Render frame, draw subtitle cues |

### Ring Buffer

The network thread writes to a 512KB ring buffer. The decode thread reads from it. This decouples network latency from decode timing and allows the ~30-second initial transcode startup (during which Jellyfin is converting the file) to be buffered before decode begins.

### AVCC → Annex B Conversion

FFmpeg outputs H.264 packets in AVCC format (4-byte big-endian length per NAL unit). MVD requires Annex B (start code `00 00 01`). The convert step runs in the decode thread before each `mvdstdProcessVideoFrame` call.

### A/V Synchronization

Audio-master sync: the audio clock is derived from `ndspChnGetSamplePos()` + the base PTS of the current wave buffer. The convert thread compares each video frame's PTS against the audio clock and:

- Sleeps if the video is ahead of audio by more than 3ms
- Drops the frame if audio has run more than 50ms ahead (video is too far behind)

This matches the ThirdTube approach and keeps lip sync tight across the full duration.

### Seeking

Seek re-issues the stream request with `StartTimeTicks` set to the new position (in Jellyfin 100ns ticks). The player drains and resets all queues, flushes FFmpeg codec contexts, reloads SPS/PPS, and resumes. Because Jellyfin transcodes from a keyframe, the first decodable frame appears quickly.

---

## MVD Hardware Decoder

### Initialization

```c
// Work buffer: ~10MB for Level 3.1 at 400×240 with 4 ref frames
mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_BGR565,
           work_buf_size, NULL);

MVDSTD_Config config;
mvdstdGenerateDefaultConfig(&config, width, height, width, height,
                            NULL, out_buf, out_buf);
```

### Feed Order

1. Send SPS NAL with Annex B prefix — must be first
2. Send PPS NAL — must be second
3. For each video packet from FFmpeg: convert AVCC→Annex B, flush cache, call `mvdstdProcessVideoFrame`
4. Feed the first real NAL twice (hardware pipeline priming)
5. After each NAL: call `mvdstdRenderVideoFrame(NULL, true)` — blocks until frame is ready in output buffer

### Output Format

MVD outputs BGR565 pixels in linear memory. The convert thread Morton-tiles (8×8 block swizzle) this into a `C3D_Tex` for the GPU. Two textures double-buffer: the decode thread writes to `tex[write_idx]` while the main thread renders from `tex[!write_idx]`.

### Known Quirks

- **1-frame pipeline delay**: feeding frame N yields frame N-1. A sentinel byte written at a corner of the output buffer before each call detects when MVD produced output.
- **Width/height must be 16-pixel-aligned**.
- **All buffers must be in linear memory** (`linearAlloc`) — heap memory is not accessible by MVD DMA.
- **Cache flush required** (`GSPGPU_FlushDataCache`) before every `ProcessVideoFrame` call.

---

## FFmpeg Configuration

FFmpeg is cross-compiled for ARM11 (armv6k) with a minimal codec set. See `lib/ffmpeg/build-ffmpeg.sh` for the full configure command.

| Library | Purpose |
|---------|---------|
| `libavformat` | MPEG-TS demux |
| `libavcodec` | H.264 parse (bitstream → NAL units); AAC decode |
| `libavutil` | Common utilities |
| `libswresample` | PCM format conversion (float → s16 for NDSP) |

Decoders enabled: `aac`, `h264`. Demuxers: `mpegts`, `mov`.

The `fake_pthread.c` shim maps POSIX thread primitives used internally by FFmpeg to libctru's `LightLock`, `CondVar`, and `threadCreate`.

---

## Stereoscopic 3D (SBS)

When the Jellyfin item has `Video3DFormat = "HalfSideBySide"` or `"FullSideBySide"`, the player requests `MaxWidth=800` from the server. The resulting frame is 800×N with the left eye in the left half and the right eye in the right half.

The convert thread splits the frame: left columns → `tex_left`, right columns → `tex_right`. The main thread renders to `GFX_LEFT` and `GFX_RIGHT` render targets with `gfxSet3D(true)`. When the 3D slider is at 0, only the left eye is rendered (same cost as 2D).

See [STEREOSCOPIC_3D.md](STEREOSCOPIC_3D.md) for full implementation details.

---

## Memory Budget (New 3DS)

| Component | Size |
|-----------|------|
| MVD work buffer | ~10 MB |
| MVD input buffer (NAL staging) | 256 KB |
| MVD output frames (×2, double-buffer) | 384 KB |
| Video ring buffer | 512 KB |
| FFmpeg internal state | ~2 MB |
| GPU frame textures (×2 or ×4 for 3D) | ~512 KB |
| Subtitle cue table | ~50 KB |
| **Total** | **~13.5 MB** |

This fits within the ~30 MB of linear memory available on New 3DS.

---

## Error States

| State | Cause | UI Display |
|-------|-------|------------|
| `VIDEO_LOADING` | Network prefetch / server transcode startup | "BUFFERING" + progress % on bottom screen |
| `VIDEO_ERROR` | Demux failed, MVD init failed, network error | Error message on bottom screen |
| `VIDEO_STOPPED` | User stopped or auto-advance | Returns to browse |

Transcode startup typically takes 20–30 seconds for the first play; seeks are faster because Jellyfin transcodes from a keyframe at the target position.
