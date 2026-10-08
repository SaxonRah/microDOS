#ifndef GFX_H
#define GFX_H

#include "gfx_color.h"
#include "gfx_config.h"
#include "gfx_fixed.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gfx_renderer gfx_renderer_t;

typedef void (*gfx_flush_fn)(gfx_renderer_t GFX_PTR *r, int x, int y, int w,
                             int h, const gfx_color_t GFX_PTR *pixels,
                             void GFX_PTR *user);
typedef void (*gfx_flush_begin_fn)(gfx_renderer_t GFX_PTR *r, int x, int y,
                                   int w, int h,
                                   const gfx_color_t GFX_PTR *pixels,
                                   void GFX_PTR *user);
typedef void (*gfx_flush_wait_fn)(gfx_renderer_t GFX_PTR *r,
                                  void GFX_PTR *user);

struct gfx_renderer {
  int width;
  int height;
  int tile_x;
  int tile_y;
  int tile_w;
  int tile_h;
  int tile_stride;
  gfx_color_t GFX_PTR *tile;
  /* Pixel capacity of the caller-provided tile buffer. gfx_init() infers this
     as width * tile_h; call gfx_set_tile_capacity() if you allocated more and
     intend to use taller tiles later. gfx_begin_tile_rect() clamps against it
     so a dirty rect can never rasterize past the end of the buffer. */
  long tile_capacity;
  int clip_x0;
  int clip_y0;
  int clip_x1; /* exclusive */
  int clip_y1; /* exclusive */
  gfx_flush_fn flush;
  gfx_flush_begin_fn flush_begin;
  gfx_flush_wait_fn flush_wait;
  void GFX_PTR *user;
};

#define GFX_SPRITE_COLORKEY 0x01u
#define GFX_SPRITE_RLE 0x02u
#define GFX_SPRITE_RLE_ROWSTART 0x04u

typedef struct gfx_rle_run {
  int16_t x;
  int16_t y;
  int16_t len;
  uint32_t offset;
} gfx_rle_run_t;

typedef struct gfx_sprite {
  int width;
  int height;
  const gfx_color_t GFX_PTR *pixels; /* raw pixels, or RLE opaque pixel pool */
  const gfx_rle_run_t GFX_PTR *runs;
  int run_count;
  const uint16_t GFX_PTR *row_start; /* optional: height + 1 entries */
  gfx_color_t key;
  uint8_t flags;
} gfx_sprite_t;

typedef struct gfx_tilemap {
  int map_w;
  int map_h;
  int tile_w;
  int tile_h;
  const uint16_t GFX_PTR *tiles;
  const gfx_sprite_t GFX_PTR *tileset;
  int tileset_count;
} gfx_tilemap_t;

typedef struct gfx_rect {
  int x;
  int y;
  int w;
  int h;
} gfx_rect_t;

typedef struct gfx_dirty_list {
  gfx_rect_t rects[GFX_DIRTY_MAX_RECTS];
  int count;
  int full_redraw;
  int screen_w;
  int screen_h;
} gfx_dirty_list_t;

typedef struct gfx_blit_stats {
  unsigned long sprite_pixels_tested;
  unsigned long sprite_pixels_drawn;
  unsigned long sprites_considered;
  unsigned long sprites_rejected;
  unsigned long sprites_drawn;
  unsigned long rle_runs_drawn;
  unsigned long rle_pixels_copied;
} gfx_blit_stats_t;

typedef struct gfx_sprite_instance {
  const gfx_sprite_t GFX_PTR *sprite;
  int x;
  int y;
  int old_x;
  int old_y;
  int z;
  uint8_t visible;
} gfx_sprite_instance_t;

void gfx_init(gfx_renderer_t GFX_PTR *r, int width, int height,
              gfx_color_t GFX_PTR *tile_buffer, int tile_h, gfx_flush_fn flush,
              void GFX_PTR *user);
void gfx_set_tile_capacity(gfx_renderer_t GFX_PTR *r, long pixels);

/* Largest sprite/tile surface the renderer can address.

   Row addressing inside the blitters is int math (row * width). On 16-bit int
   targets such as Open Watcom -ml that overflows above 32767 pixels, so
   gfx_sprite_ready() rejects oversized sprites there rather than silently
   wrapping. 32-bit targets pay nothing for this. */
#if defined(GFX_INT_IS_16BIT) && GFX_INT_IS_16BIT
#define GFX_MAX_SPRITE_PIXELS 32767L
#else
#define GFX_MAX_SPRITE_PIXELS 0x7FFFFFFFL
#endif

/* Load-time validation for RLE sprites whose runs came from disk or from any
   other untrusted source. The hot blit paths deliberately do not re-check runs
   per frame, so anything decoded from a .MRP should be passed through this once
   after loading. Returns 1 if every run is in bounds, 0 otherwise.
   pool_pixels is the number of gfx_color_t entries in s->pixels. */
int gfx_sprite_rle_validate(const gfx_sprite_t GFX_PTR *s,
                            unsigned long pool_pixels);
void gfx_begin_tile(gfx_renderer_t GFX_PTR *r, int tile_y, int tile_h);
void gfx_begin_tile_rect(gfx_renderer_t GFX_PTR *r, int tile_x, int tile_y,
                         int tile_w, int tile_h);
void gfx_flush_tile(gfx_renderer_t GFX_PTR *r);

#define GFX_RENDER_CLEAR 0x01u

void gfx_set_async_flush(gfx_renderer_t GFX_PTR *r, gfx_flush_begin_fn begin_fn,
                         gfx_flush_wait_fn wait_fn);
void gfx_render_tiled(gfx_renderer_t GFX_PTR *r,
                      void (*draw_scene)(gfx_renderer_t GFX_PTR *r,
                                         void GFX_PTR *scene_user),
                      void GFX_PTR *scene_user, gfx_color_t clear_color);
void gfx_render_tiled_ex(gfx_renderer_t GFX_PTR *r,
                         void (*draw_scene)(gfx_renderer_t GFX_PTR *r,
                                            void GFX_PTR *scene_user),
                         void GFX_PTR *scene_user, gfx_color_t clear_color,
                         unsigned flags);
void gfx_render_tiled_no_clear(gfx_renderer_t GFX_PTR *r,
                               void (*draw_scene)(gfx_renderer_t GFX_PTR *r,
                                                  void GFX_PTR *scene_user),
                               void GFX_PTR *scene_user);
void gfx_render_tiled_pipelined(
    gfx_renderer_t GFX_PTR *r, gfx_color_t GFX_PTR *second_tile_buffer,
    void (*draw_scene)(gfx_renderer_t GFX_PTR *r, void GFX_PTR *scene_user),
    void GFX_PTR *scene_user, gfx_color_t clear_color, unsigned flags);
void gfx_render_tiled_region_ex(
    gfx_renderer_t GFX_PTR *r, int x, int y, int w, int h,
    void (*draw_scene)(gfx_renderer_t GFX_PTR *r, void GFX_PTR *scene_user),
    void GFX_PTR *scene_user, gfx_color_t clear_color, unsigned flags);
void gfx_render_tiled_region(gfx_renderer_t GFX_PTR *r, int x, int y, int w,
                             int h,
                             void (*draw_scene)(gfx_renderer_t GFX_PTR *r,
                                                void GFX_PTR *scene_user),
                             void GFX_PTR *scene_user, gfx_color_t clear_color);
void gfx_render_tiled_region_no_clear(
    gfx_renderer_t GFX_PTR *r, int x, int y, int w, int h,
    void (*draw_scene)(gfx_renderer_t GFX_PTR *r, void GFX_PTR *scene_user),
    void GFX_PTR *scene_user);

/* Clip state is per-tile. gfx_begin_tile_rect() resets the clip to the full
   tile at the top of every tile, so a clip set before gfx_render_tiled*() has
   no effect. Set it inside the draw_scene callback instead. */
void gfx_set_clip(gfx_renderer_t GFX_PTR *r, int x, int y, int w, int h);
void gfx_reset_clip(gfx_renderer_t GFX_PTR *r);
void gfx_clear_tile(gfx_renderer_t GFX_PTR *r, gfx_color_t color);
void gfx_draw_pixel(gfx_renderer_t GFX_PTR *r, int x, int y, gfx_color_t color);
void gfx_draw_hline(gfx_renderer_t GFX_PTR *r, int x, int y, int w,
                    gfx_color_t color);
void gfx_draw_vline(gfx_renderer_t GFX_PTR *r, int x, int y, int h,
                    gfx_color_t color);
void gfx_fill_rect(gfx_renderer_t GFX_PTR *r, int x, int y, int w, int h,
                   gfx_color_t color);
void gfx_draw_rect(gfx_renderer_t GFX_PTR *r, int x, int y, int w, int h,
                   gfx_color_t color);
void gfx_draw_line(gfx_renderer_t GFX_PTR *r, int x0, int y0, int x1, int y1,
                   gfx_color_t color);

void gfx_blit_sprite_unclipped(gfx_renderer_t GFX_PTR *r,
                               const gfx_sprite_t GFX_PTR *s, int x, int y);
void gfx_blit_sprite_clipped(gfx_renderer_t GFX_PTR *r,
                             const gfx_sprite_t GFX_PTR *s, int x, int y);
void gfx_blit_sprite_colorkey_unclipped(gfx_renderer_t GFX_PTR *r,
                                        const gfx_sprite_t GFX_PTR *s, int x,
                                        int y);
void gfx_blit_sprite_colorkey_clipped(gfx_renderer_t GFX_PTR *r,
                                      const gfx_sprite_t GFX_PTR *s, int x,
                                      int y);
void gfx_blit_sprite_rle_unclipped(gfx_renderer_t GFX_PTR *r,
                                   const gfx_sprite_t GFX_PTR *s, int x, int y);
void gfx_blit_sprite_rle_clipped(gfx_renderer_t GFX_PTR *r,
                                 const gfx_sprite_t GFX_PTR *s, int x, int y);
void gfx_blit(gfx_renderer_t GFX_PTR *r, const gfx_sprite_t GFX_PTR *s, int x,
              int y);

int gfx_rle_build_from_colorkey(const gfx_color_t GFX_PTR *src, int width,
                                int height, gfx_color_t key,
                                gfx_rle_run_t GFX_PTR *runs, int max_runs,
                                gfx_color_t GFX_PTR *pixel_pool, int max_pixels,
                                gfx_sprite_t GFX_PTR *out_sprite);
int gfx_rle_build_from_colorkey_indexed(
    const gfx_color_t GFX_PTR *src, int width, int height, gfx_color_t key,
    gfx_rle_run_t GFX_PTR *runs, int max_runs, gfx_color_t GFX_PTR *pixel_pool,
    int max_pixels, uint16_t GFX_PTR *row_start, int max_row_start,
    gfx_sprite_t GFX_PTR *out_sprite);

void gfx_draw_tilemap(gfx_renderer_t GFX_PTR *r,
                      const gfx_tilemap_t GFX_PTR *tm, int camera_x,
                      int camera_y, int dst_x, int dst_y, int view_w,
                      int view_h);

gfx_rect_t gfx_rect_make(int x, int y, int w, int h);
gfx_rect_t gfx_rect_clip(gfx_rect_t r, int screen_w, int screen_h);
gfx_rect_t gfx_rect_union(gfx_rect_t a, gfx_rect_t b);
gfx_rect_t gfx_rect_intersection(gfx_rect_t a, gfx_rect_t b);
int gfx_rect_empty(gfx_rect_t r);
long gfx_rect_area(gfx_rect_t r);
int gfx_rects_overlap(gfx_rect_t a, gfx_rect_t b);
int gfx_rects_touch_or_overlap(gfx_rect_t a, gfx_rect_t b);
void gfx_blit_counted(gfx_renderer_t GFX_PTR *r, const gfx_sprite_t GFX_PTR *s,
                      int x, int y, gfx_blit_stats_t GFX_PTR *stats);
gfx_rect_t gfx_sprite_rect(const gfx_sprite_t GFX_PTR *s, int x, int y);
void gfx_sprite_instance_init(gfx_sprite_instance_t GFX_PTR *inst,
                              const gfx_sprite_t GFX_PTR *sprite, int x, int y,
                              int z, int visible);
void gfx_sprite_instance_set_pos(gfx_sprite_instance_t GFX_PTR *inst, int x,
                                 int y);
gfx_rect_t gfx_sprite_instance_rect(const gfx_sprite_instance_t GFX_PTR *inst);
gfx_rect_t
gfx_sprite_instance_old_rect(const gfx_sprite_instance_t GFX_PTR *inst);
void gfx_sprites_mark_dirty(gfx_dirty_list_t GFX_PTR *d,
                            const gfx_sprite_instance_t GFX_PTR *sprites,
                            int count, int pad);
void gfx_sprites_draw_intersecting(gfx_renderer_t GFX_PTR *r,
                                   const gfx_sprite_instance_t GFX_PTR *sprites,
                                   int count, gfx_rect_t region,
                                   gfx_blit_stats_t GFX_PTR *stats);
void gfx_sprites_draw_intersecting_grid(
    gfx_renderer_t GFX_PTR *r, const gfx_sprite_instance_t GFX_PTR *sprites,
    int count, gfx_rect_t region, gfx_blit_stats_t GFX_PTR *stats, int cell_w,
    int cell_h);
void gfx_dirty_init(gfx_dirty_list_t GFX_PTR *d, int screen_w, int screen_h);
void gfx_dirty_clear(gfx_dirty_list_t GFX_PTR *d);
void gfx_dirty_mark_full(gfx_dirty_list_t GFX_PTR *d);
void gfx_dirty_add_rect(gfx_dirty_list_t GFX_PTR *d, gfx_rect_t r);
void gfx_dirty_add_sprite_move(gfx_dirty_list_t GFX_PTR *d, int old_x,
                               int old_y, int new_x, int new_y, int w, int h,
                               int pad);
void gfx_dirty_merge(gfx_dirty_list_t GFX_PTR *d);
unsigned long gfx_dirty_total_area(const gfx_dirty_list_t GFX_PTR *d);
void gfx_dirty_adapt_to_full(gfx_dirty_list_t GFX_PTR *d,
                             unsigned long threshold_pixels);
void gfx_dirty_render(
    gfx_renderer_t GFX_PTR *r, const gfx_dirty_list_t GFX_PTR *d,
    void (*draw_scene)(gfx_renderer_t GFX_PTR *r, void GFX_PTR *scene_user),
    void GFX_PTR *scene_user, gfx_color_t clear_color, unsigned flags);

void gfx_draw_char5x7(gfx_renderer_t GFX_PTR *r, int x, int y, char ch,
                      gfx_color_t color, int scale);
void gfx_draw_text5x7(gfx_renderer_t GFX_PTR *r, int x, int y,
                      const char GFX_PTR *text, gfx_color_t color, int scale);

#if GFX_ENABLE_TRIANGLES
void gfx_fill_triangle(gfx_renderer_t GFX_PTR *r, int x0, int y0, int x1,
                       int y1, int x2, int y2, gfx_color_t color);
#endif

#ifdef __cplusplus
}
#endif

#endif /* GFX_H */
