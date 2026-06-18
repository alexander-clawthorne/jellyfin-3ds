# Jellyfin 3DS

Native Jellyfin media client for Nintendo 3DS. Stream music and video, read manga, and download files to your SD card — all from your Jellyfin server.

**The first native Jellyfin client for Nintendo 3DS.**

## Features

- **Music streaming** — browse artists, albums, tracks; MP3/AAC with album art; ZL/ZR track skip
- **Video streaming** — H.264 hardware decode at 24fps on New 3DS (400×224)
- **Subtitles** — select any subtitle track; burned in server-side; sticky across episodes
- **Manga / CBZ reader** — download and read CBZ archives page by page
- **Split-screen reader** — page spans both screens simultaneously (480px combined height)
- **Zoom and pan** — pinch-zoom equivalent via circle pad in reader
- **Downloads** — queue video (with optional subtitle), audio, and CBZ files to SD card
- **Download next** — press X while watching to queue the next undownloaded episode automatically
- **Download with subtitles** — press ZL+X while watching to queue the next episode with the active subtitle track burned in
- **Download subtitle file** — press ZL+Y to download just the .ass subtitle for the current or selected episode
- **Encoded subtitle download** — press ZL+A to download with subtitles burned into the encoded stream
- **Download progress** — live speed, estimated file size, progress bar, and ETA on the downloads screen
- **Downloads manager** — browse (grouped by series/season), open, and delete downloaded files; color-coded by type
- **Library browsing** — navigate all Jellyfin libraries with pagination and search
- **Auto-play** — next track or episode plays automatically; shuffle for music
- **Session persistence** — login once, credentials saved to SD card
- **Instant shutdown** — ZR+START powers off the 3DS from any screen
- **Sleep timer** — ZR+SELECT opens a shutdown timer popup; set HH:MM:SS and the console powers off automatically
- **Background theme** — Dark / Black / White / Grey selectable in settings

## Requirements

- Nintendo 3DS or 2DS with [Luma3DS](https://github.com/LumaTeam/Luma3DS) CFW
- [Homebrew Launcher](https://github.com/devkitPro/3ds-hbmenu)
- Jellyfin server accessible on your network
- **New 3DS / New 2DS** required for video playback (Old 3DS: audio only)

## Install

1. Download `jellyfin-3ds.3dsx` from [Releases](../../releases)
2. Copy to `sdmc:/3ds/jellyfin-3ds/jellyfin-3ds.3dsx`
3. Launch from Homebrew Launcher

Downloaded files are saved to `sdmc:/3ds/jellyfin-3ds/`.

---

## Controls

Controls change based on the current screen. **START exits the app from every screen except Now Playing and the Manga Reader.**

### Global (any screen)

| Button | Action |
|--------|--------|
| ZR + START | Instantly power off the 3DS |
| ZR + SELECT | Open shutdown sleep timer popup |

The sleep timer popup lets you set hours, minutes, and seconds (default 00:05:00). Use D-pad Left/Right to move between fields, D-pad Up/Down to change the value, A to start the countdown, and B to cancel. While active, the remaining time is shown in the bottom-right corner of the bottom screen.

### Login

| Button | Action |
|--------|--------|
| D-pad Up/Down | Move between fields (URL / Username / Password) |
| A or R | Connect |
| Touch | Tap field to select, use on-screen keyboard |

### Browse (libraries & item lists)

| Button | Action |
|--------|--------|
| D-pad Up/Down (hold) | Move cursor / continuous scroll |
| A | Enter folder / Play media |
| ZL + A | Download selected video with subtitles burned into encoded stream |
| B | Go back |
| X | Download item to SD card (video, audio, CBZ, or whole album) |
| ZL + X | Download selected video with first available subtitle burned in |
| Y | Select subtitle track for next video playback or download |
| ZL + Y | Download just the subtitle (.ass) file for the selected video |
| L / R | Previous / next page |
| SELECT | Search across all libraries (Browse) / Settings (Libraries) |
| Touch | Tap to select, drag to scroll |
| START | Exit app |

Items that have already been downloaded show a **[D]** badge.

### Now Playing — Audio

| Button | Action |
|--------|--------|
| A | Pause / Resume |
| B | Back to browse (keeps playing) |
| START | Stop playback and exit |
| Y | Toggle shuffle |
| SELECT | Cycle repeat (Off → Repeat One → Repeat All) |
| ZL | Previous track (New 3DS only) |
| ZR | Next track (New 3DS only) |
| ZR + START | Power off the 3DS |
| ZR + SELECT | Open shutdown sleep timer |
| L / R | Seek −30 / +30 seconds |
| D-pad Down | Watch mode (hide bottom screen) |
| D-pad Up | Exit watch mode |

### Now Playing — Video (online)

| Button | Action |
|--------|--------|
| A | Pause / Resume |
| B | Back to browse (keeps playing) |
| START | Stop playback and exit |
| Y | Cycle subtitle tracks |
| X | Queue download of next undownloaded episode |
| ZL + X | Queue next episode download with current subtitle burned in |
| ZL + Y | Download subtitle .ass file for the current episode |
| ZL + A | Queue next episode download with subtitles burned into encoded stream |
| ZR + START | Power off the 3DS |
| ZR + SELECT | Open shutdown sleep timer |
| L / R | Seek −15 / +30 seconds |
| D-pad Down | Watch mode (hide controls) |
| D-pad Up | Exit watch mode |

A brief toast notification ("DL: E04 - Episode Title") appears when a download is queued.

### Now Playing — Video (offline / from Downloads)

| Button | Action |
|--------|--------|
| A | Pause / Resume |
| B | Back to browse (keeps playing) |
| START | Stop playback and exit |
| X | Queue download of next undownloaded episode (requires internet) |
| ZR + START | Power off the 3DS |
| ZR + SELECT | Open shutdown sleep timer |
| L / R | Seek −15 / +30 seconds |
| D-pad Down | Watch mode |
| D-pad Up | Exit watch mode |

Subtitles are burned in at download time; Y is not available for offline files.

### Manga Reader — Normal mode

The top screen shows the page; the bottom screen shows page info and controls.

| Button | Action |
|--------|--------|
| L / R or D-pad Left/Right | Previous / next page |
| D-pad Up / Down | Zoom in / out |
| Circle pad | Pan (horizontal and vertical) |
| SELECT | Toggle rotation (portrait ↔ landscape) |
| START | Enter split-screen mode |
| B | Back to browse |

### Manga Reader — Split-screen mode

The page spans both screens (top screen = upper half, bottom screen = lower half).

| Button | Action |
|--------|--------|
| L / R or D-pad Left/Right | Previous / next page |
| D-pad Up / Down | Zoom in / out |
| Circle pad Up/Down | Scroll the page vertically |
| Circle pad Left/Right | Pan horizontally |
| START | Return to normal mode |
| B | Return to normal mode |

When entering split mode the zoom is automatically set to fit the full page height across both screens (480px combined).

### Downloads Manager

Access from **Settings → Manage Downloads**.

| Button | Action |
|--------|--------|
| D-pad Up/Down (hold) | Move cursor / continuous scroll |
| D-pad Left/Right (hold) | Scroll long filename sideways |
| Circle pad Up/Down | Navigate download queue (top screen) |
| A | Open selected file (CBZ → reader, video/audio → player) |
| X | Delete selected file / Remove from queue / Cancel active download |
| Y | Go to Now Playing |
| B | Back to settings |

Files are grouped by type (video, books, audio) then by series, season, and episode number. Each file type is color-coded: video = blue, audio = purple, CBZ = red.

The top screen shows the active download with a progress bar, file size, download speed, and estimated time remaining. The queue below it lists upcoming items numbered from 1.

### Settings

| Button | Action |
|--------|--------|
| D-pad Up/Down | Move cursor |
| D-pad Left/Right or L/R | Cycle values (bitrate, theme) |
| A | Toggle / activate option |
| Y | Go to Now Playing (if something is playing) |
| B | Save and return to libraries |

---

## First Launch

1. Enter your Jellyfin server URL — e.g. `http://192.168.1.100:8096`
2. Enter your username and password
3. Press **R** to connect

Credentials are saved to `sdmc:/3ds/jellyfin-3ds/config.ini` and restored on next launch.

---

## Downloading Content

### Videos

Press **X** on any movie or episode in the browse view, or **X** while watching to queue the next undownloaded episode.

**Subtitle options when downloading:**

| Combo | Where | What it does |
|-------|-------|--------------|
| ZL + X | Browse or Now Playing | Burns the selected/first subtitle into the transcoded video |
| ZL + A | Browse or Now Playing | Burns subtitles using the encoded stream (alternative transcode path) |
| ZL + Y | Browse or Now Playing | Downloads just the raw .ass subtitle file separately |

A `.txt` companion file with the item title is saved alongside each `.ts` video file.

Multiple downloads can be queued. The queue processes one item at a time and continues automatically. View queue status, speed, and progress in **Settings → Manage Downloads**.

### Manga / CBZ
Press **X** on any book item. The CBZ archive downloads to SD and is cached — opening it again later skips the download entirely.

### Playing downloaded files
Open **Settings → Manage Downloads**, navigate to the file, and press **A**. The app picks the correct player automatically:
- `.cbz` → Manga reader
- `.ts` → Video player

---

## Settings Reference

| Setting | Options | Default |
|---------|---------|---------|
| Audio bitrate | 64 / 96 / 128 / 192 / 320 kbps | 128 kbps |
| Video bitrate | 500K – 8000K bps | 1500K |
| Auto-advance | On / Off | On |
| Background theme | Dark / Black / White / Grey | Dark |
| Manage Downloads | — | — |

---

## Building from Source

### Docker (recommended)

```bash
docker run --rm -v "$(pwd):/src/jellyfin-3ds" devkitpro/devkitarm:latest \
    bash -c "cd /src/jellyfin-3ds && make"
```

### Native (devkitPro)

```bash
sudo dkp-pacman -S 3ds-dev 3ds-curl 3ds-mbedtls 3ds-zlib \
    3ds-libmpg123 3ds-libopus 3ds-opusfile 3ds-libvorbisidec 3ds-libogg
make
```

### Deploy via FTP

```bash
./deploy-ftp.sh <3DS_IP> 5000
```

---

## Architecture

```
Network thread → Ring buffer → Decode thread (FFmpeg demux + MVD H.264 + AAC)
    → Frame queue (6 slots) → Convert thread (Morton tile + A/V sync)
    → Double-buffered GPU textures → Main thread (citro2d render)
```

| Component | Role |
|-----------|------|
| FFmpeg 6.x | MPEG-TS demux, AAC decode, H.264 parse |
| MVD | Hardware H.264 decoder (New 3DS only) |
| NDSP | Hardware audio output |
| citro2d / citro3d | GPU-accelerated 2D rendering |
| libcurl + mbedTLS | HTTPS networking |
| cJSON | Jellyfin API JSON parsing |
| stb_image | JPEG/PNG decode (album art, manga pages) |
| zlib | CBZ (ZIP) page decompression |

---

## Known Limitations

- Video playback requires **New 3DS / New 2DS** (Old 3DS: audio only)
- Video download speed is limited by server-side transcoding time, not network speed
- CIA install not supported — use `.3dsx` via Homebrew Launcher
- Some library types may show unexpected content depending on server configuration

## Credits

Built with reference to:
- [ThirdTube](https://github.com/windows-server-2003/ThirdTube) — video architecture
- [Switchfin](https://github.com/dragonflylee/switchfin) — Jellyfin API patterns
- [Video player for 3DS](https://github.com/Core-2-Extreme/Video_player_for_3DS) — MVD decoder usage
