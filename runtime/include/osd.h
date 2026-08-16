/* osd.h — on-screen frame statistics overlay (see src/osd.c).
 *
 * Backend-independent: the caller hands over the ARGB8888 present buffer just
 * before it goes to a renderer, so GL, Vulkan and the software path all get the
 * same overlay, and it appears in `screenshot` output as well. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  psx_osd_enabled(void);
void psx_osd_set_enabled(int on);

/* One call per PRESENTED frame, before psx_osd_draw_argb. Frame pacing is
 * measured from these calls, not from renderer counters, so the reported ms
 * means the same thing on every backend.
 *
 * This is the VSYNC cadence, not the game's frame rate: the runtime presents at
 * every simulated vblank whether or not the game finished a new frame, so on a
 * vsync-locked host it is pinned at ~60 Hz even while the game visibly crawls.
 * Use psx_osd_game_frame_tick for the rate a player actually perceives. */
void psx_osd_frame_tick(void);

/* One call per frame the player actually SEES — i.e. when the GPU's display
 * area start (GP1(05h)) changes, which is how a double-buffered PSX title
 * publishes a finished frame. A game that misses its vblank deadline holds the
 * same buffer for 2 or 3 vblanks and this fires correspondingly less often,
 * which is exactly the slowdown a player perceives.
 *
 * Single-buffered titles never move the display area and so never call this;
 * the reported FPS then falls back to the present cadence, where present rate
 * genuinely IS the frame rate. */
void psx_osd_game_frame_tick(void);

/* Frames-per-second as perceived (display-area flips), with the fallback above.
 * psx_osd_game_frame_count is the raw tick count since boot. */
double             psx_osd_game_fps(void);
unsigned long long psx_osd_game_frame_count(void);

/* The present/vblank cadence, for comparison against psx_osd_game_fps. */
double psx_osd_vsync_hz(void);

/* Optional extra stat rendered on the second line (e.g. "PRIM", 302). */
void psx_osd_set_extra(const char *label, double value);

/* Composite the overlay into an ARGB8888 buffer in place. No-op when disabled. */
void psx_osd_draw_argb(uint32_t *buf, int w, int h, int pitch_bytes);

/* Composites performed since boot. Grows at frame rate when the on-window
 * overlay is live; only on capture when it is not. */
unsigned long long psx_osd_draw_count(void);

/* Render the overlay into a self-contained OPAQUE ARGB8888 panel, for backends
 * that present on the GPU and never expose a CPU frame buffer (the GL
 * native-wide path). Returns NULL when disabled. Buffer is statically owned and
 * valid until the next call. */
const uint32_t *psx_osd_render_panel(int *out_w, int *out_h, int scale);

#ifdef __cplusplus
}
#endif
