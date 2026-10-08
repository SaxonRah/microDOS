#ifndef GFX_ENGINE_H
#define GFX_ENGINE_H

#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_ACTOR_VISIBLE 0x01u
#define GFX_ACTOR_ACTIVE 0x02u
#define GFX_ACTOR_SOLID 0x04u

#define GFX_TILE_SOLID 0x01u

typedef struct gfx_anim_frame {
  const gfx_sprite_t GFX_PTR *sprite;
  uint16_t ticks;
} gfx_anim_frame_t;

typedef struct gfx_anim {
  const gfx_anim_frame_t GFX_PTR *frames;
  int frame_count;
  uint8_t loop;
  uint8_t pingpong;
} gfx_anim_t;

typedef struct gfx_camera {
  int x;
  int y;
  int screen_w;
  int screen_h;
  int map_w;
  int map_h;
  int dead_x;
  int dead_y;
  int dead_w;
  int dead_h;
  int shake_x;
  int shake_y;
  int shake_ticks;
} gfx_camera_t;

typedef struct gfx_collision_map {
  int map_w;
  int map_h;
  int tile_w;
  int tile_h;
  const uint8_t GFX_PTR *flags;
  uint8_t solid_mask;
} gfx_collision_map_t;

typedef struct gfx_actor {
  gfx_sprite_instance_t inst;
  const gfx_anim_t GFX_PTR *anim;
  int anim_index;
  int anim_tick;
  int anim_dir;
  int world_x;
  int world_y;
  int vx;
  int vy;
  int min_x;
  int min_y;
  int max_x;
  int max_y;
  int collider_x;
  int collider_y;
  int collider_w;
  int collider_h;
  uint8_t flags;
  uint8_t state;
  uint8_t blocked_x;
  uint8_t blocked_y;
} gfx_actor_t;

void gfx_actor_init(gfx_actor_t GFX_PTR *a, const gfx_sprite_t GFX_PTR *sprite,
                    int world_x, int world_y, int z, int visible);
void gfx_actor_set_bounds(gfx_actor_t GFX_PTR *a, int min_x, int min_y,
                          int max_x, int max_y);
void gfx_actor_set_anim(gfx_actor_t GFX_PTR *a, const gfx_anim_t GFX_PTR *anim);
void gfx_actor_set_velocity(gfx_actor_t GFX_PTR *a, int vx, int vy);
void gfx_actor_set_state(gfx_actor_t GFX_PTR *a, uint8_t state);

void gfx_actor_set_collider(gfx_actor_t GFX_PTR *a, int x, int y, int w, int h);
gfx_rect_t gfx_actor_world_rect(const gfx_actor_t GFX_PTR *a);
void gfx_collision_init(gfx_collision_map_t GFX_PTR *cm, int map_w, int map_h,
                        int tile_w, int tile_h, const uint8_t GFX_PTR *flags,
                        uint8_t solid_mask);
int gfx_collision_tile_solid(const gfx_collision_map_t GFX_PTR *cm, int tx,
                             int ty);
int gfx_collision_rect_solid(const gfx_collision_map_t GFX_PTR *cm,
                             gfx_rect_t world_rect);
void gfx_actor_move_collide(gfx_actor_t GFX_PTR *a,
                            const gfx_collision_map_t GFX_PTR *cm, int dx,
                            int dy);

void gfx_camera_init(gfx_camera_t GFX_PTR *cam, int screen_w, int screen_h,
                     int map_w, int map_h);
void gfx_camera_set_deadzone(gfx_camera_t GFX_PTR *cam, int x, int y, int w,
                             int h);
void gfx_camera_follow_rect(gfx_camera_t GFX_PTR *cam, gfx_rect_t target);
void gfx_camera_apply_shake(gfx_camera_t GFX_PTR *cam, int strength_x,
                            int strength_y, int ticks);
void gfx_camera_update_shake(gfx_camera_t GFX_PTR *cam);
void gfx_actor_update(gfx_actor_t GFX_PTR *a);
void gfx_actor_update_animation(gfx_actor_t GFX_PTR *a);
void gfx_actor_sync_screen(gfx_actor_t GFX_PTR *a, int camera_x, int camera_y);

void gfx_actors_mark_dirty(gfx_dirty_list_t GFX_PTR *d,
                           const gfx_actor_t GFX_PTR *actors, int count,
                           int old_camera_x, int old_camera_y, int new_camera_x,
                           int new_camera_y, int pad);
void gfx_actors_copy_instances(const gfx_actor_t GFX_PTR *actors,
                               gfx_sprite_instance_t GFX_PTR *out_instances,
                               int count);

#ifdef __cplusplus
}
#endif

#endif
