# Power Management

> Status: Implemented.

## Overview

Two system-level shutdown controls are available, both using the 3DS `ptmsysm` service:

| Feature | Combo | Scope |
|---------|-------|-------|
| Instant shutdown | ZR + START | Any screen, any time |
| Sleep timer | ZR + SELECT (set from Now Playing) | Timer fires from any screen |

---

## Instant Shutdown (ZR + START)

Pressing ZR + START shuts the 3DS down immediately from any screen — browse, login, settings, reader, or now-playing.

This is handled in `main.c` in the main loop, **before** `ui_update()` is called, so it takes priority over any view-specific input handling:

```c
if ((kheld & KEY_ZR) && (kdown & KEY_START)) {
    ptmSysmInit();
    PTMSYSM_ShutdownAsync(0);
    ptmSysmExit();
    break;
}
```

The loop exits immediately after the shutdown call. Normal cleanup (save config, stop players, free resources) is skipped intentionally — `ShutdownAsync` puts the system into shutdown; the process terminates within a second regardless.

---

## Sleep Timer (ZR + SELECT → popup → countdown)

### Setting the Timer

ZR + SELECT opens the shutdown timer popup **from Now Playing only** (handled inside `VIEW_NOW_PLAYING` in `ui_update()`). It is not accessible from other screens.

The popup lets you set a countdown duration in HH:MM:SS format:

```
┌──────────────────────────────────────┐
│         Shutdown Timer               │
│                                      │
│         00 : [05] : 00               │
│                                      │
│  U/D:value  L/R:field  A:Start B:Cancel │
└──────────────────────────────────────┘
```

- **D-pad Left/Right**: move the cursor between H, M, and S fields (highlighted in accent color)
- **D-pad Up**: increment the selected field (wraps: hours 0–23, minutes/seconds 0–59)
- **D-pad Down**: decrement the selected field (wraps downward)
- **A**: confirm and start the countdown
- **B**: cancel (close popup, timer unchanged)

Default value on first open: 00:05:00. The popup remembers the last-set value for the session.

### Starting the Countdown

Pressing A converts the set duration to milliseconds and stores the deadline:

```c
u64 total_ms = ((u64)h * 3600 + (u64)m * 60 + (u64)s) * 1000;
state->shutdown_timer_deadline = osGetTime() + total_ms;
state->shutdown_timer_active   = true;
```

`osGetTime()` returns milliseconds since a fixed epoch — it is immune to media seeks, playback pauses, and any other app state. The deadline is an absolute timestamp.

If the duration is zero (all fields at 0), A is a no-op and the timer is not activated.

### Countdown Display

While the timer is active, the remaining time is shown in the bottom-right corner of the bottom screen (HH:MM:SS, accent color) whenever the bottom screen is visible. It updates every frame.

```c
u64 rem = (deadline > now) ? deadline - now : 0;
int h = rem / 3600000;
int m = (rem % 3600000) / 60000;
int s = (rem % 60000) / 1000;
```

### Firing the Shutdown

The timer is checked in `main.c` every frame, **before** `ui_update()`, alongside the ZR+START check:

```c
if (s_ui.shutdown_timer_active) {
    u64 now = osGetTime();
    if (now >= s_ui.shutdown_timer_deadline) {
        ptmSysmInit();
        PTMSYSM_ShutdownAsync(0);
        ptmSysmExit();
        break;
    }
}
```

This fires from **any screen** — even if you've navigated away from Now Playing after setting the timer.

---

## ptmsysm Service

Both shutdown paths use the same libctru service:

```c
ptmSysmInit();          // Open handle to ptm:sysm
PTMSYSM_ShutdownAsync(0); // Request system shutdown (0 = power off)
ptmSysmExit();          // Release handle (process ends before this matters)
```

`PTMSYSM_ShutdownAsync` is non-blocking — it signals the system firmware to initiate a clean power-off sequence. The firmware handles writing any pending flash data (RTC, system config) before cutting power.

---

## UI State

In `ui_state_t` (defined in `include/ui/ui.h`):

```c
bool    shutdown_popup_open;      // true while the popup is showing
int     shutdown_popup_sel;       // 0=hours, 1=minutes, 2=seconds
int     shutdown_popup_h;         // hours field value (0–23)
int     shutdown_popup_m;         // minutes field value (0–59), default 5
int     shutdown_popup_s;         // seconds field value (0–59)
bool    shutdown_timer_active;    // true when countdown is running
u64     shutdown_timer_deadline;  // osGetTime() ms value when to fire
```

`shutdown_popup_m` is initialized to 5 at startup so the default is 00:05:00.
