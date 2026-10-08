#ifndef GFX_PACK_H
#define GFX_PACK_H

#include "gfx_config.h"
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_PACK_MAGIC0 'M'
#define GFX_PACK_MAGIC1 'R'
#define GFX_PACK_MAGIC2 'P'
#define GFX_PACK_MAGIC3 '1'

#define GFX_PACK_HEADER_SIZE 12u
#define GFX_PACK_DIR_ENTRY_SIZE 44u

#define GFX_PACK_ENTRY_SPRITE_RAW 1u
#define GFX_PACK_ENTRY_SPRITE_RLE 2u
#define GFX_PACK_ENTRY_TILEMAP_U16 3u
#define GFX_PACK_ENTRY_PALETTE_RGB 4u
#define GFX_PACK_ENTRY_ANIM 5u
#define GFX_PACK_ENTRY_COLLISION_U8 6u
#define GFX_PACK_ENTRY_SPAWNS 7u
#define GFX_PACK_ENTRY_TRIGGERS 8u
#define GFX_PACK_ENTRY_TILE_FLAGS 9u
#define GFX_PACK_ENTRY_AUDIO_U8 10u
#define GFX_PACK_ENTRY_PROJECT_INFO 11u

#ifndef GFX_PACK_MAX_ENTRIES
#define GFX_PACK_MAX_ENTRIES 96
#endif

typedef struct gfx_pack_entry {
  char name[32];
  uint16_t kind;
  uint32_t offset;
  uint32_t size;
} gfx_pack_entry_t;

typedef struct gfx_pack {
  FILE *fp;
  uint16_t count;
  uint32_t data_offset;
  gfx_pack_entry_t entries[GFX_PACK_MAX_ENTRIES];
} gfx_pack_t;

int gfx_pack_open(gfx_pack_t GFX_PTR *pack, const char *path);
void gfx_pack_close(gfx_pack_t GFX_PTR *pack);
const gfx_pack_entry_t GFX_PTR *gfx_pack_find(const gfx_pack_t GFX_PTR *pack,
                                              const char *name);
const char *gfx_pack_kind_name(uint16_t kind);
int gfx_pack_read(const gfx_pack_t GFX_PTR *pack,
                  const gfx_pack_entry_t GFX_PTR *entry, void GFX_PTR *dst,
                  uint32_t max_bytes, uint32_t GFX_PTR *out_bytes);

#ifdef __cplusplus
}
#endif

#endif
