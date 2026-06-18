# Jellyfin 3DS

Native Jellyfin media client for Nintendo 3DS. Stream music and video, read manga, and download files to your SD card — all from your Jellyfin server.

**The first native Jellyfin client for Nintendo 3DS.**

## Features

### Streaming
- **Music** — browse artists, albums, and tracks; MP3/AAC playback with album art; shuffle and repeat modes; ZL/ZR to skip tracks
- **Video** — H.264 hardware decode at up to 24fps on New 3DS (400×240); Old 3DS supports audio only
- **Subtitles** — select any subtitle track; rendered client-side during playback; preference sticks across episodes automatically

### Manga / CBZ Reader
- **Page reader** — download and read CBZ archives directly on device
- **Split-screen mode** — page spans both screens simultaneously (480px combined height) for a taller view
- **Zoom & pan** — zoom in/out with D-pad Up/Down; pan with the circle pad

### Downloads
- **Download to SD** — queue video, audio, and CBZ files to your SD card from browse or while watching
  - **X** — download selected item, or queue the next episode while watching
  - **ZL + X** — download with subtitle burned into the transcoded video
  - **ZL + A** — download with subtitle burned into the encoded stream (alternative method)
  - **ZL + Y** — download just the .ass subtitle file separately
- **Progress tracking** — live speed, estimated file size, progress bar, and ETA in the downloads manager
- **Downloads manager** — browse completed files by series/season, open them directly, or delete; color-coded by type (video / audio / CBZ)

### Library & Navigation
- **Library browsing** — navigate all Jellyfin libraries with pagination and search
- **Auto-play** — next episode or track starts automatically; shuffle mode for music
- **Session persistence** — login once; credentials saved to SD card and restored on next launch

### Power Management
- **Instant shutdown** — ZR+START powers off the 3DS from any screen at any time
- **Sleep timer** — ZR+SELECT (from Now Playing) opens a HH:MM:SS countdown popup; console shuts down automatically when it fires, even if you navigate away

### Settings
- **Themes** — Dark / Black / White / Grey background
- **Bitrates** — configurable audio (64–320 kbps) and video (500K–8000K) bitrates

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

Controls change based on the current screen. **ZR+START powers off the 3DS from any screen. START exits the app from menu screens (not Now Playing or Manga Reader).**

### Login

| Button | Action |
|--------|--------|
| A / R | Connect |
| D-pad Up / Down | Move between fields (URL / Username / Password) |
| Touch | Tap a field to select it; use the on-screen keyboard to type |

### Browse (libraries & item lists)

| Button | Action |
|--------|--------|
| A | Enter folder / play media |
| B | Go back |
| X | Download item to SD card (video, audio, CBZ, or whole album) |
| Y | Select subtitle track for next playback or download |
| ZL + A | Download selected video — subtitles burned in (encoded stream) |
| ZL + X | Download selected video — subtitle burned in (transcoded) |
| ZL + Y | Download just the subtitle (.ass) file for the selected video |
| ZR + START | Power off the 3DS |
| L / R | Previous / next page |
| D-pad Up / Down | Move cursor (hold to scroll continuously) |
| SELECT | Search all libraries (Browse) / Open settings (Libraries) |
| START | Exit app |
| Touch | Tap to select; drag to scroll |

Items that have already been downloaded show a **[D]** badge.

### Now Playing — Audio

| Button | Action |
|--------|--------|
| A | Pause / Resume |
| B | Back to browse (playback continues) |
| Y | Toggle shuffle |
| ZL | Previous track *(New 3DS only)* |
| ZR | Next track *(New 3DS only)* |
| ZR + START | Power off the 3DS |
| ZR + SELECT | Open shutdown sleep timer |
| L / R | Seek −30 / +30 seconds |
| D-pad Up | Exit watch mode |
| D-pad Down | Watch mode (hide bottom screen) |
| SELECT | Cycle repeat: Off → Repeat One → Repeat All |
| START | Stop and exit |

**Sleep timer:** ZR+SELECT opens a popup — D-pad Left/Right to move between H/M/S fields, D-pad Up/Down to change the value, A to start, B to cancel. Default is 00:05:00. Once active the remaining time shows in the bottom-right corner; the console shuts down when it hits zero, even if you've left Now Playing.

### Now Playing — Video (online)

| Button | Action |
|--------|--------|
| A | Pause / Resume |
| B | Back to browse (playback continues) |
| X | Queue next undownloaded episode |
| Y | Cycle subtitle tracks |
| ZL + A | Queue next episode — subtitles burned in (encoded stream) |
| ZL + X | Queue next episode — subtitle burned in (transcoded) |
| ZL + Y | Download subtitle .ass file for the current episode |
| ZR + START | Power off the 3DS |
| ZR + SELECT | Open shutdown sleep timer |
| L / R | Seek −15 / +30 seconds |
| D-pad Up | Exit watch mode |
| D-pad Down | Watch mode (hide controls) |
| START | Stop and exit |

A brief toast ("DL: E04 - Episode Title") appears when a download is queued.

### Now Playing — Video (offline)

| Button | Action |
|--------|--------|
| A | Pause / Resume |
| B | Back to browse (playback continues) |
| X | Queue next undownloaded episode *(requires internet)* |
| ZR + START | Power off the 3DS |
| ZR + SELECT | Open shutdown sleep timer |
| L / R | Seek −15 / +30 seconds |
| D-pad Up | Exit watch mode |
| D-pad Down | Watch mode |
| START | Stop and exit |

Subtitles are burned in at download time; Y (subtitle cycle) is not available for offline files.

### Manga Reader — Normal mode

The top screen shows the page; the bottom screen shows page info and controls.

| Button | Action |
|--------|--------|
| B | Back to browse |
| L / R | Previous / next page |
| D-pad Left / Right | Previous / next page |
| D-pad Up / Down | Zoom in / out |
| Circle pad | Pan (horizontal and vertical) |
| SELECT | Toggle rotation (portrait ↔ landscape) |
| START | Enter split-screen mode |

### Manga Reader — Split-screen mode

The page spans both screens (top = upper half, bottom = lower half).

| Button | Action |
|--------|--------|
| B / START | Return to normal mode |
| L / R | Previous / next page |
| D-pad Left / Right | Previous / next page |
| D-pad Up / Down | Zoom in / out |
| Circle pad Up / Down | Scroll the page vertically |
| Circle pad Left / Right | Pan horizontally |

When entering split mode the zoom is automatically set to fit the full page height across both screens (480px combined).

### Downloads Manager

Access from **Settings → Manage Downloads**.

| Button | Action |
|--------|--------|
| A | Open selected file (CBZ → reader, video/audio → player) |
| B | Back to settings |
| X | Delete file / remove from queue / cancel active download |
| Y | Go to Now Playing |
| D-pad Up / Down | Move cursor (hold to scroll) |
| D-pad Left / Right | Scroll long filename sideways (hold) |
| Circle pad Up / Down | Navigate the download queue on the top screen |

Files are grouped by type then by series, season, and episode. Color-coded: video = blue, audio = purple, CBZ = red. The top screen shows the active download with speed, size, and ETA; the queue is listed below it numbered from 1.

### Settings

| Button | Action |
|--------|--------|
| A | Toggle / activate option |
| B | Save and return to libraries |
| Y | Go to Now Playing *(if something is playing)* |
| L / R or D-pad Left / Right | Cycle values (bitrate, theme) |
| D-pad Up / Down | Move cursor |

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
