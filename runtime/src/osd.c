/* osd.c — on-screen frame statistics overlay.
 *
 * Draws directly into the ARGB8888 present buffer just before it is handed to a
 * renderer backend, so it works identically for GL, Vulkan and the software
 * path, and it lands in `screenshot` output too — which is the point: a
 * screenshot then carries its own frame timing instead of needing a separate
 * TCP query correlated by hand.
 *
 * Timing is measured HERE, from presented-frame to presented-frame, rather than
 * read out of a renderer's counters, so the number means the same thing on
 * every backend. Off unless enabled (PSX_OSD=1 or the `osd` TCP command).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <time.h>
#endif

static double osd_now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
}

/* 5x7 glyphs, one byte per row, low 5 bits, MSB-of-5 = leftmost pixel. */
typedef struct { char c; unsigned char rows[7]; } OsdGlyph;
static const OsdGlyph OSD_FONT[] = {
    {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {'3',{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
    {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6',{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9',{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'.',{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
    {':',{0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}},
    {'/',{0x01,0x01,0x02,0x04,0x08,0x10,0x10}},
    {'-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'C',{0x0F,0x10,0x10,0x10,0x10,0x10,0x0F}},
    {'D',{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
    {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G',{0x0E,0x11,0x10,0x13,0x11,0x11,0x0E}},
    {'I',{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M',{0x11,0x1B,0x15,0x11,0x11,0x11,0x11}},
    {'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
};

static const unsigned char *osd_glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (size_t i = 0; i < sizeof(OSD_FONT)/sizeof(OSD_FONT[0]); i++)
        if (OSD_FONT[i].c == c) return OSD_FONT[i].rows;
    return NULL;                        /* space and anything unmapped */
}

/* ---- state -------------------------------------------------------------- */
#define OSD_WIN 120                     /* frames in the rolling window */
static int    s_osd_on = -1;            /* -1 = read PSX_OSD once */
static double s_prev_ms = 0.0;
static double s_ring[OSD_WIN];
static int    s_ring_n = 0, s_ring_i = 0;
static double s_extra_val = 0.0;        /* optional caller-supplied stat */
static unsigned long long s_draws = 0;  /* composites performed (observability) */
static char   s_extra_lbl[8] = {0};

/* Perceived-frame ring, fed by display-area flips rather than by presents. */
static double s_gring[OSD_WIN];
static int    s_gring_n = 0, s_gring_i = 0;
static double s_gprev_ms = 0.0;
static unsigned long long s_game_ticks = 0;

int psx_osd_enabled(void) {
    if (s_osd_on < 0) {
        const char *e = getenv("PSX_OSD");
        s_osd_on = (e && e[0] == '1') ? 1 : 0;
    }
    return s_osd_on;
}
void psx_osd_set_enabled(int on) { s_osd_on = on ? 1 : 0; }

/* Let a backend publish one extra number (e.g. prims/frame) without this file
 * having to know anything about that backend. */
void psx_osd_set_extra(const char *label, double value) {
    if (label) { strncpy(s_extra_lbl, label, sizeof(s_extra_lbl) - 1);
                 s_extra_lbl[sizeof(s_extra_lbl) - 1] = 0; }
    s_extra_val = value;
}

void psx_osd_frame_tick(void) {
    double now = osd_now_ms();
    if (s_prev_ms > 0.0) {
        double dt = now - s_prev_ms;
        if (dt > 0.0 && dt < 1000.0) {
            s_ring[s_ring_i] = dt;
            s_ring_i = (s_ring_i + 1) % OSD_WIN;
            if (s_ring_n < OSD_WIN) s_ring_n++;
        }
    }
    s_prev_ms = now;
}

void psx_osd_game_frame_tick(void) {
    double now = osd_now_ms();
    s_game_ticks++;
    if (s_gprev_ms > 0.0) {
        double dt = now - s_gprev_ms;
        if (dt > 0.0 && dt < 1000.0) {
            s_gring[s_gring_i] = dt;
            s_gring_i = (s_gring_i + 1) % OSD_WIN;
            if (s_gring_n < OSD_WIN) s_gring_n++;
        }
    }
    s_gprev_ms = now;
}

unsigned long long psx_osd_game_frame_count(void) { return s_game_ticks; }

static void osd_stats(double *avg, double *mx) {
    double sum = 0.0, m = 0.0;
    for (int i = 0; i < s_ring_n; i++) { sum += s_ring[i]; if (s_ring[i] > m) m = s_ring[i]; }
    *avg = s_ring_n ? sum / s_ring_n : 0.0;
    *mx  = m;
}

double psx_osd_vsync_hz(void) {
    double avg = 0.0, mx = 0.0;
    osd_stats(&avg, &mx);
    return avg > 0.0 ? 1000.0 / avg : 0.0;
}

double psx_osd_game_fps(void) {
    /* Single-buffered titles never flip the display area, so no ticks ever
     * arrive; there the present cadence genuinely IS the frame rate. */
    if (s_gring_n < 2) return psx_osd_vsync_hz();
    double sum = 0.0;
    for (int i = 0; i < s_gring_n; i++) sum += s_gring[i];
    return sum > 0.0 ? 1000.0 * (double)s_gring_n / sum : 0.0;
}

static void osd_put(uint32_t *buf, int w, int h, int pitch_px,
                    int x, int y, const char *s, int scale, uint32_t rgb) {
    for (const char *p = s; *p; p++) {
        const unsigned char *g = osd_glyph(*p);
        if (g) {
            for (int ry = 0; ry < 7; ry++) {
                for (int rx = 0; rx < 5; rx++) {
                    if (!(g[ry] & (0x10 >> rx))) continue;
                    for (int sy = 0; sy < scale; sy++) {
                        int py = y + ry * scale + sy;
                        if (py < 0 || py >= h) continue;
                        for (int sx = 0; sx < scale; sx++) {
                            int px = x + rx * scale + sx;
                            if (px < 0 || px >= w) continue;
                            buf[py * pitch_px + px] = rgb;
                        }
                    }
                }
            }
        }
        x += 6 * scale;
    }
}

unsigned long long psx_osd_draw_count(void) { return s_draws; }

void psx_osd_draw_argb(uint32_t *buf, int w, int h, int pitch_bytes) {
    if (!psx_osd_enabled() || !buf || w <= 0 || h <= 0) return;
    s_draws++;
    int pitch_px = pitch_bytes / (int)sizeof(uint32_t);
    if (pitch_px <= 0) pitch_px = w;

    double avg = 0.0, mx = 0.0;
    osd_stats(&avg, &mx);

    char l1[48], l2[48];
    /* FPS is what the player actually sees (display-area flips). VSYNC is the
     * present cadence, which stays pinned at ~60 on a vsync-locked host even
     * when the game is holding each frame for two or three vblanks. Showing
     * both makes "the game is slow" distinguishable from "the host is slow". */
    snprintf(l1, sizeof l1, "%.1f FPS  %.0f VSYNC",
             psx_osd_game_fps(), psx_osd_vsync_hz());
    if (s_extra_lbl[0])
        snprintf(l2, sizeof l2, "%.1f/%.1f MS  %.0f %s", avg, mx, s_extra_val, s_extra_lbl);
    else
        snprintf(l2, sizeof l2, "%.1f/%.1f MS", avg, mx);

    /* Scale with the frame so the overlay stays readable at 320x240 and at a
     * hi-res internal buffer, but never eats more than a slim top strip. */
    int scale = w >= 1024 ? 3 : (w >= 512 ? 2 : 1);
    int pad = 2 * scale;
    int lh = 8 * scale;
    int bw = (int)(strlen(l2) > strlen(l1) ? strlen(l2) : strlen(l1)) * 6 * scale + pad * 2;
    int bh = lh * 2 + pad * 2;
    if (bw > w) bw = w;
    if (bh > h) bh = h;

    for (int y = 0; y < bh; y++) {              /* dim backdrop for contrast */
        uint32_t *row = buf + (size_t)y * pitch_px;
        for (int x = 0; x < bw; x++) {
            uint32_t p = row[x];
            row[x] = 0xFF000000u | (((p & 0x00FEFEFEu) >> 1) & 0x00FFFFFFu);
        }
    }
    osd_put(buf, w, h, pitch_px, pad, pad,          l1, scale, 0xFF00FF66u);
    osd_put(buf, w, h, pitch_px, pad, pad + lh,     l2, scale, 0xFFFFFFFFu);
}

/* ---- standalone panel ---------------------------------------------------
 * The GL native-wide path presents FBO -> screen and never materialises a CPU
 * frame buffer, so psx_osd_draw_argb has nothing to composite into there.
 * Render the overlay into its own opaque ARGB panel instead; the caller uploads
 * it as a small texture and blits it into a corner. Opaque by design so no
 * alpha blending or shader changes are needed. */
#define OSD_PANEL_MAX (512 * 64)
static uint32_t s_panel[OSD_PANEL_MAX];

const uint32_t *psx_osd_render_panel(int *out_w, int *out_h, int scale) {
    if (!psx_osd_enabled()) return NULL;
    if (scale < 1) scale = 1;
    double avg = 0.0, mx = 0.0;
    osd_stats(&avg, &mx);

    char l1[48], l2[48];
    /* FPS is what the player actually sees (display-area flips). VSYNC is the
     * present cadence, which stays pinned at ~60 on a vsync-locked host even
     * when the game is holding each frame for two or three vblanks. Showing
     * both makes "the game is slow" distinguishable from "the host is slow". */
    snprintf(l1, sizeof l1, "%.1f FPS  %.0f VSYNC",
             psx_osd_game_fps(), psx_osd_vsync_hz());
    if (s_extra_lbl[0])
        snprintf(l2, sizeof l2, "%.1f/%.1f MS  %.0f %s", avg, mx, s_extra_val, s_extra_lbl);
    else
        snprintf(l2, sizeof l2, "%.1f/%.1f MS", avg, mx);

    size_t cols = strlen(l2) > strlen(l1) ? strlen(l2) : strlen(l1);
    int pad = 2 * scale, lh = 8 * scale;
    int w = (int)cols * 6 * scale + pad * 2;
    int h = lh * 2 + pad * 2;
    if (w < 1) w = 1;
    if ((long)w * h > OSD_PANEL_MAX) return NULL;

    for (int i = 0; i < w * h; i++) s_panel[i] = 0xFF101014u;
    osd_put(s_panel, w, h, w, pad, pad,      l1, scale, 0xFF00FF66u);
    osd_put(s_panel, w, h, w, pad, pad + lh, l2, scale, 0xFFFFFFFFu);
    s_draws++;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return s_panel;
}
