# Jellyfin 3DS

Native Jellyfin media client for Nintendo 3DS. Stream music and video, read manga, and download files to your SD card — all from your Jellyfin server.

**The first native Jellyfin client for Nintendo 3DS.**

## Features

- **Music streaming** — browse artists, albums, tracks; MP3/AAC with album art
- **Video streaming** — H.264 hardware decode at 24fps on New 3DS (400×224)
- **Subtitles** — select any subtitle track; burned in server-side (no font rendering needed)
- **Manga / CBZ reader** — download and read CBZ archives page by page
- **Split-screen reader** — page spans both screens simultaneously (480px combined height)
- **Zoom and pan** — pinch-zoom equivalent via circle pad in reader
- **Downloads** — queue video (with subtitle track) and CBZ files to SD card
- **Downloads manager** — browse, open, and delete downloaded files
- **Library browsing** — navigate all Jellyfin libraries with pagination and search
- **Auto-play** — next track or episode plays automatically
- **Session persistence** — login once, credentials saved to SD card
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

Controls change based on the current screen.

### Login

| Button | Action |
|--------|--------|
| D-pad Up/Down | Move between fields (URL / Username / Password) |
| A or R | Connect |
| Touch | Tap field to select, use on-screen keyboard |

### Browse (libraries & item lists)

| Button | Action |
|--------|--------|
| D-pad Up/Down | Move cursor |
| A | Enter folder / Play media |
| B | Go back |
| X | Download item to SD card (video or CBZ) |
| Y | Select subtitle track for next playback or download |
| L / R | Previous / next page |
| SELECT | Search across all libraries |
| Touch | Tap to select, drag to scroll |
| START | Exit app |

Items that have already been downloaded show a **[D]** badge.

When you press X to download a video, the currently selected subtitle track is burned in. A download queue allows multiple items to be queued at once.

### Now Playing — Audio

| Button | Action |
|--------|--------|
| A | Pause / Resume |
| X | Stop |
| B | Back to browse |
| L / R | Seek −30 / +30 seconds |
| D-pad Down | Watch mode (hide bottom screen) |
| D-pad Up | Show bottom screen |
| START | Exit app |

### Now Playing — Video

| Button | Action |
|--------|--------|
| A | Pause / Resume |
| X | Stop |
| B | Back to browse |
| L / R | Seek −30 / +30 seconds |
| Y | Toggle subtitles on / off (online only) |
| D-pad Down | Watch mode (hide controls) |
| D-pad Up | Show controls |
| START | Exit app |

**Offline playback** (files opened from Downloads Manager): subtitles are burned in at download time — the Y button is disabled. L/R restarts the file from the beginning (mid-file seeking is not available for downloaded TS files).

### Manga Reader — Normal mode

The top screen shows the page; the bottom screen shows page info and controls.

| Button | Action |
|--------|--------|
| L / R or D-pad Left/Right | Previous / next page |
| SELECT | Toggle rotation (portrait ↔ landscape) |
| Circle pad Up/Down | Zoom in / out |
| Circle pad Left/Right | Pan horizontally |
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
| START | Exit split-screen mode |
| B | Exit split-screen mode (returns to normal) |

When entering split mode the zoom is automatically set to fit the full page height across both screens (480px combined). You can then use D-pad to zoom and the circle pad to scroll and pan.

### Downloads Manager

Access from **Settings → Manage Downloads**.

| Button | Action |
|--------|--------|
| D-pad Up/Down | Move cursor |
| A | Open selected file (CBZ → reader, video → player) |
| X | Delete selected file (and companion .txt) |
| Y | Cancel the currently active download |
| B | Back to settings |

The top screen shows the current download status including file name and progress. Items waiting in the queue are shown with a count.

### Settings

| Button | Action |
|--------|--------|
| D-pad Up/Down | Move cursor |
| D-pad Left/Right or L/R | Cycle values (bitrate, theme) |
| A | Toggle / activate option |
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
Press **X** on any movie or episode in the browse view. If a subtitle track is selected (via **Y**), it is burned into the video server-side — no separate subtitle file is needed. A `.txt` companion file with the item title is saved alongside the `.ts` video file.

Multiple downloads can be queued. The queue processes one item at a time and continues automatically. View queue status in **Settings → Manage Downloads**.

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
