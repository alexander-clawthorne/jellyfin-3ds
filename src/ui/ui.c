/**
 * ui.c - Dual-screen UI implementation
 *
 * Uses citro2d for GPU-accelerated 2D rendering.
 * Top screen: now-playing / branding
 * Bottom screen: touch-driven list navigation
 *
 * MVP: text-based rendering. Album art loading deferred to phase 2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <3ds.h>
#include <citro2d.h>

#include "ui/ui.h"
#include "api/jellyfin.h"
#include "audio/player.h"
#include "video/video_player.h"
#include "ui/album_art.h"
#include "ui/reader.h"
#include "util/config.h"
#include "util/downloader.h"
#include "util/cache.h"
#include "util/log.h"

extern jfin_config_t g_config;

/* ── Render targets ────────────────────────────────────────────────── */

static C3D_RenderTarget *s_top       = NULL;
static C3D_RenderTarget *s_top_right = NULL;  /* right eye for stereoscopic 3D */
static C3D_RenderTarget *s_bottom    = NULL;
static C2D_TextBuf       s_text_buf = NULL;
static C2D_Font          s_font = NULL;

/* ── Helpers ───────────────────────────────────────────────────────── */

static u32 rgba(u32 hex)
{
    /* Convert 0xRRGGBBAA to citro2d's ABGR format */
    u8 r = (hex >> 24) & 0xFF;
    u8 g = (hex >> 16) & 0xFF;
    u8 b = (hex >> 8)  & 0xFF;
    u8 a = hex & 0xFF;
    return C2D_Color32(r, g, b, a);
}

static u32 bg_color(void)
{
    switch (g_config.bg_theme) {
        case 1:  return rgba(0x000000FF);
        case 2:  return rgba(0xEEEEEEFF);
        case 3:  return rgba(0x505050FF);
        default: return rgba(COLOR_BG_DARK);
    }
}

/* ── Download manager (file list on SD card) ───────────────────────── */

#define MAX_DL_ENTRIES 64

typedef struct {
    char   item_id[80];  /* item id for cache_remove() */
    char   name[128];    /* display name */
    char   path[192];    /* full path for deletion */
    size_t sz;           /* bytes */
    bool   is_video;
    int    dl_type;      /* 0=video, 1=cbz, 2=audio */
    char   series[64];   /* series/album name for sorting */
    int    season_num;   /* season or volume number */
    int    ep_num;       /* episode or chapter number */
} dl_entry_t;

static dl_entry_t s_dl_entries[MAX_DL_ENTRIES];
static int        s_dl_count = 0;

/* Parse "Series / Season N / E## - Title" or "Series / Volume N / Chapter M" into
 * sort fields. Handles any " / "-delimited breadcrumb. */
static void dl_parse_metadata(dl_entry_t *e)
{
    e->series[0] = '\0';
    e->season_num = 0;
    e->ep_num = 0;

    char tmp[128];
    strncpy(tmp, e->name, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = '\0';

    char *p1 = strstr(tmp, " / ");
    if (!p1) {
        strncpy(e->series, tmp, sizeof(e->series)-1);
        e->series[sizeof(e->series)-1] = '\0';
        return;
    }
    *p1 = '\0';
    strncpy(e->series, tmp, sizeof(e->series)-1);
    e->series[sizeof(e->series)-1] = '\0';

    char *p2 = strstr(p1 + 3, " / ");
    if (p2) {
        /* Season/Volume part */
        char spart[32];
        int slen = (int)(p2 - (p1 + 3));
        if (slen > 31) slen = 31;
        strncpy(spart, p1 + 3, slen);
        spart[slen] = '\0';
        const char *snum = spart;
        while (*snum && (*snum < '0' || *snum > '9')) snum++;
        e->season_num = (*snum) ? atoi(snum) : 0;
        /* Episode/Chapter part */
        const char *ep = p2 + 3;
        if (*ep == 'E' || *ep == 'e') ep++;
        else { while (*ep && (*ep < '0' || *ep > '9')) ep++; }
        e->ep_num = (*ep) ? atoi(ep) : 0;
    }
}

static int dl_entry_cmp(const void *a, const void *b)
{
    const dl_entry_t *ea = (const dl_entry_t *)a;
    const dl_entry_t *eb = (const dl_entry_t *)b;
    if (ea->dl_type != eb->dl_type) return ea->dl_type - eb->dl_type;
    int sc = strcmp(ea->series, eb->series);
    if (sc != 0) return sc;
    if (ea->season_num != eb->season_num) return ea->season_num - eb->season_num;
    return ea->ep_num - eb->ep_num;
}

static void dl_manager_scan(void)
{
    s_dl_count = 0;
    DIR *d = opendir(CACHE_DIR);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && s_dl_count < MAX_DL_ENTRIES) {
        const char *n = de->d_name;
        int nlen = (int)strlen(n);

        /* Skip hidden, .part files, and .txt companion files */
        if (n[0] == '.') continue;
        if (nlen > 5 && strcmp(n + nlen - 5, ".part") == 0) continue;
        if (nlen > 4 && strcmp(n + nlen - 4, ".txt")  == 0) continue;

        /* Detect type by extension: ITEMID.ts / .cbz / .mp3 */
        bool is_vid   = (nlen > 3 && strcmp(n + nlen - 3, ".ts")  == 0);
        bool is_cbz   = (nlen > 4 && strcmp(n + nlen - 4, ".cbz") == 0);
        bool is_audio = (nlen > 4 && strcmp(n + nlen - 4, ".mp3") == 0);
        if (!is_vid && !is_cbz && !is_audio) continue;

        dl_entry_t *e = &s_dl_entries[s_dl_count++];
        e->is_video = is_vid;
        e->dl_type  = is_vid ? 0 : (is_cbz ? 1 : 2);

        snprintf(e->path, sizeof(e->path), "%s/%s", CACHE_DIR, n);

        /* Strip extension to get item_id */
        int ext_len = is_vid ? 3 : 4;
        int id_len  = nlen - ext_len;
        if (id_len > 0 && id_len < (int)sizeof(e->item_id)) {
            memcpy(e->item_id, n, (size_t)id_len);
            e->item_id[id_len] = '\0';
        } else {
            e->item_id[0] = '\0';
        }

        struct stat st;
        e->sz = (stat(e->path, &st) == 0) ? (size_t)st.st_size : 0;

        /* Try companion .txt: CACHE_DIR/ITEMID.txt */
        char txt[208];
        snprintf(txt, sizeof(txt), "%s/%s.txt", CACHE_DIR, e->item_id);
        FILE *f = fopen(txt, "r");
        if (f) {
            if (fgets(e->name, sizeof(e->name), f)) {
                int l = (int)strlen(e->name);
                while (l > 0 && (e->name[l-1] == '\n' || e->name[l-1] == '\r'))
                    e->name[--l] = '\0';
            }
            fclose(f);
        } else {
            snprintf(e->name, sizeof(e->name), "%s", e->item_id);
        }

        dl_parse_metadata(e);
    }
    closedir(d);

    if (s_dl_count > 1)
        qsort(s_dl_entries, s_dl_count, sizeof(dl_entry_t), dl_entry_cmp);
}

static void dl_manager_delete(int idx)
{
    if (idx < 0 || idx >= s_dl_count) return;
    dl_entry_t *e = &s_dl_entries[idx];
    /* Remove file via cache (also removes from in-memory index) */
    const char *ext = e->is_video ? "ts" : (e->dl_type == 1 ? "cbz" : "mp3");
    if (e->item_id[0])
        cache_remove(e->item_id, ext);
    else
        remove(e->path);
    /* Remove companion .txt */
    char txt[208];
    snprintf(txt, sizeof(txt), "%s/%s.txt", CACHE_DIR, e->item_id);
    remove(txt);
}

static int dl_manager_count(void) { return s_dl_count; }

static const dl_entry_t *dl_manager_get(int idx)
{
    if (idx < 0 || idx >= s_dl_count) return NULL;
    return &s_dl_entries[idx];
}

static void draw_text(float x, float y, float size, u32 color, const char *text)
{
    C2D_Text c2d_text;
    C2D_TextParse(&c2d_text, s_text_buf, text);
    C2D_TextOptimize(&c2d_text);
    C2D_DrawText(&c2d_text, C2D_WithColor, x, y, 0.5f, size, size, color);
}

static void draw_rect(float x, float y, float w, float h, u32 color)
{
    C2D_DrawRectSolid(x, y, 0.0f, w, h, color);
}

#define SUB_SCALE 0.55f
#define SUB_LINE_H 14.0f

static void draw_subtitles_top(void)
{
    vp_subtitle_t subs[4];
    int sub_count = video_player_get_subtitles(subs, 4);
    if (!sub_count) return;

    u32 outline = C2D_Color32(0, 0, 0, 200);

    for (int si = 0; si < sub_count; si++) {
        const vp_subtitle_t *s = &subs[si];
        u32 fg = s->color ? s->color : C2D_Color32(255, 255, 255, 255);

        /* Count lines to compute total text block height */
        int nlines = 1;
        for (const char *q = s->text; *q; q++) if (*q == '\n') nlines++;

        /* Vertical placement based on ASS alignment row (1-3=bottom, 4-6=mid, 7-9=top) */
        int row = (s->alignment - 1) / 3; /* 0=bottom, 1=mid, 2=top */

        /* Anchor point — default depends on row when no explicit \pos */
        float ax = (s->screen_x >= 0.0f) ? s->screen_x : 200.0f;
        float ay;
        if (s->screen_y >= 0.0f) ay = s->screen_y;
        else if (row == 2)        ay = 8.0f;    /* top */
        else if (row == 1)        ay = 120.0f;  /* middle */
        else                      ay = 226.0f;  /* bottom */

        float block_h = nlines * SUB_LINE_H;
        float line_y;
        if (row == 0)      line_y = ay - block_h;          /* bottom-anchored */
        else if (row == 1) line_y = ay - block_h / 2.0f;   /* middle-anchored */
        else               line_y = ay;                     /* top-anchored */

        /* Split text on '\n' and render each line */
        const char *lp = s->text;
        while (lp && *lp) {
            const char *nl = strchr(lp, '\n');
            int ll = nl ? (int)(nl - lp) : (int)strlen(lp);

            char lbuf[260];
            if (ll >= (int)sizeof(lbuf)) ll = sizeof(lbuf) - 1;
            memcpy(lbuf, lp, ll);
            lbuf[ll] = '\0';

            C2D_Text ct;
            C2D_TextParse(&ct, s_text_buf, lbuf);
            C2D_TextOptimize(&ct);

            float tw = 0, th = 0;
            C2D_TextGetDimensions(&ct, SUB_SCALE, SUB_SCALE, &tw, &th);
            (void)th;

            /* Horizontal placement */
            int col = (s->alignment - 1) % 3; /* 0=left, 1=center, 2=right */
            float lx;
            if (col == 0)      lx = ax;
            else if (col == 1) lx = ax - tw / 2.0f;
            else               lx = ax - tw;

            /* Clamp to screen */
            if (lx < 2.0f) lx = 2.0f;
            if (lx + tw > 398.0f) lx = 398.0f - tw;

            /* Outline then fill */
            C2D_DrawText(&ct, C2D_WithColor, lx-1, line_y-1, 0.5f, SUB_SCALE, SUB_SCALE, outline);
            C2D_DrawText(&ct, C2D_WithColor, lx+1, line_y-1, 0.5f, SUB_SCALE, SUB_SCALE, outline);
            C2D_DrawText(&ct, C2D_WithColor, lx-1, line_y+1, 0.5f, SUB_SCALE, SUB_SCALE, outline);
            C2D_DrawText(&ct, C2D_WithColor, lx+1, line_y+1, 0.5f, SUB_SCALE, SUB_SCALE, outline);
            C2D_DrawText(&ct, C2D_WithColor, lx,   line_y,   0.5f, SUB_SCALE, SUB_SCALE, fg);

            line_y += SUB_LINE_H;
            lp = nl ? nl + 1 : NULL;
        }
    }
}

static void format_ticks(int64_t ticks, char *out, int out_len)
{
    int total_sec = (int)(ticks / 10000000LL);
    int min = total_sec / 60;
    int sec = total_sec % 60;
    snprintf(out, out_len, "%d:%02d", min, sec);
}

/* Map Jellyfin's 3D metadata onto the video player's render mode.
 * TAB formats aren't renderable in 3D yet — they play flat (2D). */
static vp_3d_mode_t item_3d_mode(const jfin_item_t *item)
{
    switch (item->video_3d_format) {
    case JFIN_3D_HSBS: return VP_3D_HSBS;
    case JFIN_3D_FSBS: return VP_3D_FSBS;
    default:           return VP_3D_NONE;
    }
}

/* ── Selection style helper ────────────────────────────────────────── */

static void draw_list_item_bg(float y, float w, float h, bool selected)
{
    u32 bg = selected ? rgba(COLOR_HIGHLIGHT) : rgba(COLOR_BG_CARD);
    draw_rect(5, y, w, h, bg);
    if (selected)
        draw_rect(5, y, 3, h, rgba(COLOR_PRIMARY));  /* left accent bar */
}

/* ── Settings items ───────────────────────────────────────────────── */

enum {
    SET_DOWNLOADS,              /* action: open downloads manager — pinned first */
    SET_AUDIO_BITRATE,
    SET_VIDEO_BITRATE,
    SET_AUTO_ADVANCE,
    SET_BG_THEME,
    SET_SEPARATOR_ACCOUNT,      /* non-selectable divider */
    SET_SERVER,                 /* display-only */
    SET_USERNAME,               /* display-only */
    SET_LOGOUT,                 /* action */
    SET_SEPARATOR_ABOUT,        /* non-selectable divider */
    SET_VERSION,                /* display-only */
    SET_DEVICE_ID,              /* display-only */
    SET_COUNT
};

static const int audio_rates[] = {64, 128, 192, 256};
static const int video_rates[] = {256, 472, 768};
#define AUDIO_RATES_COUNT (int)(sizeof(audio_rates) / sizeof(audio_rates[0]))
#define VIDEO_RATES_COUNT (int)(sizeof(video_rates) / sizeof(video_rates[0]))

static bool settings_is_separator(int idx)
{
    return idx == SET_SEPARATOR_ACCOUNT ||
           idx == SET_SEPARATOR_ABOUT;
}

static int settings_next_selectable(int from, int dir)
{
    int n = from + dir;
    while (n >= 0 && n < SET_COUNT && settings_is_separator(n))
        n += dir;
    if (n < 0 || n >= SET_COUNT) return from;
    return n;
}

/* ── Subtitle helpers ──────────────────────────────────────────────── */

static void apply_sticky_subtitle(ui_state_t *state, const jfin_session_t *session,
                                  const char *item_id)
{
    state->subtitle_stream_index = -1;
    state->subtitle_list_loaded = false;
    if (!state->subtitle_sticky || state->subtitle_lang_pref[0] == '\0') return;
    if (!jfin_get_subtitle_streams(session, item_id, &state->subtitle_list)) return;
    state->subtitle_list_loaded = true;
    for (int i = 0; i < state->subtitle_list.count; i++) {
        if (strcmp(state->subtitle_list.subs[i].language,
                   state->subtitle_lang_pref) == 0) {
            state->subtitle_stream_index = state->subtitle_list.subs[i].index;
            return;
        }
    }
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

bool ui_init(void)
{
    s_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    s_top_right = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
    s_bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    if (!s_top || !s_top_right || !s_bottom) return false;

    s_text_buf = C2D_TextBufNew(4096);
    if (!s_text_buf) return false;

    /* Use system font */
    s_font = NULL; /* NULL = default system font */

    srand((unsigned int)svcGetSystemTick());
    return true;
}

void ui_cleanup(void)
{
    dl_cleanup();
    if (s_text_buf) {
        C2D_TextBufDelete(s_text_buf);
        s_text_buf = NULL;
    }
}

/* ── Input Handling ────────────────────────────────────────────────── */

void ui_update(ui_state_t *state, const jfin_session_t *session,
               u32 kdown, u32 kheld, touchPosition touch)
{
    /* Advance download queue every frame regardless of active view */
    dl_process_queue();

    switch (state->current_view) {
    case VIEW_LOGIN:
        /* D-pad up/down to select field */
        if (kdown & KEY_DUP) {
            state->login_field = (state->login_field + 2) % 3;
        }
        if (kdown & KEY_DDOWN) {
            state->login_field = (state->login_field + 1) % 3;
        }
        /* A to activate swkbd for the selected field */
        if (kdown & KEY_A) {
            SwkbdState swkbd;
            char buf[JFIN_MAX_URL] = {0};

            SwkbdType type = (state->login_field == 2)
                ? SWKBD_TYPE_WESTERN : SWKBD_TYPE_WESTERN;
            swkbdInit(&swkbd, type, 2, 255);

            switch (state->login_field) {
            case 0:
                swkbdSetHintText(&swkbd, "Server URL (e.g. http://192.168.1.100:8096)");
                snprintf(buf, sizeof(buf), "%s", state->server_url);
                break;
            case 1:
                swkbdSetHintText(&swkbd, "Username");
                snprintf(buf, sizeof(buf), "%s", state->username);
                break;
            case 2:
                swkbdSetHintText(&swkbd, "Password");
                swkbdSetPasswordMode(&swkbd, SWKBD_PASSWORD_HIDE_DELAY);
                break;
            }

            swkbdSetInitialText(&swkbd, buf);
            SwkbdButton button = swkbdInputText(&swkbd, buf, sizeof(buf));

            if (button == SWKBD_BUTTON_CONFIRM) {
                switch (state->login_field) {
                case 0: snprintf(state->server_url, sizeof(state->server_url), "%s", buf); break;
                case 1: snprintf(state->username, sizeof(state->username), "%s", buf); break;
                case 2: snprintf(state->password, sizeof(state->password), "%s", buf); break;
                }
            }
        }
        /* R: attempt login */
        if (kdown & KEY_R) {
            /* Show connecting indicator before blocking login call */
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            C2D_TextBufClear(s_text_buf);
            C2D_TargetClear(s_bottom, bg_color());
            C2D_SceneBegin(s_bottom);
            draw_text(100, 110, 0.6f, rgba(COLOR_PRIMARY), "Connecting...");
            C3D_FrameEnd(0);

            jfin_session_t *s = (jfin_session_t *)session; /* cast away const for login */
            if (jfin_login(s, state->server_url, state->username, state->password,
                           g_config.device_id)) {
                /* Save credentials immediately so they persist even on crash */
                config_save_session(&g_config, s->server_url,
                                    s->access_token, s->user_id,
                                    state->username);
                /* Token obtained — no reason to keep the password in RAM */
                memset(state->password, 0, sizeof(state->password));
                state->current_view = VIEW_LIBRARIES;
                jfin_get_views(session, &state->items);
                state->selected_index = 0;
                state->scroll_offset = 0;
            }
        }
        /* SELECT: open settings without logging in (offline/downloads access) */
        if (kdown & KEY_SELECT) {
            state->previous_view = VIEW_LOGIN;
            state->settings_index = 0;
            state->settings_scroll = 0;
            state->current_view = VIEW_SETTINGS;
        }
        break;

    case VIEW_LIBRARIES:
    case VIEW_BROWSE:
        /* D-pad navigation with hold-to-repeat: ~270ms initial delay,
         * then one step every other frame (~15 items/s at 30fps).
         * Statics instead of ui_state_t fields: keeps this out of the
         * header while the wip UI branch is active; ui_update is
         * main-thread only so there is no reentrancy concern. */
        {
            static int dpad_repeat_frames = 0;
            static int dpad_last_view = -1;

            /* Entering this view with the D-pad already held must not
             * inherit a matured timer from the previous view */
            if (dpad_last_view != (int)state->current_view) {
                dpad_repeat_frames = 0;
                dpad_last_view = (int)state->current_view;
            }

            bool nav_up = (kdown & KEY_DUP) != 0;
            bool nav_down = (kdown & KEY_DDOWN) != 0;

            if (nav_up || nav_down) {
                dpad_repeat_frames = 0; /* fresh press resets the timer */
            } else if (kheld & (KEY_DUP | KEY_DDOWN)) {
                dpad_repeat_frames++;
                if (dpad_repeat_frames >= 8 && (dpad_repeat_frames % 2) == 0) {
                    nav_up = (kheld & KEY_DUP) != 0;
                    nav_down = (kheld & KEY_DDOWN) != 0;
                }
            } else {
                dpad_repeat_frames = 0;
            }

            if (nav_up) {
                if (state->selected_index > 0) {
                    state->selected_index--;
                    if (state->selected_index < state->scroll_offset)
                        state->scroll_offset = state->selected_index;
                }
            }
            if (nav_down) {
                if (state->selected_index < state->items.count - 1) {
                    state->selected_index++;
                    if (state->selected_index >= state->scroll_offset + UI_MAX_VISIBLE_ITEMS)
                        state->scroll_offset = state->selected_index - UI_MAX_VISIBLE_ITEMS + 1;
                }
            }
        }
        /* Touch: drag to scroll, tap to select+activate */
        {
            /* List items start at y=25 (browse) or y=30 (libraries) on screen */
            int list_top = (state->current_view == VIEW_LIBRARIES) ? 30 : 25;

            if (kdown & KEY_TOUCH) {
                state->touch_held = true;
                state->touch_start_y = touch.py;
                state->scroll_velocity = 0;
            }
            if ((kheld & KEY_TOUCH) && state->touch_held) {
                int dy = state->touch_start_y - touch.py;
                if (dy > UI_LIST_ITEM_HEIGHT / 2 || dy < -UI_LIST_ITEM_HEIGHT / 2) {
                    /* Dragging — scroll the list */
                    int scroll_items = dy / UI_LIST_ITEM_HEIGHT;
                    if (scroll_items != 0) {
                        state->scroll_offset += scroll_items;
                        state->touch_start_y = touch.py;
                        state->scroll_velocity = scroll_items;
                        /* Clamp */
                        int max_scroll = state->items.count - UI_MAX_VISIBLE_ITEMS;
                        if (max_scroll < 0) max_scroll = 0;
                        if (state->scroll_offset > max_scroll) state->scroll_offset = max_scroll;
                        if (state->scroll_offset < 0) state->scroll_offset = 0;
                        if (state->selected_index < state->scroll_offset)
                            state->selected_index = state->scroll_offset;
                        if (state->selected_index >= state->scroll_offset + UI_MAX_VISIBLE_ITEMS)
                            state->selected_index = state->scroll_offset + UI_MAX_VISIBLE_ITEMS - 1;
                    }
                }
            }
            if (!(kheld & KEY_TOUCH) && state->touch_held) {
                state->touch_held = false;
                if (state->scroll_velocity == 0) {
                    /* Tap — select AND activate (same as D-pad + A) */
                    int tapped = state->scroll_offset + ((state->touch_start_y - list_top) / UI_LIST_ITEM_HEIGHT);
                    if (tapped >= 0 && tapped < state->items.count) {
                        state->selected_index = tapped;
                        /* Simulate A press by setting kdown */
                        kdown |= KEY_A;
                    }
                }
            }
            /* Momentum scrolling */
            if (!state->touch_held && state->scroll_velocity != 0) {
                state->scroll_offset += state->scroll_velocity;
                if (state->scroll_velocity > 0) state->scroll_velocity--;
                else state->scroll_velocity++;
                int max_scroll = state->items.count - UI_MAX_VISIBLE_ITEMS;
                if (max_scroll < 0) max_scroll = 0;
                if (state->scroll_offset > max_scroll) state->scroll_offset = max_scroll;
                if (state->scroll_offset < 0) { state->scroll_offset = 0; state->scroll_velocity = 0; }
            }
        }
        /* A to enter / play (skip if ZL held — ZL+A is encoded-sub download) */
        if ((kdown & KEY_A) && !(kheld & KEY_ZL)) {
            if (state->selected_index < state->items.count) {
                jfin_item_t *item = &state->items.items[state->selected_index];
                bool is_playable = (item->type == JFIN_ITEM_AUDIO ||
                                    item->type == JFIN_ITEM_MOVIE ||
                                    item->type == JFIN_ITEM_EPISODE);
                bool is_container = (item->type == JFIN_ITEM_FOLDER ||
                                     item->type == JFIN_ITEM_MUSIC_ALBUM ||
                                     item->type == JFIN_ITEM_MUSIC_ARTIST ||
                                     item->type == JFIN_ITEM_SERIES ||
                                     item->type == JFIN_ITEM_SEASON);
                bool is_video = (item->type == JFIN_ITEM_MOVIE ||
                                 item->type == JFIN_ITEM_EPISODE);
                if (is_playable) {
                    if (is_video && video_player_is_supported()) {
                        /* Video playback on New 3DS */
                        vp_3d_mode_t mode_3d = item_3d_mode(item);
                        audio_player_stop();
                        apply_sticky_subtitle(state, session, item->id);
                        jfin_stream_t stream;
                        state->video_retry_count = 0;
                        state->video_retry_timer = 0;
                        if (jfin_get_video_stream(session, item->id, 0,
                                                  mode_3d != VP_3D_NONE,
                                                  state->subtitle_stream_index,
                                                  &stream) &&
                            video_player_play(stream.url, stream.subtitle_url, item->runtime_ticks, 0, mode_3d)) {
                            state->now_playing_offline = false;
                            state->now_playing_local_path[0] = '\0';
                            state->now_playing_sub_path[0] = '\0';
                            state->now_playing = *item;
                            state->has_now_playing = true;
                            state->playing_index = state->selected_index;
                            state->auto_stopped = false;
                            state->previous_view = state->current_view;
                            state->current_view = VIEW_NOW_PLAYING;
                            jfin_report_start(session, item->id);
                                album_art_load(session, item);
                        } else {
                            /* Video failed — fall back to audio */
                            if (jfin_get_audio_stream(session, item->id, 0, &stream)) {
                                audio_player_play(stream.url, item->runtime_ticks, 0);
                                state->now_playing = *item;
                                state->has_now_playing = true;
                                state->playing_index = state->selected_index;
                                state->auto_stopped = false;
                                jfin_report_start(session, item->id);
                                album_art_load(session, item);
                            }
                        }
                    } else {
                        /* Audio-only (music, or video on Old 3DS) */
                        video_player_stop(); /* stop any video playback */
                        jfin_stream_t stream;
                        if (jfin_get_audio_stream(session, item->id, 0, &stream)) {
                            audio_player_play(stream.url, item->runtime_ticks, 0);
                            state->now_playing = *item;
                            state->has_now_playing = true;
                            state->playing_index = state->selected_index;
                            state->auto_stopped = false;
                            state->previous_view = state->current_view;
                            state->current_view = VIEW_NOW_PLAYING;
                            jfin_report_start(session, item->id);
                            album_art_load(session, item);
                        }
                    }
                } else if (is_container) {
                    ui_navigate_into(state, session, item);
                } else if (item->type == JFIN_ITEM_BOOK) {
                    state->now_playing      = *item;
                    state->reader_page      = 0;
                    state->reader_load_page = false;
                    state->reader_rotated   = false;
                    state->reader_split     = false;
                    state->reader_zoom      = 1.0f;
                    state->reader_pan_x     = 0.0f;
                    state->reader_pan_y     = 0.0f;
                    state->previous_view    = state->current_view;
                    state->current_view     = VIEW_READER;
                    /* Save companion .txt so downloads manager shows a readable name */
                    {
                        char cbz_meta[192];
                        cache_path(item->id, "txt", cbz_meta, sizeof(cbz_meta));
                        FILE *mf = fopen(cbz_meta, "w");
                        if (mf) {
                            for (int _d = 1; _d < state->parent_depth; _d++)
                                fprintf(mf, "%s / ", state->parent_stack_names[_d]);
                            fprintf(mf, "%s", item->name);
                            fclose(mf);
                        }
                    }
                    reader_open_book(session, item->id);
                }
                /* JFIN_ITEM_UNKNOWN: do nothing */
            }
        }
        /* B to go back */
        if (kdown & KEY_B) {
            ui_navigate_back(state, session);
        }
        /* L/R for pagination */
        if (kdown & KEY_R) {
            /* Next page */
            if (state->items.start_index + state->items.count < state->items.total_count) {
                int next_start = state->items.start_index + JFIN_MAX_ITEMS;
                const char *parent_id = (state->parent_depth > 0)
                    ? state->parent_stack_ids[state->parent_depth - 1] : NULL;
                if (state->current_view == VIEW_LIBRARIES) {
                    /* Libraries don't paginate the same way */
                } else if (parent_id) {
                    jfin_get_items(session, parent_id, next_start, JFIN_MAX_ITEMS, &state->items);
                    state->selected_index = 0;
                    state->scroll_offset = 0;
                }
            }
        }
        if (kdown & KEY_L) {
            /* Previous page */
            if (state->items.start_index > 0) {
                int prev_start = state->items.start_index - JFIN_MAX_ITEMS;
                if (prev_start < 0) prev_start = 0;
                const char *parent_id = (state->parent_depth > 0)
                    ? state->parent_stack_ids[state->parent_depth - 1] : NULL;
                if (parent_id) {
                    jfin_get_items(session, parent_id, prev_start, JFIN_MAX_ITEMS, &state->items);
                    state->selected_index = 0;
                    state->scroll_offset = 0;
                }
            }
        }
        /* X: download video/book/audio. ZL+X: download video with first available subtitle. */
        if (kdown & KEY_X) {
            if (state->selected_index < state->items.count) {
                jfin_item_t *item = &state->items.items[state->selected_index];
                bool is_video = (item->type == JFIN_ITEM_MOVIE ||
                                 item->type == JFIN_ITEM_EPISODE);
                bool is_book  = (item->type == JFIN_ITEM_BOOK);
                bool is_audio = (item->type == JFIN_ITEM_AUDIO);
                bool is_album = (item->type == JFIN_ITEM_MUSIC_ALBUM);
                if (is_video) {
                    jfin_stream_t stream;
                    bool want_first_sub = (kheld & KEY_ZL) != 0;
                    int dl_sub_idx;
                    char dl_sub_lang[8] = "";

                    if (want_first_sub) {
                        /* ZL+X: fetch first available subtitle and burn it in */
                        if (state->subtitle_list_loaded && state->subtitle_list.count > 0) {
                            dl_sub_idx = state->subtitle_list.subs[0].index;
                            snprintf(dl_sub_lang, sizeof(dl_sub_lang), "%s",
                                     state->subtitle_list.subs[0].language);
                        } else {
                            jfin_subtitle_list_t tmp_subs;
                            if (jfin_get_subtitle_streams(session, item->id, &tmp_subs)
                                && tmp_subs.count > 0) {
                                dl_sub_idx = tmp_subs.subs[0].index;
                                snprintf(dl_sub_lang, sizeof(dl_sub_lang), "%s",
                                         tmp_subs.subs[0].language);
                            } else {
                                dl_sub_idx = -1;
                            }
                        }
                    } else {
                        /* Plain X: use currently active subtitle (may be -1 = none) */
                        dl_sub_idx = state->subtitle_stream_index;
                        if (state->subtitle_stream_index >= 0 && state->subtitle_list_loaded) {
                            for (int si = 0; si < state->subtitle_list.count; si++) {
                                if (state->subtitle_list.subs[si].index == state->subtitle_stream_index) {
                                    snprintf(dl_sub_lang, sizeof(dl_sub_lang), "%s",
                                             state->subtitle_list.subs[si].language);
                                    break;
                                }
                            }
                        }
                    }

                    /* Build breadcrumb display name: "Series / Season / E## - Title" */
                    char dl_name[256];
                    if (item->type == JFIN_ITEM_EPISODE) {
                        char series_part[80] = "";
                        if (item->series_name[0])
                            snprintf(series_part, sizeof(series_part), "%s / ", item->series_name);
                        else if (state->parent_depth >= 2)
                            snprintf(series_part, sizeof(series_part), "%s / ",
                                     state->parent_stack_names[state->parent_depth - 2]);
                        char season_part[64] = "";
                        if (state->parent_depth >= 1)
                            snprintf(season_part, sizeof(season_part), "%s / ",
                                     state->parent_stack_names[state->parent_depth - 1]);
                        if (item->index_number > 0)
                            snprintf(dl_name, sizeof(dl_name), "%s%sE%02d - %s",
                                     series_part, season_part, item->index_number, item->name);
                        else
                            snprintf(dl_name, sizeof(dl_name), "%s%s%s",
                                     series_part, season_part, item->name);
                    } else {
                        snprintf(dl_name, sizeof(dl_name), "%s", item->name);
                    }
                    if (jfin_get_video_stream(session, item->id, 0, false, dl_sub_idx, &stream))
                        dl_queue_video(item->id, dl_name, stream.url, dl_sub_lang,
                                       stream.subtitle_url, item->runtime_ticks);
                } else if (is_book) {
                    /* Build breadcrumb display name for the book */
                    char book_name[256];
                    book_name[0] = '\0';
                    for (int _d = 1; _d < state->parent_depth; _d++) {
                        strncat(book_name, state->parent_stack_names[_d],
                                sizeof(book_name) - strlen(book_name) - 1);
                        strncat(book_name, " / ",
                                sizeof(book_name) - strlen(book_name) - 1);
                    }
                    strncat(book_name, item->name, sizeof(book_name) - strlen(book_name) - 1);
                    /* Build download URL with api_key */
                    char dl_url[JFIN_URL_BUF];
                    snprintf(dl_url, sizeof(dl_url), "%s/Items/%s/Download?api_key=%s",
                             session->server_url, item->id, session->access_token);
                    dl_queue_book(item->id, book_name, dl_url);
                } else if (is_audio) {
                    /* Download single audio track as MP3 */
                    jfin_stream_t aud_stream;
                    if (jfin_get_audio_stream(session, item->id, 0, &aud_stream)) {
                        char dl_name[256];
                        if (item->artist[0] && item->album[0])
                            snprintf(dl_name, sizeof(dl_name), "%s / %s / %s",
                                     item->artist, item->album, item->name);
                        else
                            snprintf(dl_name, sizeof(dl_name), "%s", item->name);
                        dl_queue_audio(item->id, dl_name, aud_stream.url);
                    }
                } else if (is_album) {
                    /* Download all tracks in album (static: ~23KB, too large for stack) */
                    static jfin_item_list_t album_tracks;
                    if (jfin_get_items(session, item->id, 0, JFIN_MAX_ITEMS, &album_tracks)) {
                        const char *album_artist = item->artist[0] ? item->artist : item->name;
                        for (int ai = 0; ai < album_tracks.count; ai++) {
                            jfin_item_t *track = &album_tracks.items[ai];
                            if (track->type != JFIN_ITEM_AUDIO) continue;
                            jfin_stream_t aud_stream;
                            if (jfin_get_audio_stream(session, track->id, 0, &aud_stream)) {
                                char dl_name[256];
                                snprintf(dl_name, sizeof(dl_name), "%s / %s / %s",
                                         album_artist, item->name, track->name);
                                dl_queue_audio(track->id, dl_name, aud_stream.url);
                            }
                        }
                    }
                }
            }
        }
        /* ZL+Y: download just the subtitle (.ass) for the hovered video item */
        if ((kdown & KEY_Y) && (kheld & KEY_ZL)
                && state->selected_index < state->items.count
                && session->authenticated) {
            jfin_item_t *item = &state->items.items[state->selected_index];
            if (item->type == JFIN_ITEM_EPISODE || item->type == JFIN_ITEM_MOVIE) {
                int sub_idx = -1;
                char sub_lang[8] = "";
                if (state->subtitle_list_loaded && state->subtitle_list.count > 0) {
                    sub_idx = state->subtitle_list.subs[0].index;
                    strncpy(sub_lang, state->subtitle_list.subs[0].language, 7);
                } else {
                    jfin_subtitle_list_t tmp_subs;
                    if (jfin_get_subtitle_streams(session, item->id, &tmp_subs)
                            && tmp_subs.count > 0) {
                        sub_idx = tmp_subs.subs[0].index;
                        strncpy(sub_lang, tmp_subs.subs[0].language, 7);
                    }
                }
                if (sub_idx >= 0) {
                    jfin_stream_t sub_stream;
                    if (jfin_get_video_stream(session, item->id, 0, false,
                                              sub_idx, &sub_stream)
                            && sub_stream.subtitle_url[0]) {
                        char dl_name[256];
                        snprintf(dl_name, sizeof(dl_name), "%s", item->name);
                        if (dl_queue_subtitle_only(item->id, dl_name, sub_stream.subtitle_url))
                            log_write("UI: queued sub-only for %s (%s)", item->id, sub_lang);
                    }
                }
            }
        }
        /* ZL+A: download selected video with subtitles burned into the transcode */
        if ((kdown & KEY_A) && (kheld & KEY_ZL)
                && state->selected_index < state->items.count
                && session->authenticated) {
            jfin_item_t *item = &state->items.items[state->selected_index];
            if (item->type == JFIN_ITEM_EPISODE || item->type == JFIN_ITEM_MOVIE) {
                int sub_idx = -1;
                char sub_lang[8] = "";
                if (state->subtitle_list_loaded && state->subtitle_list.count > 0) {
                    sub_idx = state->subtitle_list.subs[0].index;
                    strncpy(sub_lang, state->subtitle_list.subs[0].language, 7);
                } else {
                    jfin_subtitle_list_t tmp_subs;
                    if (jfin_get_subtitle_streams(session, item->id, &tmp_subs)
                            && tmp_subs.count > 0) {
                        sub_idx = tmp_subs.subs[0].index;
                        strncpy(sub_lang, tmp_subs.subs[0].language, 7);
                    }
                }
                if (sub_idx >= 0) {
                    jfin_stream_t enc_stream;
                    if (jfin_get_video_stream_encoded(session, item->id, sub_idx, &enc_stream)) {
                        char dl_name[256];
                        if (item->type == JFIN_ITEM_EPISODE) {
                            char series_part[80] = "";
                            if (item->series_name[0])
                                snprintf(series_part, sizeof(series_part), "%s / ", item->series_name);
                            else if (state->parent_depth >= 2)
                                snprintf(series_part, sizeof(series_part), "%s / ",
                                         state->parent_stack_names[state->parent_depth - 2]);
                            char season_part[64] = "";
                            if (state->parent_depth >= 1)
                                snprintf(season_part, sizeof(season_part), "%s / ",
                                         state->parent_stack_names[state->parent_depth - 1]);
                            if (item->index_number > 0)
                                snprintf(dl_name, sizeof(dl_name), "%s%sE%02d - %s",
                                         series_part, season_part, item->index_number, item->name);
                            else
                                snprintf(dl_name, sizeof(dl_name), "%s%s%s",
                                         series_part, season_part, item->name);
                        } else {
                            snprintf(dl_name, sizeof(dl_name), "%s", item->name);
                        }
                        dl_queue_video(item->id, dl_name, enc_stream.url, sub_lang,
                                       "", item->runtime_ticks);
                        log_write("UI: queued encoded-sub dl for %s (%s)", item->id, sub_lang);
                    }
                }
            }
        }
        /* Y to toggle now-playing view */
        if ((kdown & KEY_Y) && !(kheld & KEY_ZL) && state->has_now_playing) {
            state->previous_view = state->current_view;
            state->current_view = VIEW_NOW_PLAYING;
        }
        /* SELECT: settings in libraries, search in browse */
        if (kdown & KEY_SELECT) {
            if (state->current_view == VIEW_LIBRARIES) {
                state->previous_view = VIEW_LIBRARIES;
                state->settings_index = 0;
                state->settings_scroll = 0;
                state->current_view = VIEW_SETTINGS;
            } else {
                SwkbdState swkbd;
                char query[128] = {0};
                swkbdInit(&swkbd, SWKBD_TYPE_WESTERN, 2, 127);
                swkbdSetHintText(&swkbd, "Search...");
                SwkbdButton button = swkbdInputText(&swkbd, query, sizeof(query));
                if (button == SWKBD_BUTTON_CONFIRM && query[0] != '\0') {
                    jfin_search(session, query, JFIN_MAX_ITEMS, &state->items);
                    state->selected_index = 0;
                    state->scroll_offset = 0;
                    state->current_view = VIEW_BROWSE;
                }
            }
        }
        /* Auto-advance: when current track/episode finishes, play next */
        if (state->has_now_playing && state->auto_advance && !state->auto_stopped) {
            player_status_t ps = audio_player_get_status();
            video_status_t vs = video_player_get_status();
            bool audio_finished = (ps.state == PLAYER_STOPPED && vs.state == VIDEO_STOPPED);
            bool video_finished = (vs.state == VIDEO_STOPPED && ps.state == PLAYER_STOPPED);

            if (audio_finished || video_finished) {
                int next;
                if (state->repeat_mode == 1) {
                    /* Repeat-one: replay the same track */
                    next = state->playing_index;
                } else if (state->shuffle_mode && state->items.count > 1) {
                    /* Shuffle: pick a random track that's different from current */
                    do {
                        next = rand() % state->items.count;
                    } while (next == state->playing_index);
                } else {
                    next = state->playing_index + 1;
                    /* Repeat-all: wrap around */
                    if (state->repeat_mode == 2 && next >= state->items.count)
                        next = 0;
                }
                if (next < state->items.count) {
                    jfin_item_t *next_item = &state->items.items[next];
                    /* Only auto-advance to same playable type */
                    bool next_playable = (next_item->type == JFIN_ITEM_AUDIO ||
                                          next_item->type == JFIN_ITEM_MOVIE ||
                                          next_item->type == JFIN_ITEM_EPISODE);
                    bool next_is_video = (next_item->type == JFIN_ITEM_MOVIE ||
                                          next_item->type == JFIN_ITEM_EPISODE);
                    if (next_playable) {
                        vp_3d_mode_t next_mode = item_3d_mode(next_item);
                        jfin_stream_t stream;
                        bool started = false;
                        apply_sticky_subtitle(state, session, next_item->id);
                        if (next_is_video && video_player_is_supported()) {
                            state->video_retry_count = 0;
                            state->video_retry_timer = 0;
                            if (jfin_get_video_stream(session, next_item->id, 0,
                                                      next_mode != VP_3D_NONE,
                                                      state->subtitle_stream_index,
                                                      &stream) &&
                                video_player_play(stream.url, stream.subtitle_url, next_item->runtime_ticks, 0, next_mode)) {
                                started = true;
                            } else if (jfin_get_audio_stream(session, next_item->id, 0, &stream)) {
                                audio_player_play(stream.url, next_item->runtime_ticks, 0);
                                started = true;
                            }
                        } else {
                            if (jfin_get_audio_stream(session, next_item->id, 0, &stream)) {
                                audio_player_play(stream.url, next_item->runtime_ticks, 0);
                                started = true;
                            }
                        }
                        if (started) {
                            state->now_playing = *next_item;
                            state->playing_index = next;
                            state->auto_stopped = false;
                            /* Clear offline flags so X/seek handlers use the fast online path */
                            state->now_playing_offline = false;
                            state->now_playing_local_path[0] = '\0';
                            state->now_playing_sub_path[0] = '\0';
                            jfin_report_start(session, next_item->id);
                            album_art_load(session, next_item);
                        } else {
                            state->has_now_playing = false;
                        }
                    } else {
                        state->has_now_playing = false;
                    }
                } else {
                    state->has_now_playing = false;
                }
            }
        }
        break;

    case VIEW_NOW_PLAYING:
        {
            video_status_t vs = video_player_get_status();
            bool vid_active = (vs.state == VIDEO_PLAYING || vs.state == VIDEO_PAUSED ||
                               vs.state == VIDEO_LOADING);

            /* Error state */
            if (vs.state == VIDEO_ERROR) {
                if (kdown) {
                    state->video_retry_count = 0;
                    state->video_retry_timer = 0;
                    video_player_stop();
                    audio_player_stop();
                    state->has_now_playing = false;
                    state->current_view = state->previous_view;
                    break;
                }
                /* Subtitle seeks need longer retries — server-side retranscode can take >10s */
                int max_retries    = (state->subtitle_stream_index >= 0) ? 5 : 3;
                int retry_interval = (state->subtitle_stream_index >= 0) ? 300 : 180;
                if (!state->now_playing_offline
                    && strncmp(vs.error_msg, "Video not ready", 15) == 0
                    && state->video_retry_count < max_retries) {
                    if (state->video_retry_timer == 0) {
                        state->video_retry_timer = retry_interval;
                    } else {
                        state->video_retry_timer--;
                        if (state->video_retry_timer == 0) {
                            state->video_retry_count++;
                            vp_3d_mode_t retry_mode = item_3d_mode(&state->now_playing);
                            jfin_stream_t retry_stream;
                            if (jfin_get_video_stream(session, state->now_playing.id,
                                                      state->video_retry_ticks,
                                                      retry_mode != VP_3D_NONE,
                                                      state->subtitle_stream_index,
                                                      &retry_stream))
                                video_player_play(retry_stream.url, retry_stream.subtitle_url,
                                                  state->now_playing.runtime_ticks,
                                                  state->video_retry_ticks,
                                                  retry_mode);
                        }
                    }
                } else if (!state->now_playing_offline
                           && strncmp(vs.error_msg, "Video not ready", 15) == 0
                           && state->video_retry_count >= max_retries
                           && state->subtitle_stream_index >= 0) {
                    /* Subtitle transcode timed out — fall back to no-subtitle at same position */
                    state->subtitle_stream_index = -1;
                    state->subtitle_sticky = false;
                    state->subtitle_lang_pref[0] = '\0';
                    state->video_retry_count = 0;
                    state->video_retry_timer = 0;
                    vp_3d_mode_t fb_mode = item_3d_mode(&state->now_playing);
                    jfin_stream_t fb_stream;
                    if (jfin_get_video_stream(session, state->now_playing.id,
                                              state->video_retry_ticks,
                                              fb_mode != VP_3D_NONE,
                                              -1,
                                              &fb_stream))
                        video_player_play(fb_stream.url, NULL,
                                          state->now_playing.runtime_ticks,
                                          state->video_retry_ticks,
                                          fb_mode);
                }
                break;
            }

            /* Shutdown timer popup — consumes all input while open */
            if (state->shutdown_popup_open) {
                if (kdown & KEY_DLEFT)
                    state->shutdown_popup_sel = (state->shutdown_popup_sel + 2) % 3;
                if (kdown & KEY_DRIGHT)
                    state->shutdown_popup_sel = (state->shutdown_popup_sel + 1) % 3;
                if (kdown & KEY_DUP) {
                    if      (state->shutdown_popup_sel == 0) state->shutdown_popup_h = (state->shutdown_popup_h +  1) % 24;
                    else if (state->shutdown_popup_sel == 1) state->shutdown_popup_m = (state->shutdown_popup_m +  1) % 60;
                    else                                     state->shutdown_popup_s = (state->shutdown_popup_s +  1) % 60;
                }
                if (kdown & KEY_DDOWN) {
                    if      (state->shutdown_popup_sel == 0) state->shutdown_popup_h = (state->shutdown_popup_h + 23) % 24;
                    else if (state->shutdown_popup_sel == 1) state->shutdown_popup_m = (state->shutdown_popup_m + 59) % 60;
                    else                                     state->shutdown_popup_s = (state->shutdown_popup_s + 59) % 60;
                }
                if (kdown & KEY_A) {
                    u64 total_ms = ((u64)state->shutdown_popup_h * 3600 +
                                    (u64)state->shutdown_popup_m *   60 +
                                    (u64)state->shutdown_popup_s) * 1000;
                    if (total_ms > 0) {
                        state->shutdown_timer_deadline = osGetTime() + total_ms;
                        state->shutdown_timer_active   = true;
                    }
                    state->shutdown_popup_open = false;
                }
                if (kdown & KEY_B)
                    state->shutdown_popup_open = false;
                break;
            }

            /* Watch mode: D-pad down hides controls, only up exits */
            if (state->bottom_hidden) {
                if (kdown & KEY_DUP)
                    state->bottom_hidden = false;
                break; /* ignore all other buttons in watch mode */
            }
            /* D-pad down: enter watch mode */
            if (kdown & KEY_DDOWN) {
                state->bottom_hidden = true;
                break;
            }
            /* A to pause/resume (skip if ZL held — ZL+A is encoded-sub download) */
            if ((kdown & KEY_A) && !(kheld & KEY_ZL)) {
                if (vid_active)
                    video_player_pause();
                else
                    audio_player_pause();
            }
            /* For offline: only sync position when MVD is actually decoding frames.
             * vs.state == VIDEO_PLAYING triggers even when MVD fails because AAC
             * audio starts successfully. vs.position_ticks then races to end-of-file
             * as the DEC thread consumes failed packets at SD-card speed.
             * vs.frames_decoded only increments when mvdstdProcessVideoFrame succeeds,
             * so it is zero for the entire duration of a failed seek session.
             * Detect a new player session by checking if frames_decoded decreased. */
            {
                static int s_off_prev_decoded = 0;
                if (state->now_playing_offline) {
                    if (vs.frames_decoded < s_off_prev_decoded)
                        s_off_prev_decoded = 0; /* new session started — counter reset */
                    if (vs.frames_decoded > s_off_prev_decoded) {
                        state->video_retry_ticks = vs.position_ticks;
                        s_off_prev_decoded = vs.frames_decoded;
                    }
                } else {
                    s_off_prev_decoded = 0;
                }
            }

            /* L/R to seek (R: +30s, L: -15s) */
            if ((kdown & KEY_L) || (kdown & KEY_R)) {
                int64_t offset = (kdown & KEY_R) ? 300000000LL : -150000000LL; /* R:+30s L:-15s */
                int64_t cur_pos;
                if (vid_active)
                    cur_pos = state->now_playing_offline
                              ? state->video_retry_ticks   /* immune to DEC racing */
                              : vs.position_ticks;
                else
                    cur_pos = audio_player_get_status().position_ticks;

                int64_t new_pos = cur_pos + offset;
                if (new_pos < 0) new_pos = 0;
                if (state->now_playing.runtime_ticks > 0 &&
                    new_pos >= state->now_playing.runtime_ticks)
                    new_pos = state->now_playing.runtime_ticks - 100000000LL; /* 10s before end */

                jfin_stream_t stream;
                if (vid_active) {
                    video_player_stop();
                    state->video_retry_count = 0;
                    state->video_retry_timer = 0;
                    if (state->now_playing_offline) {
                        /* Seek by byte offset in local TS file (net_thread estimates position) */
                        state->video_retry_ticks = new_pos;
                        const char *seek_sub = (state->offline_subs_on && state->now_playing_sub_path[0])
                                               ? state->now_playing_sub_path : NULL;
                        video_player_play(state->now_playing_local_path, seek_sub,
                                          0, new_pos, VP_3D_NONE);
                    } else {
                        vp_3d_mode_t mode_3d = item_3d_mode(&state->now_playing);
                        state->video_retry_ticks = new_pos;
                        if (jfin_get_video_stream(session, state->now_playing.id, new_pos,
                                                  mode_3d != VP_3D_NONE,
                                                  state->subtitle_stream_index,
                                                  &stream))
                            video_player_play(stream.url, stream.subtitle_url, state->now_playing.runtime_ticks, new_pos, mode_3d);
                    }
                } else {
                    /* Audio seek: only call Jellyfin for online streams.
                     * For offline (local MP3), seeking is not yet supported. */
                    if (!state->now_playing_offline) {
                        audio_player_stop();
                        if (jfin_get_audio_stream(session, state->now_playing.id, new_pos, &stream))
                            audio_player_play(stream.url, state->now_playing.runtime_ticks, new_pos);
                    }
                }
            }
            /* Y (video offline): toggle subtitle display on/off */
            if ((kdown & KEY_Y) && vid_active && state->now_playing_offline
                && state->now_playing_sub_path[0]) {
                state->offline_subs_on = !state->offline_subs_on;
                if (state->offline_subs_on)
                    video_player_load_subtitles(state->now_playing_sub_path);
                else
                    video_player_clear_subtitles();
            }
            /* Y (video online): cycle subtitle tracks */
            if ((kdown & KEY_Y) && vid_active && !state->now_playing_offline) {
                if (!state->subtitle_list_loaded) {
                    if (jfin_get_subtitle_streams(session, state->now_playing.id,
                                                  &state->subtitle_list))
                        state->subtitle_list_loaded = true;
                }
                int next_index = -1;
                if (state->subtitle_stream_index < 0) {
                    if (state->subtitle_list_loaded && state->subtitle_list.count > 0)
                        next_index = state->subtitle_list.subs[0].index;
                } else {
                    int cur_pos = -1;
                    for (int si = 0; si < state->subtitle_list.count; si++) {
                        if (state->subtitle_list.subs[si].index == state->subtitle_stream_index) {
                            cur_pos = si; break;
                        }
                    }
                    if (cur_pos < 0 || cur_pos >= state->subtitle_list.count - 1)
                        next_index = -1;
                    else
                        next_index = state->subtitle_list.subs[cur_pos + 1].index;
                }
                state->subtitle_stream_index = next_index;
                if (next_index >= 0) {
                    state->subtitle_sticky = true;
                    for (int si = 0; si < state->subtitle_list.count; si++) {
                        if (state->subtitle_list.subs[si].index == next_index) {
                            snprintf(state->subtitle_lang_pref,
                                     sizeof(state->subtitle_lang_pref),
                                     "%s", state->subtitle_list.subs[si].language);
                            break;
                        }
                    }
                } else {
                    state->subtitle_sticky = false;
                    state->subtitle_lang_pref[0] = '\0';
                }
                int64_t resume_ticks = vs.position_ticks;
                vp_3d_mode_t sub_mode = item_3d_mode(&state->now_playing);
                video_player_stop();
                state->video_retry_count = 0;
                state->video_retry_timer = 0;
                state->video_retry_ticks = resume_ticks;
                jfin_stream_t sub_stream;
                if (jfin_get_video_stream(session, state->now_playing.id, resume_ticks,
                                          sub_mode != VP_3D_NONE,
                                          state->subtitle_stream_index, &sub_stream))
                    video_player_play(sub_stream.url, sub_stream.subtitle_url,
                                      state->now_playing.runtime_ticks,
                                      resume_ticks, sub_mode);
            }
            /* Y (audio): toggle shuffle */
            if ((kdown & KEY_Y) && !vid_active)
                state->shuffle_mode = !state->shuffle_mode;
            /* ZR+SELECT: open shutdown timer popup */
            if ((kdown & KEY_SELECT) && (kheld & KEY_ZR)) {
                state->shutdown_popup_open = true;
                state->shutdown_popup_sel  = 1;
                break;
            }
            /* SELECT: repeat mode 0→1→2 (audio only) */
            if ((kdown & KEY_SELECT) && !vid_active)
                state->repeat_mode = (state->repeat_mode + 1) % 3;
            /* ZL/ZR (audio, New 3DS): previous / next track */
            if (!vid_active && state->has_now_playing) {
                int skip_dir = 0;
                if (kdown & KEY_ZR) skip_dir = +1;
                if (kdown & KEY_ZL) skip_dir = -1;
                if (skip_dir != 0) {
                    int target = state->playing_index + skip_dir;
                    if (target >= 0 && target < state->items.count) {
                        jfin_item_t *trk = &state->items.items[target];
                        jfin_stream_t skip_stream;
                        audio_player_stop();
                        if (jfin_get_audio_stream(session, trk->id, 0, &skip_stream)) {
                            audio_player_play(skip_stream.url, trk->runtime_ticks, 0);
                            state->now_playing  = *trk;
                            state->playing_index = target;
                            state->auto_stopped  = false;
                            jfin_report_start(session, trk->id);
                            album_art_load(session, trk);
                        }
                    }
                }
            }
            /* X / ZL+X (video): queue download of next undownloaded episode.
             * ZL held = burn in current subtitle track (same as browse ZL+X). */
            if ((kdown & KEY_X) && state->has_now_playing &&
                    (vid_active || state->now_playing_local_path[0]) &&
                    session->authenticated) {
                bool with_subs = (kheld & KEY_ZL) != 0;
                /* sub_idx: prefer current active track; -1 forces per-candidate lookup.
                 * Offline: subtitle_stream_index is stale, always force lookup. */
                int sub_idx = (with_subs && !state->now_playing_offline)
                              ? state->subtitle_stream_index : -1;
                const char *sub_name = (with_subs && state->subtitle_lang_pref[0])
                                       ? state->subtitle_lang_pref : NULL;

#define TRY_QUEUE_NEXT(list_ptr, _mc, start_i)                                \
    do {                                                                       \
        for (int _i = (start_i); _i < (_mc); _i++) {                          \
            jfin_item_t *cand = &(list_ptr)[_i];                               \
            if (cand->type != JFIN_ITEM_EPISODE &&                             \
                cand->type != JFIN_ITEM_MOVIE) continue;                       \
            bool already = cache_has(cand->id, "ts") ||                        \
                           dl_queue_has_video(cand->id);                       \
            if (already) continue;                                             \
            /* When ZL+X but no sub currently active, look up subs for cand */\
            int _eff_sub = sub_idx;                                            \
            char _eff_lang[8] = {0};                                           \
            const char *_eff_sub_name = sub_name;                              \
            if (with_subs && _eff_sub < 0) {                                   \
                jfin_subtitle_list_t _csl;                                      \
                if (jfin_get_subtitle_streams(session, cand->id, &_csl) &&     \
                        _csl.count > 0) {                                       \
                    _eff_sub = _csl.subs[0].index;                             \
                    strncpy(_eff_lang, _csl.subs[0].language, 7);              \
                    _eff_sub_name = _eff_lang[0] ? _eff_lang : NULL;           \
                    if (sub_name) {                                             \
                        for (int _sli = 0; _sli < _csl.count; _sli++) {        \
                            if (strcmp(_csl.subs[_sli].language, sub_name)==0) { \
                                _eff_sub = _csl.subs[_sli].index;              \
                                strncpy(_eff_lang, _csl.subs[_sli].language, 7);\
                                _eff_sub_name = _eff_lang;                     \
                                break;                                          \
                            }                                                   \
                        }                                                       \
                    }                                                           \
                }                                                               \
            }                                                                   \
            bool got_sub = with_subs && (_eff_sub >= 0);                       \
            jfin_stream_t xstream;                                             \
            vp_3d_mode_t xmode = item_3d_mode(cand);                          \
            if (jfin_get_video_stream(session, cand->id, 0,                    \
                                      xmode != VP_3D_NONE, _eff_sub, &xstream)) { \
                char xname[256];                                               \
                if (cand->series_name[0] && cand->season_number > 0)           \
                    snprintf(xname, sizeof(xname), "%s / Season %d / E%02d - %s", \
                             cand->series_name, cand->season_number,           \
                             cand->index_number, cand->name);                  \
                else if (cand->series_name[0])                                 \
                    snprintf(xname, sizeof(xname), "%s / E%02d - %s",         \
                             cand->series_name, cand->index_number, cand->name); \
                else snprintf(xname, sizeof(xname), "%s", cand->name);        \
                dl_queue_video(cand->id, xname, xstream.url, _eff_sub_name,   \
                               xstream.subtitle_url, cand->runtime_ticks);    \
                if (cand->index_number > 0)                                    \
                    snprintf(state->np_toast, sizeof(state->np_toast),         \
                             got_sub ? "DL+sub: E%02d - %s"                    \
                                     : "DL: E%02d - %s",                       \
                             cand->index_number, cand->name);                  \
                else snprintf(state->np_toast, sizeof(state->np_toast),        \
                              got_sub ? "DL+sub: %s" : "DL: %s", cand->name); \
                state->np_toast_timer = 150;                                   \
            }                                                                  \
            break;                                                             \
        }                                                                      \
    } while (0)

                if (!state->now_playing_offline && state->playing_index >= 0) {
                    /* Online fast path: browse list already has the season */
                    int _prev_toast = state->np_toast_timer;
                    TRY_QUEUE_NEXT(state->items.items, state->items.count,
                                   state->playing_index + 1);
                    /* If nothing was queued and more pages exist on the server,
                     * fetch the next page and continue searching from index 0.
                     * np_toast_timer is set to 150 by TRY_QUEUE_NEXT on success. */
                    if (state->np_toast_timer == _prev_toast &&
                        state->items.total_count >
                        state->items.start_index + state->items.count) {
                        const char *_pg_pid = (state->parent_depth > 0)
                            ? state->parent_stack_ids[state->parent_depth - 1] : NULL;
                        if (_pg_pid) {
                            static jfin_item_list_t s_x_overflow;
                            int _pg_start = state->items.start_index + state->items.count;
                            if (jfin_get_items(session, _pg_pid, _pg_start,
                                               JFIN_MAX_ITEMS, &s_x_overflow))
                                TRY_QUEUE_NEXT(s_x_overflow.items, s_x_overflow.count, 0);
                        }
                    }
                } else {
                    /* Offline path: resolve parent from server */
                    char xparent[JFIN_MAX_ID] = {0};
                    if (state->now_playing.parent_id[0])
                        strncpy(xparent, state->now_playing.parent_id, sizeof(xparent)-1);
                    else if (state->now_playing.id[0])
                        jfin_get_item_parent_id(session, state->now_playing.id,
                                                xparent, sizeof(xparent));
                    if (xparent[0]) {
                        static jfin_item_list_t s_sibs;
                        int sib_start = state->now_playing.index_number > 1
                                        ? state->now_playing.index_number - 1 : 0;
                        if (jfin_get_siblings(session, xparent, sib_start,
                                              JFIN_MAX_ITEMS, &s_sibs)) {
                            int cur_pos = -1;
                            for (int si = 0; si < s_sibs.count; si++) {
                                if (strcmp(s_sibs.items[si].id,
                                           state->now_playing.id) == 0)
                                    { cur_pos = si; break; }
                            }
                            if (cur_pos < 0 && state->now_playing.index_number > 0) {
                                for (int si = 0; si < s_sibs.count; si++) {
                                    if (s_sibs.items[si].index_number ==
                                            state->now_playing.index_number)
                                        { cur_pos = si; break; }
                                }
                            }
                            TRY_QUEUE_NEXT(s_sibs.items, s_sibs.count,
                                           cur_pos >= 0 ? cur_pos + 1 : 0);
                        }
                    }
                }
#undef TRY_QUEUE_NEXT
            }
            /* ZL+Y (now-playing): download subtitle .ass for the current episode */
            if ((kdown & KEY_Y) && (kheld & KEY_ZL) && state->has_now_playing
                    && state->now_playing.id[0] && session->authenticated) {
                int sub_idx = -1;
                char sub_lang[8] = "";
                /* Always fetch fresh — subtitle_stream_index may be stale from a
                 * different episode or a previous online session. */
                jfin_subtitle_list_t tmp_subs;
                if (jfin_get_subtitle_streams(session, state->now_playing.id, &tmp_subs)
                        && tmp_subs.count > 0) {
                    sub_idx = tmp_subs.subs[0].index;
                    strncpy(sub_lang, tmp_subs.subs[0].language, 7);
                }
                if (sub_idx >= 0) {
                    jfin_stream_t sub_stream;
                    if (jfin_get_video_stream(session, state->now_playing.id, 0, false,
                                              sub_idx, &sub_stream)
                            && sub_stream.subtitle_url[0]) {
                        if (dl_queue_subtitle_only(state->now_playing.id,
                                                   state->now_playing.name,
                                                   sub_stream.subtitle_url)) {
                            snprintf(state->np_toast, sizeof(state->np_toast),
                                     "DL sub: %s", sub_lang[0] ? sub_lang : "track");
                            state->np_toast_timer = 150;
                        }
                    }
                }
            }
            /* ZL+A (now-playing): download next episode with subtitles burned in */
            if ((kdown & KEY_A) && (kheld & KEY_ZL) && state->has_now_playing &&
                    (vid_active || state->now_playing_local_path[0]) &&
                    session->authenticated) {

#define TRY_QUEUE_NEXT_ENC(list_ptr, _mc, start_i)                                \
    do {                                                                           \
        for (int _i = (start_i); _i < (_mc); _i++) {                              \
            jfin_item_t *cand = &(list_ptr)[_i];                                   \
            if (cand->type != JFIN_ITEM_EPISODE &&                                 \
                cand->type != JFIN_ITEM_MOVIE) continue;                           \
            bool already = cache_has(cand->id, "ts") ||                            \
                           dl_queue_has_video(cand->id);                           \
            if (already) continue;                                                 \
            int _eff_sub = -1;                                                     \
            char _eff_lang[8] = {0};                                               \
            jfin_subtitle_list_t _csl;                                             \
            if (jfin_get_subtitle_streams(session, cand->id, &_csl) &&            \
                    _csl.count > 0) {                                               \
                _eff_sub = _csl.subs[0].index;                                     \
                strncpy(_eff_lang, _csl.subs[0].language, 7);                      \
            }                                                                       \
            if (_eff_sub < 0) break;                                                \
            jfin_stream_t xstream;                                                 \
            if (jfin_get_video_stream_encoded(session, cand->id,                   \
                                              _eff_sub, &xstream)) {               \
                char xname[256];                                                   \
                if (cand->series_name[0] && cand->season_number > 0)               \
                    snprintf(xname, sizeof(xname), "%s / Season %d / E%02d - %s", \
                             cand->series_name, cand->season_number,               \
                             cand->index_number, cand->name);                      \
                else if (cand->series_name[0])                                     \
                    snprintf(xname, sizeof(xname), "%s / E%02d - %s",             \
                             cand->series_name, cand->index_number, cand->name);   \
                else snprintf(xname, sizeof(xname), "%s", cand->name);            \
                dl_queue_video(cand->id, xname, xstream.url, _eff_lang,           \
                               "", cand->runtime_ticks);                           \
                if (cand->index_number > 0)                                        \
                    snprintf(state->np_toast, sizeof(state->np_toast),             \
                             "DL+enc: E%02d - %s",                                 \
                             cand->index_number, cand->name);                      \
                else snprintf(state->np_toast, sizeof(state->np_toast),            \
                              "DL+enc: %s", cand->name);                           \
                state->np_toast_timer = 150;                                       \
            }                                                                      \
            break;                                                                 \
        }                                                                          \
    } while (0)

                if (!state->now_playing_offline && state->playing_index >= 0) {
                    int _prev_toast = state->np_toast_timer;
                    TRY_QUEUE_NEXT_ENC(state->items.items, state->items.count,
                                       state->playing_index + 1);
                    if (state->np_toast_timer == _prev_toast &&
                        state->items.total_count >
                        state->items.start_index + state->items.count) {
                        const char *_pg_pid = (state->parent_depth > 0)
                            ? state->parent_stack_ids[state->parent_depth - 1] : NULL;
                        if (_pg_pid) {
                            static jfin_item_list_t s_a_overflow;
                            int _pg_start = state->items.start_index + state->items.count;
                            if (jfin_get_items(session, _pg_pid, _pg_start,
                                               JFIN_MAX_ITEMS, &s_a_overflow))
                                TRY_QUEUE_NEXT_ENC(s_a_overflow.items, s_a_overflow.count, 0);
                        }
                    }
                } else {
                    char xparent[JFIN_MAX_ID] = {0};
                    if (state->now_playing.parent_id[0])
                        strncpy(xparent, state->now_playing.parent_id, sizeof(xparent)-1);
                    else if (state->now_playing.id[0])
                        jfin_get_item_parent_id(session, state->now_playing.id,
                                                xparent, sizeof(xparent));
                    if (xparent[0]) {
                        static jfin_item_list_t s_a_sibs;
                        int sib_start = state->now_playing.index_number > 1
                                        ? state->now_playing.index_number - 1 : 0;
                        if (jfin_get_siblings(session, xparent, sib_start,
                                              JFIN_MAX_ITEMS, &s_a_sibs)) {
                            int cur_pos = -1;
                            for (int si = 0; si < s_a_sibs.count; si++) {
                                if (strcmp(s_a_sibs.items[si].id,
                                           state->now_playing.id) == 0)
                                    { cur_pos = si; break; }
                            }
                            if (cur_pos < 0 && state->now_playing.index_number > 0) {
                                for (int si = 0; si < s_a_sibs.count; si++) {
                                    if (s_a_sibs.items[si].index_number ==
                                            state->now_playing.index_number)
                                        { cur_pos = si; break; }
                                }
                            }
                            TRY_QUEUE_NEXT_ENC(s_a_sibs.items, s_a_sibs.count,
                                               cur_pos >= 0 ? cur_pos + 1 : 0);
                        }
                    }
                }
#undef TRY_QUEUE_NEXT_ENC
            }
            /* B: back to browse, keep playing */
            if (kdown & KEY_B) {
                state->bottom_hidden = false;
                state->current_view = state->previous_view;
            }
            /* START: stop media and exit now-playing */
            if (kdown & KEY_START) {
                state->bottom_hidden = false;
                video_player_stop();
                audio_player_stop();
                state->has_now_playing = false;
                state->auto_stopped = true;
                state->current_view = state->previous_view;
            }
            /* Auto-advance from now-playing screen */
            if (state->has_now_playing && state->auto_advance && !state->auto_stopped) {
                player_status_t ps = audio_player_get_status();
                bool audio_done = (ps.state == PLAYER_STOPPED && vs.state == VIDEO_STOPPED);
                bool video_done = (vs.state == VIDEO_STOPPED && ps.state == PLAYER_STOPPED);

                if (audio_done || video_done) {
                    int next = state->playing_index + 1;
                    if (next < state->items.count) {
                        jfin_item_t *next_item = &state->items.items[next];
                        bool next_playable = (next_item->type == JFIN_ITEM_AUDIO ||
                                              next_item->type == JFIN_ITEM_MOVIE ||
                                              next_item->type == JFIN_ITEM_EPISODE);
                        bool next_is_video = (next_item->type == JFIN_ITEM_MOVIE ||
                                              next_item->type == JFIN_ITEM_EPISODE);
                        if (next_playable) {
                            vp_3d_mode_t next_mode = item_3d_mode(next_item);
                            jfin_stream_t stream;
                            bool started = false;
                            apply_sticky_subtitle(state, session, next_item->id);
                            if (next_is_video && video_player_is_supported()) {
                                state->video_retry_count = 0;
                                state->video_retry_timer = 0;
                                if (jfin_get_video_stream(session, next_item->id, 0,
                                                          next_mode != VP_3D_NONE,
                                                          state->subtitle_stream_index,
                                                          &stream) &&
                                    video_player_play(stream.url, stream.subtitle_url, next_item->runtime_ticks, 0, next_mode)) {
                                    started = true;
                                } else if (jfin_get_audio_stream(session, next_item->id, 0, &stream)) {
                                    audio_player_play(stream.url, next_item->runtime_ticks, 0);
                                    started = true;
                                }
                            } else {
                                if (jfin_get_audio_stream(session, next_item->id, 0, &stream)) {
                                    audio_player_play(stream.url, next_item->runtime_ticks, 0);
                                    started = true;
                                }
                            }
                            if (started) {
                                state->now_playing = *next_item;
                                state->playing_index = next;
                                state->auto_stopped = false;
                                jfin_report_start(session, next_item->id);
                                album_art_load(session, next_item);
                            } else {
                                state->has_now_playing = false;
                                state->current_view = state->previous_view;
                            }
                        } else {
                            state->has_now_playing = false;
                            state->current_view = state->previous_view;
                        }
                    } else {
                        state->has_now_playing = false;
                        state->current_view = state->previous_view;
                    }
                }
            }
        }
        if (state->np_toast_timer > 0) state->np_toast_timer--;
        break;

    case VIEW_READER: {
        reader_state_t rs = reader_get_state();

        /* Once the CBZ is ready, load the pending page */
        if (rs == READER_READY && state->reader_load_page) {
            reader_load_page(state->reader_page, state->reader_rotated);
            state->reader_load_page = false;
        }

        /* Auto-trigger first page load when download just finished */
        if (rs == READER_READY && !reader_page_ready() && !state->reader_load_page)
            state->reader_load_page = true;

        /* Page navigation — only when book is open and a page is showing */
        if (rs == READER_READY) {
            int total = reader_page_count();
            if (kdown & (KEY_R | KEY_DRIGHT)) {
                if (total <= 0 || state->reader_page < total - 1) {
                    state->reader_page++;
                    state->reader_load_page = true;
                    if (state->reader_split) state->reader_pan_y = 0.0f;
                }
            }
            if (kdown & (KEY_L | KEY_DLEFT)) {
                if (state->reader_page > 0) {
                    state->reader_page--;
                    state->reader_load_page = true;
                    if (state->reader_split) state->reader_pan_y = 0.0f;
                }
            }
        }

        /* START: toggle dual-screen split mode */
        if (kdown & KEY_START) {
            state->reader_split = !state->reader_split;
            if (state->reader_split && reader_page_ready()) {
                /* Auto-zoom so the full page height fits across both 240px screens */
                int pw = reader_page_width(), ph = reader_page_height();
                if (pw > 0 && ph > 0) {
                    float base_sc = 400.0f / (float)pw;
                    float full_h  = (float)ph * base_sc;
                    state->reader_zoom = (full_h > 0.0f) ? (480.0f / full_h) : 1.0f;
                    if (state->reader_zoom < 0.05f) state->reader_zoom = 0.05f;
                    if (state->reader_zoom > 3.0f)  state->reader_zoom = 3.0f;
                }
                state->reader_pan_x = 0.0f;
                state->reader_pan_y = 0.0f;
            } else if (!state->reader_split) {
                state->reader_zoom  = 1.0f;
                state->reader_pan_x = 0.0f;
                state->reader_pan_y = 0.0f;
            }
        }

        /* Circle pad: horizontal + vertical pan in both modes.
         * Push right → image shifts left. Push up → image shifts up. */
        if (rs == READER_READY && reader_page_ready()) {
            circlePosition circle;
            hidCircleRead(&circle);
            const float dead = 0.15f;
            float cx = (float)circle.dx / 155.0f;
            float cy = (float)circle.dy / 155.0f;
            if (cx >  dead) state->reader_pan_x -= 3.0f;
            else if (cx < -dead) state->reader_pan_x += 3.0f;
            if (cy >  dead) state->reader_pan_y += 3.0f;
            else if (cy < -dead) state->reader_pan_y -= 3.0f;
        }

        /* D-pad Up/Down: zoom in/out in both normal and split mode */
        if (rs == READER_READY && reader_page_ready()) {
            if (kdown & KEY_DUP) {
                state->reader_zoom *= 1.1f;
                if (state->reader_zoom > 5.0f) state->reader_zoom = 5.0f;
            }
            if (kdown & KEY_DDOWN) {
                state->reader_zoom *= 0.9f;
                if (state->reader_zoom < 0.2f) state->reader_zoom = 0.2f;
            }
        }

        /* SELECT: toggle portrait/landscape — re-extract page with new rotation */
        if (kdown & KEY_SELECT) {
            state->reader_rotated   = !state->reader_rotated;
            state->reader_load_page = true;
        }

        /* B: back (cancel download if in progress, exit split mode first if active) */
        if (kdown & KEY_B) {
            if (state->reader_split) {
                state->reader_split = false;
                state->reader_zoom  = 1.0f;
                state->reader_pan_x = 0.0f;
                state->reader_pan_y = 0.0f;
            } else {
                reader_cancel();
                state->current_view = state->previous_view;
            }
        }
        break;
    }

    case VIEW_SETTINGS:
        /* D-pad up/down: move cursor, skip separators */
        if (kdown & KEY_DUP)
            state->settings_index = settings_next_selectable(state->settings_index, -1);
        if (kdown & KEY_DDOWN)
            state->settings_index = settings_next_selectable(state->settings_index, 1);

        /* Scroll to keep cursor visible */
        if (state->settings_index < state->settings_scroll)
            state->settings_scroll = state->settings_index;
        if (state->settings_index >= state->settings_scroll + UI_MAX_VISIBLE_ITEMS)
            state->settings_scroll = state->settings_index - UI_MAX_VISIBLE_ITEMS + 1;

        /* L/R: cycle selector values */
        if ((kdown & KEY_DLEFT) || (kdown & KEY_DRIGHT) ||
            (kdown & KEY_L) || (kdown & KEY_R)) {
            int dir = ((kdown & KEY_DRIGHT) || (kdown & KEY_R)) ? 1 : -1;
            if (state->settings_index == SET_AUDIO_BITRATE) {
                int cur = 0;
                for (int i = 0; i < AUDIO_RATES_COUNT; i++)
                    if (g_config.audio_bitrate == audio_rates[i]) { cur = i; break; }
                cur = (cur + dir + AUDIO_RATES_COUNT) % AUDIO_RATES_COUNT;
                g_config.audio_bitrate = audio_rates[cur];
            } else if (state->settings_index == SET_VIDEO_BITRATE) {
                int cur = 0;
                for (int i = 0; i < VIDEO_RATES_COUNT; i++)
                    if (g_config.video_bitrate == video_rates[i]) { cur = i; break; }
                cur = (cur + dir + VIDEO_RATES_COUNT) % VIDEO_RATES_COUNT;
                g_config.video_bitrate = video_rates[cur];
            } else if (state->settings_index == SET_BG_THEME) {
                g_config.bg_theme = (g_config.bg_theme + dir + 4) % 4;
            }
        }

        /* A: toggle booleans, activate actions */
        if (kdown & KEY_A) {
            if (state->settings_index == SET_AUTO_ADVANCE) {
                g_config.auto_advance = !g_config.auto_advance;
                state->auto_advance = g_config.auto_advance;
            } else if (state->settings_index == SET_BG_THEME) {
                g_config.bg_theme = (g_config.bg_theme + 1) % 4;
            } else if (state->settings_index == SET_DOWNLOADS) {
                state->downloads_scroll = 0;
                state->downloads_index  = 0;
                state->downloads_loaded = false;
                state->current_view = VIEW_DOWNLOADS;
            } else if (state->settings_index == SET_LOGOUT && session->access_token[0] != '\0') {
                jfin_session_t *s = (jfin_session_t *)session;
                jfin_logout(s);
                g_config.access_token[0] = '\0';
                config_save(&g_config);
                state->current_view = VIEW_LOGIN;
                if (g_config.server_url[0] != '\0')
                    snprintf(state->server_url, sizeof(state->server_url),
                             "%s", g_config.server_url);
            }
        }

        /* Y: jump to now playing (if something is playing) */
        if ((kdown & KEY_Y) && state->has_now_playing) {
            state->previous_view = state->current_view;
            state->current_view = VIEW_NOW_PLAYING;
        }

        /* B: save and return — go to libraries if logged in, login screen if not */
        if (kdown & KEY_B) {
            config_save(&g_config);
            if (session->access_token[0] == '\0') {
                /* Not authenticated — no network, just return to login */
                state->current_view = VIEW_LOGIN;
            } else {
                state->current_view = VIEW_LIBRARIES;
                jfin_get_views(session, &state->items);
                state->selected_index = 0;
                state->scroll_offset = 0;
            }
        }
        break;

    case VIEW_DOWNLOADS:
        {
            /* Auto-refresh when a queued download finishes */
            static dl_state_t s_prev_dl = DL_IDLE;
            dl_state_t s_cur_dl = dl_get_state();
            if (s_prev_dl == DL_ACTIVE && s_cur_dl != DL_ACTIVE)
                state->downloads_loaded = false;
            s_prev_dl = s_cur_dl;
        }
        if (!state->downloads_loaded) {
            dl_manager_scan();
            state->downloads_loaded = true;
            if (state->downloads_index >= dl_manager_count())
                state->downloads_index = 0;
            state->downloads_queue_focus = false;
            state->downloads_queue_index = 0;
        }

        /* ── Circle pad: queue navigation (top screen) ─────────────── */
        {
            /* Index 0 = active download (when DL_ACTIVE), 1+ = queue items */
            int has_active = (dl_get_state() == DL_ACTIVE) ? 1 : 0;
            int total = has_active + dl_queue_count();
            if (kdown & KEY_CPAD_UP) {
                state->downloads_queue_focus = true;
                if (state->downloads_queue_index > 0)
                    state->downloads_queue_index--;
            }
            if (kdown & KEY_CPAD_DOWN) {
                state->downloads_queue_focus = true;
                if (state->downloads_queue_index < total - 1)
                    state->downloads_queue_index++;
            }
            /* Clamp if items shrank */
            if (total == 0)
                state->downloads_queue_index = 0;
            else if (state->downloads_queue_index >= total)
                state->downloads_queue_index = total - 1;
        }

        /* ── D-pad: downloaded file list — hold to repeat ───────────── */
        {
            static int dl_rep_frames = 0;

            bool nav_up    = (kdown & KEY_DUP)    != 0;
            bool nav_down  = (kdown & KEY_DDOWN)  != 0;
            bool nav_left  = (kdown & KEY_DLEFT)  != 0;
            bool nav_right = (kdown & KEY_DRIGHT) != 0;

            u32 any_fresh = kdown & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT);
            u32 any_held  = kheld & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT);

            if (any_fresh) {
                dl_rep_frames = 0;
            } else if (any_held) {
                dl_rep_frames++;
                if (dl_rep_frames >= 8) {
                    if (dl_rep_frames % 2 == 0) {
                        nav_up   = (kheld & KEY_DUP)   != 0;
                        nav_down = (kheld & KEY_DDOWN)  != 0;
                    }
                    if (dl_rep_frames % 3 == 0) {
                        nav_left  = (kheld & KEY_DLEFT)  != 0;
                        nav_right = (kheld & KEY_DRIGHT) != 0;
                    }
                }
            } else {
                dl_rep_frames = 0;
            }

            if (nav_up) {
                state->downloads_queue_focus = false;
                if (state->downloads_index > 0) {
                    state->downloads_index--;
                    state->downloads_name_offset = 0;
                    if (state->downloads_index < state->downloads_scroll)
                        state->downloads_scroll = state->downloads_index;
                }
            }
            if (nav_down) {
                state->downloads_queue_focus = false;
                if (state->downloads_index < dl_manager_count() - 1) {
                    state->downloads_index++;
                    state->downloads_name_offset = 0;
                    if (state->downloads_index >= state->downloads_scroll + UI_MAX_VISIBLE_ITEMS)
                        state->downloads_scroll = state->downloads_index - UI_MAX_VISIBLE_ITEMS + 1;
                }
            }
            if (nav_left) {
                state->downloads_queue_focus = false;
                if (state->downloads_name_offset > 0)
                    state->downloads_name_offset--;
            }
            if (nav_right) {
                state->downloads_queue_focus = false;
                int sel = state->downloads_index;
                if (sel < dl_manager_count()) {
                    int nlen = (int)strlen(s_dl_entries[sel].name);
                    if (state->downloads_name_offset + 1 < nlen)
                        state->downloads_name_offset++;
                }
            }
        }

        /* ── A: open file ───────────────────────────────────────────── */
        if (kdown & KEY_A && state->downloads_index < dl_manager_count()) {
            const dl_entry_t *e = dl_manager_get(state->downloads_index);
            if (e) {
                if (e->dl_type == 2) {
                    /* Audio download: play via audio player (local MP3) */
                    video_player_stop();
                    audio_player_stop();
                    audio_player_play(e->path, 0, 0);
                    strncpy(state->now_playing.name, e->name, sizeof(state->now_playing.name)-1);
                    state->now_playing.name[sizeof(state->now_playing.name)-1] = '\0';
                    /* Clear video-specific fields so seek/X handlers don't use stale state */
                    state->now_playing.id[0] = '\0';
                    state->now_playing_local_path[0] = '\0';
                    state->now_playing_sub_path[0] = '\0';
                    state->video_retry_ticks = 0;
                    state->has_now_playing = true;
                    state->now_playing_offline = true;
                    state->previous_view = state->current_view;
                    state->current_view  = VIEW_NOW_PLAYING;
                } else if (!e->is_video) {
                    strncpy(state->now_playing.name, e->name, sizeof(state->now_playing.name)-1);
                    state->now_playing.name[sizeof(state->now_playing.name)-1] = '\0';
                    state->reader_page    = 0;
                    state->reader_load_page = false;
                    state->reader_rotated = false;
                    state->reader_split   = false;
                    state->reader_zoom    = 1.0f;
                    state->reader_pan_x   = 0.0f;
                    state->reader_pan_y   = 0.0f;
                    state->previous_view  = state->current_view;
                    state->current_view   = VIEW_READER;
                    reader_open_local(e->path);
                } else {
                    audio_player_stop();
                    /* Look up companion .ass subtitle file (downloaded alongside .ts) */
                    char loc_sub_path[192] = {0};
                    if (e->item_id[0]) {
                        char ass_path[192];
                        if (cache_path(e->item_id, "ass", ass_path, sizeof(ass_path))) {
                            FILE *afp = fopen(ass_path, "rb");
                            if (afp) { fclose(afp); snprintf(loc_sub_path, sizeof(loc_sub_path), "%s", ass_path); }
                        }
                    }
                    if (video_player_play(e->path, loc_sub_path[0] ? loc_sub_path : NULL, 0, 0, VP_3D_NONE)) {
                        strncpy(state->now_playing.name, e->name, sizeof(state->now_playing.name)-1);
                        state->now_playing.name[sizeof(state->now_playing.name)-1] = '\0';
                        state->has_now_playing = true;
                        state->now_playing_offline = true;
                        state->video_retry_ticks = 0;  /* reset tracked seek position */
                        snprintf(state->now_playing_local_path,
                                 sizeof(state->now_playing_local_path), "%s", e->path);
                        snprintf(state->now_playing_sub_path,
                                 sizeof(state->now_playing_sub_path), "%s", loc_sub_path);
                        state->offline_subs_on = (loc_sub_path[0] != '\0');
                        /* Use item_id stored in dl_entry_t (new cache naming) */
                        state->now_playing.id[0] = '\0';
                        if (e->item_id[0])
                            strncpy(state->now_playing.id, e->item_id,
                                    sizeof(state->now_playing.id) - 1);
                        state->now_playing.index_number = e->ep_num;
                        state->previous_view = state->current_view;
                        state->current_view  = VIEW_NOW_PLAYING;
                    }
                }
            }
        }

        /* ── X: cancel active (index 0) or remove queued item ──────── */
        if (kdown & KEY_X) {
            if (state->downloads_queue_focus) {
                bool has_active = (dl_get_state() == DL_ACTIVE);
                if (has_active && state->downloads_queue_index == 0) {
                    dl_cancel();
                    state->downloads_queue_focus = false;
                    state->downloads_queue_index = 0;
                } else {
                    int qi = state->downloads_queue_index - (has_active ? 1 : 0);
                    int qcnt = dl_queue_count();
                    if (qi >= 0 && qi < qcnt) {
                        dl_queue_remove(qi);
                        int has_active2 = (dl_get_state() == DL_ACTIVE) ? 1 : 0;
                        int total = has_active2 + dl_queue_count();
                        if (state->downloads_queue_index >= total && total > 0)
                            state->downloads_queue_index = total - 1;
                    }
                }
            } else if (state->downloads_index < dl_manager_count()) {
                dl_manager_delete(state->downloads_index);
                dl_manager_scan();
                state->downloads_name_offset = 0;
                int cnt = dl_manager_count();
                if (state->downloads_index >= cnt && cnt > 0)
                    state->downloads_index = cnt - 1;
            }
        }

        /* Y: return to now-playing if something is playing */
        if ((kdown & KEY_Y) && state->has_now_playing) {
            state->previous_view = state->current_view;
            state->current_view = VIEW_NOW_PLAYING;
        }
        if (kdown & KEY_B) {
            if (state->downloads_queue_focus) {
                state->downloads_queue_focus = false;
                state->downloads_queue_index = 0;
            } else {
                state->current_view = VIEW_SETTINGS;
            }
        }
        break;
    }
}

/* ── Navigation ────────────────────────────────────────────────────── */

void ui_navigate_into(ui_state_t *state, const jfin_session_t *session,
                      const jfin_item_t *item)
{
    /* Push breadcrumb first, then fetch. If empty, pop it back. */
    char saved_id[JFIN_MAX_ID];
    char saved_name[JFIN_MAX_NAME];
    snprintf(saved_id, sizeof(saved_id), "%s", item->id);
    snprintf(saved_name, sizeof(saved_name), "%s", item->name);

    if (state->parent_depth < 8) {
        snprintf(state->parent_stack_ids[state->parent_depth],
                 sizeof(state->parent_stack_ids[0]), "%s", saved_id);
        snprintf(state->parent_stack_names[state->parent_depth],
                 sizeof(state->parent_stack_names[0]), "%s", saved_name);
        state->parent_depth++;
    }

    state->current_view = VIEW_BROWSE;
    state->selected_index = 0;
    state->scroll_offset = 0;

    /* Show loading indicator before blocking API call */
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TextBufClear(s_text_buf);
    C2D_TargetClear(s_bottom, bg_color());
    C2D_SceneBegin(s_bottom);
    draw_text(115, 110, 0.6f, rgba(COLOR_PRIMARY), "Loading...");
    C3D_FrameEnd(0);

    /* Fetch into state->items directly (no stack-heavy temp copy) */
    jfin_get_items(session, saved_id, 0, JFIN_MAX_ITEMS, &state->items);

    log_write("NAV: into '%s' id=%s depth=%d items=%d",
              saved_name, saved_id, state->parent_depth, state->items.count);

    if (state->items.count == 0) {
        /* Empty folder — undo navigation */
        log_write("NAV: empty, undoing");
        state->parent_depth--;
        ui_navigate_back(state, session);
    }
}

void ui_navigate_back(ui_state_t *state, const jfin_session_t *session)
{
    if (state->parent_depth <= 0) {
        /* Already at top — go to libraries */
        state->current_view = VIEW_LIBRARIES;
        jfin_get_views(session, &state->items);
        state->selected_index = 0;
        state->scroll_offset = 0;
        return;
    }

    state->parent_depth--;
    state->selected_index = 0;
    state->scroll_offset = 0;

    if (state->parent_depth == 0) {
        state->current_view = VIEW_LIBRARIES;
        jfin_get_views(session, &state->items);
    } else {
        const char *parent_id = state->parent_stack_ids[state->parent_depth - 1];
        jfin_get_items(session, parent_id, 0, JFIN_MAX_ITEMS, &state->items);
    }
}

/* ── Renderers ─────────────────────────────────────────────────────── */

void ui_render_login(const ui_state_t *state)
{
    /* Bottom screen: login form */
    C2D_TargetClear(s_bottom, bg_color());
    C2D_SceneBegin(s_bottom);

    draw_text(10, 10, 0.7f, rgba(COLOR_PRIMARY), "Connect to Jellyfin Server");

    const char *labels[] = {"Server URL:", "Username:", "Password:"};
    const char *values[] = {state->server_url, state->username, "********"};

    for (int i = 0; i < 3; i++) {
        float y = 50 + i * 50;
        u32 bg = (i == state->login_field) ? rgba(COLOR_HIGHLIGHT) : rgba(COLOR_BG_CARD);
        draw_rect(10, y, 300, 40, bg);
        draw_text(15, y + 2, 0.45f, rgba(COLOR_TEXT_SECONDARY), labels[i]);
        draw_text(15, y + 18, 0.5f, rgba(COLOR_TEXT_PRIMARY),
                  values[i][0] ? values[i] : "(tap A to enter)");
    }

    draw_text(10, 210, 0.45f, rgba(COLOR_TEXT_SECONDARY),
              "A: Edit field  R: Connect  SEL: Settings");
}

void ui_render_libraries(const ui_state_t *state)
{
    /* Bottom screen: library list */
    C2D_TargetClear(s_bottom, bg_color());
    C2D_SceneBegin(s_bottom);

    draw_text(10, 5, 0.55f, rgba(COLOR_PRIMARY), "Libraries");

    for (int i = 0; i < state->items.count && i < UI_MAX_VISIBLE_ITEMS; i++) {
        int idx = state->scroll_offset + i;
        if (idx >= state->items.count) break;

        float y = 30 + i * UI_LIST_ITEM_HEIGHT;
        draw_list_item_bg(y, 310, UI_LIST_ITEM_HEIGHT - 4, idx == state->selected_index);

        draw_text(15, y + 10, 0.55f, rgba(COLOR_TEXT_PRIMARY),
                  state->items.items[idx].name);
    }

    draw_text(10, 220, 0.4f, rgba(COLOR_TEXT_SECONDARY),
              "A:Enter B:Back Y:Now Playing SEL:Settings");
}

static bool item_has_download(const char *item_id, bool is_video)
{
    return cache_has(item_id, is_video ? "ts" : "cbz");
}

void ui_render_browse(const ui_state_t *state)
{
    /* Bottom screen: item list */
    C2D_TargetClear(s_bottom, bg_color());
    C2D_SceneBegin(s_bottom);

    /* Breadcrumb */
    if (state->parent_depth > 0) {
        draw_text(10, 5, 0.45f, rgba(COLOR_TEXT_SECONDARY),
                  state->parent_stack_names[state->parent_depth - 1]);
    }

    for (int i = 0; i < UI_MAX_VISIBLE_ITEMS; i++) {
        int idx = state->scroll_offset + i;
        if (idx >= state->items.count) break;

        float y = 25 + i * UI_LIST_ITEM_HEIGHT;
        const jfin_item_t *item = &state->items.items[idx];

        draw_list_item_bg(y, 310, UI_LIST_ITEM_HEIGHT - 4, idx == state->selected_index);

        /* Item name */
        bool is_vid_item = (item->type == JFIN_ITEM_MOVIE || item->type == JFIN_ITEM_EPISODE);
        bool is_book_item = (item->type == JFIN_ITEM_BOOK);
        bool has_dl = ((is_vid_item || is_book_item) &&
                       item_has_download(item->id, is_vid_item));
        const char *dl_badge = has_dl ? " [D]" : "";
        char label[160];
        if (item->type == JFIN_ITEM_AUDIO && item->index_number > 0) {
            snprintf(label, sizeof(label), "%d. %s%s", item->index_number, item->name, dl_badge);
        } else if (item->type == JFIN_ITEM_EPISODE && item->index_number > 0) {
            snprintf(label, sizeof(label), "E%d - %s%s", item->index_number, item->name, dl_badge);
        } else {
            snprintf(label, sizeof(label), "%s%s", item->name, dl_badge);
        }
        draw_text(15, y + 4, 0.5f, rgba(COLOR_TEXT_PRIMARY), label);

        /* Subtitle: artist/year/duration */
        char sub[128] = {0};
        if (item->artist[0]) {
            snprintf(sub, sizeof(sub), "%s", item->artist);
        } else if (item->year > 0) {
            snprintf(sub, sizeof(sub), "%d", item->year);
        }
        if (item->runtime_ticks > 0) {
            char dur[16];
            format_ticks(item->runtime_ticks, dur, sizeof(dur));
            if (sub[0]) {
                strncat(sub, " - ", sizeof(sub) - strlen(sub) - 1);
                strncat(sub, dur, sizeof(sub) - strlen(sub) - 1);
            } else {
                snprintf(sub, sizeof(sub), "%s", dur);
            }
        }
        if (sub[0])
            draw_text(15, y + 22, 0.4f, rgba(COLOR_TEXT_SECONDARY), sub);
    }

    /* Scroll + page indicator */
    if (state->items.total_count > 0) {
        char info[48];
        if (state->items.total_count > JFIN_MAX_ITEMS) {
            int page = (state->items.start_index / JFIN_MAX_ITEMS) + 1;
            int total_pages = (state->items.total_count + JFIN_MAX_ITEMS - 1) / JFIN_MAX_ITEMS;
            snprintf(info, sizeof(info), "pg %d/%d (%d total)", page, total_pages, state->items.total_count);
        } else {
            snprintf(info, sizeof(info), "%d/%d",
                     state->selected_index + 1, state->items.count);
        }
        draw_text(220, 220, 0.4f, rgba(COLOR_TEXT_SECONDARY), info);
    }

    {
        bool sel_dl = false;
        if (state->selected_index < state->items.count) {
            jfin_item_type_t t = state->items.items[state->selected_index].type;
            sel_dl = (t == JFIN_ITEM_MOVIE || t == JFIN_ITEM_EPISODE ||
                      t == JFIN_ITEM_BOOK  || t == JFIN_ITEM_AUDIO   ||
                      t == JFIN_ITEM_MUSIC_ALBUM);
        }
        bool paginating = (state->items.total_count > JFIN_MAX_ITEMS);
        if (paginating && sel_dl)
            draw_text(10, 220, 0.4f, rgba(COLOR_TEXT_SECONDARY),
                      "A:Sel  X:DL  L/R:Pg  B:Back  ZR:Off");
        else if (paginating)
            draw_text(10, 220, 0.4f, rgba(COLOR_TEXT_SECONDARY),
                      "A:Sel  B:Back  L/R:Pg  SEL:Search  ZR:Off");
        else if (sel_dl)
            draw_text(10, 220, 0.4f, rgba(COLOR_TEXT_SECONDARY),
                      "A:Play  X:DL  ZL+X:+Sub  ZL+Y:Sub  ZL+A:Enc  B:Bk  ZR:Off");
        else
            draw_text(10, 220, 0.4f, rgba(COLOR_TEXT_SECONDARY),
                      "A:Select  B:Back  SEL:Search  ZR:Off");
    }
}

void ui_render_now_playing(const ui_state_t *state, const player_status_t *player)
{
    video_status_t vstatus = video_player_get_status();
    bool is_video = (vstatus.state != VIDEO_STOPPED);

    /* Top screen */
    C2D_TargetClear(s_top, bg_color());
    C2D_SceneBegin(s_top);

    if (!state->has_now_playing) {
        draw_text(120, 110, 0.7f, rgba(COLOR_TEXT_SECONDARY), "Nothing playing");
        return;
    }

    if (is_video && vstatus.state == VIDEO_ERROR) {
        int max_retries = (state->subtitle_stream_index >= 0) ? 5 : 3;
        bool retrying = !state->now_playing_offline
                        && (strncmp(vstatus.error_msg, "Video not ready", 15) == 0)
                        && state->video_retry_count < max_retries
                        && state->video_retry_timer > 0;
        if (retrying) {
            int secs = (state->video_retry_timer + 59) / 60;
            char rbuf[48];
            snprintf(rbuf, sizeof(rbuf), "Retrying in %ds (%d/%d)...", secs,
                     state->video_retry_count + 1, max_retries);
            draw_text(80, 70, 0.65f, rgba(COLOR_PRIMARY), "Video not ready");
            draw_text(40, 108, 0.5f, rgba(COLOR_TEXT_PRIMARY), rbuf);
            draw_text(30, 143, 0.5f, rgba(COLOR_TEXT_SECONDARY), state->now_playing.name);
            draw_text(35, 178, 0.4f, rgba(COLOR_TEXT_SECONDARY),
                      "Will retry without subs if it keeps failing");
            draw_text(60, 200, 0.45f, rgba(COLOR_TEXT_SECONDARY), "Press any button to cancel");
        } else {
            draw_text(50, 60, 0.7f, rgba(0xFF4444FF), "Playback Error");
            draw_text(30, 100, 0.5f, rgba(COLOR_TEXT_PRIMARY),
                      vstatus.error_msg[0] ? vstatus.error_msg : "Cannot play this content");
            draw_text(30, 140, 0.5f, rgba(COLOR_TEXT_SECONDARY),
                      state->now_playing.name);
            draw_text(60, 190, 0.45f, rgba(COLOR_TEXT_SECONDARY),
                      "Press any button to go back");
        }
    } else if (is_video && vstatus.state == VIDEO_LOADING) {
        /* Show buffering indicator while video is loading */
        draw_text(130, 100, 0.7f, rgba(COLOR_PRIMARY), "Buffering...");
        draw_text(80, 135, 0.45f, rgba(COLOR_TEXT_SECONDARY),
                  state->now_playing.name);
    } else if (is_video) {
        /* Render video frame on top screen */
        video_player_render_frame();
        draw_subtitles_top();

        /* Right eye for stereoscopic 3D (only when 3D slider is up) */
        if (vstatus.is_3d && osGet3DSliderState() > 0.0f) {
            C2D_TargetClear(s_top_right, rgba(0x000000FF));
            C2D_SceneBegin(s_top_right);
            video_player_render_frame_right();
            draw_subtitles_top();
        }
    } else if (player->state == PLAYER_LOADING) {
        /* Show buffering indicator while audio is loading */
        draw_text(130, 100, 0.7f, rgba(COLOR_PRIMARY), "Buffering...");
        draw_text(80, 135, 0.45f, rgba(COLOR_TEXT_SECONDARY),
                  state->now_playing.name);
    } else {
        /* Audio-only: show track info */
        const jfin_item_t *item = &state->now_playing;

        /* Album art or placeholder */
        draw_rect(125, 20, 150, 150, rgba(COLOR_BG_CARD));
        if (album_art_is_loaded())
            album_art_draw(125, 20, 150);
        else
            draw_text(165, 85, 0.6f, rgba(COLOR_TEXT_SECONDARY), "ART");

        draw_text(50, 180, 0.6f, rgba(COLOR_TEXT_PRIMARY), item->name);

        if (item->artist[0])
            draw_text(50, 200, 0.45f, rgba(COLOR_ACCENT), item->artist);

        if (item->album[0])
            draw_text(50, 215, 0.4f, rgba(COLOR_TEXT_SECONDARY), item->album);
    }

    /* Bottom screen: black when hidden (night mode), controls otherwise */
    C2D_TargetClear(s_bottom, rgba(0x000000FF));
    C2D_SceneBegin(s_bottom);

    if (state->bottom_hidden)
        return; /* black bottom screen — just the clear above */

    /* Use video position/state if video is playing, otherwise audio */
    int64_t pos_ticks, dur_ticks;
    int buf_pct;
    const char *state_str = "STOPPED";

    if (is_video) {
        pos_ticks = vstatus.position_ticks;
        dur_ticks = vstatus.duration_ticks;
        buf_pct = vstatus.buffer_percent;
        switch (vstatus.state) {
        case VIDEO_LOADING:  state_str = "BUFFERING..."; break;
        case VIDEO_PLAYING:  state_str = "PLAYING"; break;
        case VIDEO_PAUSED:   state_str = "PAUSED"; break;
        case VIDEO_ERROR:    state_str = vstatus.error_msg; break;
        default: break;
        }
    } else {
        pos_ticks = player->position_ticks;
        dur_ticks = player->duration_ticks;
        buf_pct = player->buffer_percent;
        switch (player->state) {
        case PLAYER_LOADING:  state_str = "BUFFERING..."; break;
        case PLAYER_PLAYING:  state_str = "PLAYING"; break;
        case PLAYER_PAUSED:   state_str = "PAUSED"; break;
        case PLAYER_ERROR:    state_str = player->error_msg; break;
        default: break;
        }
    }

    /* Progress bar */
    float progress = 0.0f;
    if (dur_ticks > 0)
        progress = (float)pos_ticks / (float)dur_ticks;
    if (progress > 1.0f) progress = 1.0f;

    draw_rect(20, 40, 280, 6, rgba(COLOR_BG_CARD));
    draw_rect(20, 40, 280 * progress, 6, rgba(COLOR_PRIMARY));

    /* Time labels */
    char pos_str[16], dur_str[16];
    format_ticks(pos_ticks, pos_str, sizeof(pos_str));
    format_ticks(dur_ticks, dur_str, sizeof(dur_str));
    draw_text(20, 50, 0.4f, rgba(COLOR_TEXT_SECONDARY), pos_str);
    draw_text(270, 50, 0.4f, rgba(COLOR_TEXT_SECONDARY), dur_str);

    draw_text(110, 80, 0.55f, rgba(COLOR_PRIMARY), state_str);

    /* Buffer indicator */
    char buf_str[64];
    snprintf(buf_str, sizeof(buf_str), "Buffer: %d%%", buf_pct);
    draw_text(115, 100, 0.4f, rgba(COLOR_TEXT_SECONDARY), buf_str);

    /* Diagnostics for video playback */
    if (is_video) {
        char diag[80];
        snprintf(diag, sizeof(diag), "%sDec: %.0f fps  Disp: %.0f fps  %dx%d",
                 vstatus.is_3d ? "3D  " : "",
                 vstatus.decode_fps, vstatus.display_fps,
                 vstatus.video_width, vstatus.video_height);
        draw_text(30, 125, 0.38f, rgba(COLOR_TEXT_SECONDARY), diag);
    }

    /* Subtitle status */
    if (is_video) {
        char sub_str[64];
        bool subs_active;
        if (state->now_playing_offline) {
            subs_active = state->offline_subs_on;
            snprintf(sub_str, sizeof(sub_str), "Subs: %s",
                     state->offline_subs_on ? "on" : "off");
        } else if (state->subtitle_stream_index >= 0) {
            subs_active = true;
            const char *label = state->subtitle_lang_pref[0]
                                ? state->subtitle_lang_pref : "on";
            for (int si = 0; si < state->subtitle_list.count; si++) {
                if (state->subtitle_list.subs[si].index == state->subtitle_stream_index) {
                    if (state->subtitle_list.subs[si].title[0])
                        label = state->subtitle_list.subs[si].title;
                    break;
                }
            }
            snprintf(sub_str, sizeof(sub_str), "Subs: %s%s",
                     label, state->subtitle_sticky ? " (sticky)" : "");
        } else {
            subs_active = false;
            snprintf(sub_str, sizeof(sub_str), "Subs: off");
        }
        draw_text(30, 145, 0.4f, rgba(subs_active ? COLOR_ACCENT : COLOR_TEXT_SECONDARY),
                  sub_str);
    }

    /* Shuffle/repeat status (audio only) */
    if (!is_video) {
        const char *rep_str = (state->repeat_mode == 1) ? "Rep:1  " :
                              (state->repeat_mode == 2) ? "Rep:All" : "Rep:Off";
        const char *shuf_str = state->shuffle_mode ? "Shuf:ON " : "Shuf:OFF";
        char mode_str[32];
        snprintf(mode_str, sizeof(mode_str), "%s  %s", shuf_str, rep_str);
        draw_text(60, 125, 0.42f,
                  rgba(state->shuffle_mode || state->repeat_mode ? COLOR_ACCENT : COLOR_TEXT_SECONDARY),
                  mode_str);
    }

    /* Toast notification */
    if (state->np_toast_timer > 0)
        draw_text(10, 163, 0.42f, rgba(COLOR_ACCENT), state->np_toast);

    /* Controls hint — two lines for video, three for video download row */
    if (!is_video) {
        draw_text(10, 180, 0.4f, rgba(COLOR_TEXT_PRIMARY),
                  "A:Pause B:Back STR:Stop ZL/ZR:Skip Y:Shuf SEL:Rep");
        draw_text(10, 193, 0.4f, rgba(COLOR_TEXT_SECONDARY),
                  "ZR+START:Off  ZR+SEL:Timer");
    } else {
        const char *line1 = state->now_playing_offline
            ? "A:Pause B:Back STR:Stop L/R:Seek Y:sub"
            : "A:Pause B:Back STR:Stop L/R:Seek Y:Subs";
        draw_text(10, 177, 0.4f, rgba(COLOR_TEXT_PRIMARY), line1);
        draw_text(10, 191, 0.4f, rgba(COLOR_TEXT_PRIMARY),
                  "X:DLnxt  ZL+X:DL+sub  ZL+Y:DLsub  ZL+A:DL+enc");
        draw_text(10, 205, 0.4f, rgba(COLOR_TEXT_SECONDARY),
                  "ZR+START:Off  ZR+SEL:Timer");
    }

    /* Shutdown timer countdown (bottom-right) */
    if (state->shutdown_timer_active) {
        u64 now = osGetTime();
        u64 rem = (state->shutdown_timer_deadline > now)
                  ? state->shutdown_timer_deadline - now : 0;
        int th = (int)(rem / 3600000);
        int tm = (int)((rem % 3600000) / 60000);
        int ts = (int)((rem % 60000)   / 1000);
        char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d", th, tm, ts);
        draw_text(218, 220, 0.45f, rgba(COLOR_ACCENT), tbuf);
    }

    /* Shutdown timer popup overlay */
    if (state->shutdown_popup_open) {
        C2D_DrawRectSolid(55, 72, 0.0f, 210, 96, rgba(0x1A1A2EFF));
        draw_text(98, 80, 0.5f, rgba(COLOR_PRIMARY), "Shutdown Timer");

        char hbuf[4], mbuf[4], sbuf[4];
        snprintf(hbuf, sizeof(hbuf), "%02d", state->shutdown_popup_h);
        snprintf(mbuf, sizeof(mbuf), "%02d", state->shutdown_popup_m);
        snprintf(sbuf, sizeof(sbuf), "%02d", state->shutdown_popup_s);

        u32 ch = state->shutdown_popup_sel == 0 ? rgba(COLOR_ACCENT) : rgba(COLOR_TEXT_PRIMARY);
        u32 cm = state->shutdown_popup_sel == 1 ? rgba(COLOR_ACCENT) : rgba(COLOR_TEXT_PRIMARY);
        u32 cs = state->shutdown_popup_sel == 2 ? rgba(COLOR_ACCENT) : rgba(COLOR_TEXT_PRIMARY);

        draw_text( 80, 108, 0.7f, ch, hbuf);
        draw_text(108, 108, 0.7f, rgba(COLOR_TEXT_SECONDARY), ":");
        draw_text(118, 108, 0.7f, cm, mbuf);
        draw_text(146, 108, 0.7f, rgba(COLOR_TEXT_SECONDARY), ":");
        draw_text(156, 108, 0.7f, cs, sbuf);

        draw_text(62, 148, 0.38f, rgba(COLOR_TEXT_SECONDARY),
                  "U/D:value  L/R:field  A:Start  B:Cancel");
    }
}

void ui_render_settings(const ui_state_t *state, const jfin_session_t *session)
{
    C2D_TargetClear(s_bottom, bg_color());
    C2D_SceneBegin(s_bottom);

    draw_text(10, 5, 0.55f, rgba(COLOR_PRIMARY), "Settings");

    for (int i = 0; i < UI_MAX_VISIBLE_ITEMS + 1 && i + state->settings_scroll < SET_COUNT; i++) {
        int idx = state->settings_scroll + i;
        float y = 30 + i * UI_LIST_ITEM_HEIGHT;

        if (settings_is_separator(idx)) {
            /* Separator: thin line + label */
            float line_y = y + UI_LIST_ITEM_HEIGHT / 2;
            draw_rect(10, line_y, 300, 1, rgba(COLOR_SEPARATOR));
            const char *sep_label = (idx == SET_SEPARATOR_ACCOUNT) ? "Account" : "About";
            draw_text(15, line_y + 4, 0.4f, rgba(COLOR_TEXT_SECONDARY), sep_label);
            continue;
        }

        bool selected = (idx == state->settings_index);
        draw_list_item_bg(y, 310, UI_LIST_ITEM_HEIGHT - 4, selected);

        /* Label + value per item */
        const char *label = "";
        char value[128] = {0};
        u32 value_color = rgba(COLOR_VALUE);

        switch (idx) {
        case SET_AUDIO_BITRATE:
            label = "Audio Bitrate";
            snprintf(value, sizeof(value), "%d kbps", g_config.audio_bitrate);
            break;
        case SET_VIDEO_BITRATE:
            label = "Video Bitrate";
            snprintf(value, sizeof(value), "%d kbps", g_config.video_bitrate);
            break;
        case SET_AUTO_ADVANCE:
            label = "Auto-advance";
            snprintf(value, sizeof(value), "%s", g_config.auto_advance ? "On" : "Off");
            break;
        case SET_BG_THEME: {
            label = "Background";
            const char *themes[] = { "Dark", "Black", "White", "Grey" };
            int th = (g_config.bg_theme >= 0 && g_config.bg_theme < 4) ? g_config.bg_theme : 0;
            snprintf(value, sizeof(value), "%s", themes[th]);
            break;
        }
        case SET_DOWNLOADS:
            label = "Manage Downloads";
            snprintf(value, sizeof(value), "A: Open");
            break;
        case SET_SERVER:
            label = "Server";
            snprintf(value, sizeof(value), "%.30s%s",
                     session->server_url,
                     strlen(session->server_url) > 30 ? "..." : "");
            value_color = rgba(COLOR_TEXT_SECONDARY);
            break;
        case SET_USERNAME:
            label = "User";
            snprintf(value, sizeof(value), "%s", g_config.username);
            value_color = rgba(COLOR_TEXT_SECONDARY);
            break;
        case SET_LOGOUT:
            label = "Logout";
            if (session->access_token[0] != '\0') {
                value_color = rgba(COLOR_DANGER);
            } else {
                snprintf(value, sizeof(value), "(not logged in)");
                value_color = rgba(COLOR_TEXT_SECONDARY);
            }
            break;
        case SET_VERSION:
            label = "Version";
            snprintf(value, sizeof(value), "v" JFIN_VERSION);
            value_color = rgba(COLOR_TEXT_SECONDARY);
            break;
        case SET_DEVICE_ID:
            label = "Device";
            snprintf(value, sizeof(value), "%.18s%s",
                     g_config.device_id,
                     strlen(g_config.device_id) > 18 ? "..." : "");
            value_color = rgba(COLOR_TEXT_SECONDARY);
            break;
        }

        draw_text(15, y + 10, 0.5f,
                  (idx == SET_LOGOUT && session->access_token[0] != '\0')
                      ? rgba(COLOR_DANGER) : rgba(COLOR_TEXT_PRIMARY),
                  label);
        if (value[0])
            draw_text(200, y + 10, 0.45f, value_color, value);
    }

    if (state->has_now_playing) {
        draw_text(10, 213, 0.4f, rgba(COLOR_TEXT_SECONDARY),
                  "A:Toggle L/R:Change B:Back");
        draw_text(10, 228, 0.38f, rgba(COLOR_ACCENT), "Y:Now Playing");
    } else {
        draw_text(10, 220, 0.4f, rgba(COLOR_TEXT_SECONDARY),
                  "A:Toggle L/R:Change B:Back");
    }
}

/* ── Manga reader render ───────────────────────────────────────────── */

void ui_render_reader(const ui_state_t *state)
{
    reader_state_t rs = reader_get_state();

    /* Top screen: page image or status overlay */
    C2D_TargetClear(s_top, bg_color());
    C2D_SceneBegin(s_top);

    if (reader_page_ready()) {
        if (state->reader_split) {
            reader_draw_split_top(state->reader_zoom,
                                  state->reader_pan_x, state->reader_pan_y);
        } else {
            reader_draw(0, 0, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT,
                        state->reader_zoom, state->reader_pan_x, state->reader_pan_y);
        }
    } else if (rs == READER_DOWNLOADING) {
        size_t total = reader_dl_total();
        size_t bytes = reader_dl_bytes();
        draw_text(115, 75, 0.52f, rgba(COLOR_TEXT_SECONDARY),
                  "Downloading book...");
        char dl_str[48];
        if (total > 0)
            snprintf(dl_str, sizeof(dl_str), "%.1f / %.1f MB",
                     bytes / (1024.0f * 1024.0f),
                     total / (1024.0f * 1024.0f));
        else
            snprintf(dl_str, sizeof(dl_str), "%.1f MB downloaded",
                     bytes / (1024.0f * 1024.0f));
        draw_text(110, 105, 0.58f, rgba(COLOR_PRIMARY), dl_str);

        /* Progress bar (shown when Content-Length is known) */
        if (total > 0) {
            float pct = (float)bytes / (float)total;
            if (pct > 1.0f) pct = 1.0f;
            int bx = 60, by = 140, bw = 280, bh = 10;
            C2D_DrawRectSolid(bx, by, 0.5f, bw, bh, rgba(COLOR_BG_CARD));
            C2D_DrawRectSolid(bx, by, 0.5f, (int)(bw * pct), bh, rgba(COLOR_PRIMARY));
        }
        draw_text(140, 170, 0.42f, rgba(COLOR_TEXT_SECONDARY), "B: Cancel");
    } else if (rs == READER_ERROR) {
        draw_text(100, 80,  0.55f, rgba(0xFF5555FF), "Failed to open book");
        draw_text(55,  110, 0.42f, rgba(COLOR_TEXT_SECONDARY),
                  "Could not download from Jellyfin.");
        draw_text(55,  130, 0.42f, rgba(COLOR_TEXT_SECONDARY),
                  "Check server URL, auth, and debug.log");
        draw_text(125, 170, 0.48f, rgba(COLOR_TEXT_PRIMARY), "B: Back");
    } else {
        /* READER_READY but page not yet extracted */
        draw_text(135, 105, 0.55f, rgba(COLOR_TEXT_SECONDARY), "Loading page...");
    }

    if (state->reader_split && reader_page_ready()) {
        /* Split mode: bottom screen shows the lower half of the page */
        C2D_TargetClear(s_bottom, rgba(0x000000FF));
        C2D_SceneBegin(s_bottom);
        reader_draw_split_bottom(state->reader_zoom,
                                 state->reader_pan_x, state->reader_pan_y);
        /* Translucent hint strip at bottom edge */
        draw_rect(0, 228, 320, 12, rgba(0x00000099));
        draw_text(4, 229, 0.33f, rgba(COLOR_TEXT_SECONDARY),
                  "START:normal  Dpad:Zoom  Circle:Pan  L/R:page");
    } else {
        /* Normal mode: bottom screen shows controls */
        C2D_TargetClear(s_bottom, bg_color());
        C2D_SceneBegin(s_bottom);

        draw_text(10, 5, 0.45f, rgba(COLOR_TEXT_SECONDARY), state->now_playing.name);

        if (reader_page_ready()) {
            char pg_str[32];
            int total = reader_page_count();
            if (total > 0)
                snprintf(pg_str, sizeof(pg_str), "Page %d / %d",
                         state->reader_page + 1, total);
            else
                snprintf(pg_str, sizeof(pg_str), "Page %d", state->reader_page + 1);
            draw_text(90, 95, 0.6f, rgba(COLOR_PRIMARY), pg_str);

            /* Zoom level indicator when not at 1.0 */
            if (state->reader_zoom < 0.99f || state->reader_zoom > 1.01f) {
                char zoom_str[16];
                snprintf(zoom_str, sizeof(zoom_str), "Zoom: %.0f%%",
                         state->reader_zoom * 100.0f);
                draw_text(105, 125, 0.42f, rgba(COLOR_VALUE), zoom_str);
            }

            /* Orientation indicator */
            draw_text(85, 145, 0.42f,
                      state->reader_rotated ? rgba(COLOR_ACCENT) : rgba(COLOR_TEXT_SECONDARY),
                      state->reader_rotated ? "Landscape (SELECT: normal)"
                                            : "Portrait  (SELECT: landscape)");

            draw_text(5, 196, 0.38f, rgba(COLOR_TEXT_PRIMARY),
                      "L/R:Page  SEL:Rotate  START:Split  B:Back");
            draw_text(5, 213, 0.38f, rgba(COLOR_TEXT_SECONDARY),
                      "Dpad(U/D):Zoom  Circle:Pan");
        }
    }
}

/* ── Downloads manager render ──────────────────────────────────────── */

void ui_render_downloads(const ui_state_t *state)
{
    C2D_TargetClear(s_top, bg_color());
    C2D_SceneBegin(s_top);

    draw_text(10, 4, 0.52f, rgba(COLOR_PRIMARY), "Download Status");

    dl_state_t vds  = dl_get_state();
    int        qcnt = dl_queue_count();

    /* ── Speed / progress tracking (sampled ~every second) ────────── */
    static size_t     s_dl_prev_bytes = 0;
    static uint64_t   s_dl_prev_tick  = 0;
    static float      s_dl_speed      = 0.0f;
    static dl_state_t s_dl_prev_state = DL_IDLE;

    uint64_t dl_tick = svcGetSystemTick();
    size_t   dl_now  = (vds == DL_ACTIVE) ? dl_bytes() : 0;
    size_t   dl_tot  = (vds == DL_ACTIVE) ? dl_total() : 0;

    if (vds != s_dl_prev_state) {
        s_dl_prev_bytes = dl_now;
        s_dl_prev_tick  = dl_tick;
        s_dl_speed      = 0.0f;
        s_dl_prev_state = vds;
    }
    {
        uint64_t diff = dl_tick - s_dl_prev_tick;
        if (vds == DL_ACTIVE && diff >= 268000000ULL && dl_now >= s_dl_prev_bytes) {
            float dt_s      = (float)diff / 268000000.0f;
            float new_sp    = (float)(dl_now - s_dl_prev_bytes) / dt_s;
            s_dl_speed      = (s_dl_speed > 0.0f) ? s_dl_speed * 0.6f + new_sp * 0.4f : new_sp;
            s_dl_prev_bytes = dl_now;
            s_dl_prev_tick  = dl_tick;
        }
    }

    char spd_str[20] = "";
    if      (s_dl_speed >= 1048576.0f) snprintf(spd_str, sizeof(spd_str), "%.1fMB/s", s_dl_speed/1048576.0f);
    else if (s_dl_speed >= 1024.0f)    snprintf(spd_str, sizeof(spd_str), "%.0fKB/s", s_dl_speed/1024.0f);
    else if (s_dl_speed > 0.0f)        snprintf(spd_str, sizeof(spd_str), "%.0fB/s",  s_dl_speed);

    char eta_str[20] = "";
    if (s_dl_speed > 1024.0f && dl_tot > dl_now) {
        int secs = (int)((float)(dl_tot - dl_now) / s_dl_speed);
        if      (secs >= 3600) snprintf(eta_str, sizeof(eta_str), "~%dh%02dm", secs/3600, (secs%3600)/60);
        else if (secs >= 60)   snprintf(eta_str, sizeof(eta_str), "~%dm%02ds", secs/60, secs%60);
        else                   snprintf(eta_str, sizeof(eta_str), "~%ds",       secs);
    }

    int dl_pct = (dl_tot > 0) ? (int)((float)dl_now / (float)dl_tot * 100.0f) : -1;

    /* Active download */
    if (vds == DL_ACTIVE) {
        char name_buf[64];
        int retry = dl_retry_count();
        if (retry > 0)
            snprintf(name_buf, sizeof(name_buf), "%.40s [Attempt %d/3]", dl_item_name(), retry + 1);
        else
            snprintf(name_buf, sizeof(name_buf), "%.54s", dl_item_name());
        draw_text(10, 26, 0.42f, rgba(COLOR_ACCENT), name_buf);
        if (dl_sub_name()[0]) {
            char si[72];
            snprintf(si, sizeof(si), "Subs: %s", dl_sub_name());
            draw_text(10, 40, 0.34f, rgba(COLOR_VALUE), si);
        }
        if (dl_tot > 0) {
            int fill = (int)(380.0f * (float)dl_now / (float)dl_tot);
            draw_rect(10, 53, 380, 8, rgba(COLOR_BG_CARD));
            draw_rect(10, 53, fill, 8, rgba(COLOR_PRIMARY));
            char prog[64];
            snprintf(prog, sizeof(prog), "%.1f / %.1f MB  (%d%%)",
                     dl_now/1048576.0f, dl_tot/1048576.0f, dl_pct);
            draw_text(10, 64, 0.38f, rgba(COLOR_TEXT_SECONDARY), prog);
        } else {
            char prog[48];
            snprintf(prog, sizeof(prog), "%.1f MB downloaded", dl_now/1048576.0f);
            draw_text(10, 64, 0.38f, rgba(COLOR_TEXT_SECONDARY), prog);
        }
        if (spd_str[0]) {
            char spd_eta[44] = "";
            if (eta_str[0])
                snprintf(spd_eta, sizeof(spd_eta), "%s  %s", spd_str, eta_str);
            else
                snprintf(spd_eta, sizeof(spd_eta), "%s", spd_str);
            draw_text(10, 76, 0.38f, rgba(COLOR_ACCENT), spd_eta);
        }
    } else if (vds == DL_DONE) {
        draw_text(10, 26, 0.42f, rgba(0x88FF88FF), "Last download complete");
    } else if (vds == DL_ERROR) {
        int retry = dl_retry_count();
        char err_buf[56];
        snprintf(err_buf, sizeof(err_buf), "Download failed (after %d attempt%s)",
                 retry + 1, retry + 1 == 1 ? "" : "s");
        draw_text(10, 26, 0.42f, rgba(COLOR_DANGER), err_buf);
        draw_text(10, 42, 0.36f, rgba(COLOR_TEXT_SECONDARY), "Re-queue to resume from partial file");
    } else {
        draw_text(10, 26, 0.42f, rgba(COLOR_TEXT_SECONDARY), "No active download");
        draw_text(10, 42, 0.38f, rgba(COLOR_TEXT_SECONDARY), "Browse items and press X to queue");
    }

    /* Queue list: index 0 = active download, 1+ = queued items */
    int qy = 90;
    bool has_active = (vds == DL_ACTIVE);
    int active_off  = has_active ? 1 : 0;
    int total_slots = active_off + qcnt;
    if (total_slots > 0) {
        char qhdr[64];
        snprintf(qhdr, sizeof(qhdr), "Queue: %d item%s%s",
                 total_slots, total_slots == 1 ? "" : "s",
                 state->downloads_queue_focus ? "  [Circle]" : "");
        draw_text(10, qy, 0.44f,
                  state->downloads_queue_focus ? rgba(COLOR_ACCENT) : rgba(COLOR_TEXT_PRIMARY),
                  qhdr);
        qy += 18;
        int shown = 0;
        if (has_active && shown < 6) {
            bool qsel = state->downloads_queue_focus && (state->downloads_queue_index == 0);
            char ql[72];
            if (dl_pct >= 0)
                snprintf(ql, sizeof(ql), "%s1. [%d%%] %.43s", qsel ? "> " : "  ", dl_pct, dl_item_name());
            else
                snprintf(ql, sizeof(ql), "%s1. [DL] %.46s",   qsel ? "> " : "  ", dl_item_name());
            draw_text(10, qy, 0.38f,
                      qsel ? rgba(COLOR_ACCENT) : rgba(COLOR_TEXT_SECONDARY), ql);
            qy += 15;
            shown++;
        }
        for (int qi = 0; qi < qcnt && shown < 6; qi++) {
            int slot = qi + active_off;
            bool qsel = state->downloads_queue_focus && (slot == state->downloads_queue_index);
            char ql[72];
            snprintf(ql, sizeof(ql), "%s%d. %.50s",
                     qsel ? "> " : "  ", slot + 1, dl_queue_item_name(qi));
            draw_text(10, qy, 0.38f,
                      qsel ? rgba(COLOR_TEXT_PRIMARY) : rgba(COLOR_TEXT_SECONDARY), ql);
            qy += 15;
            shown++;
        }
        if (total_slots > 6)
            draw_text(10, qy, 0.36f, rgba(COLOR_TEXT_SECONDARY), "  ...");
    } else {
        draw_text(10, qy, 0.40f, rgba(COLOR_TEXT_SECONDARY), "Queue empty");
    }

    /* Hint at bottom of top screen */
    if (state->downloads_queue_focus)
        draw_text(10, 225, 0.36f, rgba(COLOR_TEXT_SECONDARY),
                  "Circle:Nav  X:Cancel/Remove  B:Exit");
    else if (total_slots > 0)
        draw_text(10, 225, 0.36f,
                  state->has_now_playing ? rgba(COLOR_ACCENT) : rgba(COLOR_TEXT_SECONDARY),
                  "Circle:Select  Y:Now Playing");

    C2D_TargetClear(s_bottom, bg_color());
    C2D_SceneBegin(s_bottom);

    draw_text(10, 5, 0.55f, rgba(COLOR_PRIMARY), "Downloads");

    int cnt = dl_manager_count();
    if (cnt == 0) {
        draw_text(40, 100, 0.48f, rgba(COLOR_TEXT_SECONDARY), "No downloaded files");
        draw_text(20, 130, 0.42f, rgba(COLOR_TEXT_SECONDARY), "Browse: A=play, X=queue download");
        draw_text(10, 220, 0.4f, rgba(COLOR_TEXT_SECONDARY), "B:Back");
        return;
    }

    /* Scrollbar */
    if (cnt > UI_MAX_VISIBLE_ITEMS) {
        int bar_h = 190;
        int bar_y = 30;
        int thumb_h = bar_h * UI_MAX_VISIBLE_ITEMS / cnt;
        if (thumb_h < 8) thumb_h = 8;
        int thumb_y = bar_y + (bar_h - thumb_h) * state->downloads_scroll
                              / (cnt - UI_MAX_VISIBLE_ITEMS);
        draw_rect(308, bar_y, 4, bar_h, rgba(COLOR_BG_CARD));
        draw_rect(308, thumb_y, 4, thumb_h, rgba(COLOR_PRIMARY));
    }

    for (int i = 0; i < UI_MAX_VISIBLE_ITEMS + 1 && i + state->downloads_scroll < cnt; i++) {
        int idx = state->downloads_scroll + i;
        float y = 30 + i * UI_LIST_ITEM_HEIGHT;
        bool sel = (idx == state->downloads_index);

        /* Draw thin separator at the start of a new type/series group */
        if (idx > 0) {
            bool new_type   = s_dl_entries[idx].dl_type != s_dl_entries[idx-1].dl_type;
            bool new_series = strcmp(s_dl_entries[idx].series, s_dl_entries[idx-1].series) != 0;
            if (new_type || new_series)
                draw_rect(14, (int)y, 288, 1, rgba(COLOR_ACCENT));
        }

        draw_list_item_bg(y, 306, UI_LIST_ITEM_HEIGHT - 4, sel);

        /* For selected item: scroll name if offset > 0 or name is long */
        char disp[40];
        const char *full_name = s_dl_entries[idx].name;
        int nlen = (int)strlen(full_name);
        if (sel && state->downloads_name_offset > 0 && state->downloads_name_offset < nlen) {
            const char *shifted = full_name + state->downloads_name_offset;
            int remain = nlen - state->downloads_name_offset;
            snprintf(disp, sizeof(disp), "%.*s%s", remain > 36 ? 36 : remain, shifted,
                     remain > 36 ? ">" : "");
        } else {
            snprintf(disp, sizeof(disp), "%.36s%s", full_name, nlen > 36 ? ">" : "");
        }

        /* Type-based accent color: VID=blue, AUD=purple, CBZ=red */
        u32 type_col = (s_dl_entries[idx].dl_type == 0) ? 0x5599FFFF   /* VID blue   */
                     : (s_dl_entries[idx].dl_type == 2) ? 0xBB66EEFF   /* AUD purple */
                     :                                    0xFF7777FF;   /* CBZ red    */
        draw_text(14, y + 4, 0.45f, rgba(type_col), disp);

        char size_str[20];
        float sz_mb = s_dl_entries[idx].sz / (1024.0f * 1024.0f);
        if (sz_mb >= 1.0f)
            snprintf(size_str, sizeof(size_str), "%.1fMB", sz_mb);
        else
            snprintf(size_str, sizeof(size_str), "%zuKB", s_dl_entries[idx].sz / 1024);

        const char *type = s_dl_entries[idx].dl_type == 0 ? "VID"
                         : s_dl_entries[idx].dl_type == 2 ? "AUD" : "CBZ";
        char meta[32];
        snprintf(meta, sizeof(meta), "%s %s", type, size_str);
        draw_text(218, y + 4, 0.4f, rgba(type_col), meta);
    }

    draw_text(10, 220, 0.4f, rgba(COLOR_TEXT_SECONDARY),
              state->downloads_queue_focus
                  ? "A:Open  X:Cancel/Del  B:Exit queue"
                  : "A:Open  X:Delete  Y:Now Playing  B:Back");
}

/* ── Main render dispatch ──────────────────────────────────────────── */

void ui_render(const ui_state_t *state, const jfin_session_t *session,
               const player_status_t *player)
{
    /* Enable stereoscopic 3D only while a 3D frame is actually being
     * drawn (PLAYING/PAUSED) AND the slider is up. The right-eye render
     * is slider-gated, so without the slider term here a slider at 0
     * would present a stale right framebuffer; with it, slider 0 falls
     * back to clean 2D (left eye full-res) per the design doc. */
    video_status_t vs_3d = video_player_get_status();
    gfxSet3D(state->current_view == VIEW_NOW_PLAYING && vs_3d.is_3d &&
             (vs_3d.state == VIDEO_PLAYING || vs_3d.state == VIDEO_PAUSED) &&
             osGet3DSliderState() > 0.0f);

    C2D_TextBufClear(s_text_buf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

    /* Top screen: branding, now-playing, or view-specific full render */
    if (state->current_view == VIEW_NOW_PLAYING) {
        ui_render_now_playing(state, player);
    } else if (state->current_view == VIEW_READER) {
        ui_render_reader(state);
    } else if (state->current_view == VIEW_DOWNLOADS) {
        ui_render_downloads(state);
    } else {
        C2D_TargetClear(s_top, bg_color());
        C2D_SceneBegin(s_top);

        draw_text(118, 80, 1.0f, rgba(COLOR_PRIMARY), "Jellyfin 3DS");
        draw_text(179, 118, 0.48f, rgba(COLOR_TEXT_SECONDARY), "v" JFIN_VERSION);
        draw_text(92, 145, 0.4f, rgba(COLOR_ACCENT),
                  "CBZ/Manga  \xe2\x80\xa2  Subtitles  \xe2\x80\xa2  Downloads");

        /* Show mini now-playing bar if something is playing */
        if (state->has_now_playing && player->state == PLAYER_PLAYING) {
            draw_rect(0, 210, 400, 30, rgba(COLOR_BG_CARD));
            draw_text(10, 215, 0.45f, rgba(COLOR_TEXT_PRIMARY),
                      state->now_playing.name);
            draw_text(340, 215, 0.4f, rgba(COLOR_ACCENT), "Y: View");
        }

        /* Video download progress toast */
        dl_state_t cur_dl = dl_get_state();
        if (cur_dl == DL_ACTIVE) {
            size_t tot = dl_total(), now = dl_bytes();
            char dl_line1[64], dl_line2[48];
            snprintf(dl_line1, sizeof(dl_line1), "DL: %.28s", dl_item_name());
            if (tot > 0)
                snprintf(dl_line2, sizeof(dl_line2), "%.1f / %.1fMB  (queue: %d)",
                         now / (1024.0f*1024.0f), tot / (1024.0f*1024.0f), dl_queue_count());
            else
                snprintf(dl_line2, sizeof(dl_line2), "%.1fMB downloaded  (queue: %d)",
                         now / (1024.0f*1024.0f), dl_queue_count());
            draw_rect(0, 0, 400, 28, rgba(0x000000CC));
            draw_text(5, 1, 0.38f, rgba(COLOR_ACCENT), dl_line1);
            draw_text(5, 14, 0.38f, rgba(COLOR_TEXT_SECONDARY), dl_line2);
        }
    }

    /* Bottom screen: view-specific */
    switch (state->current_view) {
    case VIEW_LOGIN:
        ui_render_login(state);
        break;
    case VIEW_LIBRARIES:
        ui_render_libraries(state);
        break;
    case VIEW_BROWSE:
        ui_render_browse(state);
        break;
    case VIEW_NOW_PLAYING:
        /* Already rendered above (both screens) */
        break;
    case VIEW_READER:
        /* Already rendered above (both screens) */
        break;
    case VIEW_SETTINGS:
        ui_render_settings(state, session);
        break;
    case VIEW_DOWNLOADS:
        /* Already rendered above (both screens) */
        break;
    }

    C3D_FrameEnd(0);
}
