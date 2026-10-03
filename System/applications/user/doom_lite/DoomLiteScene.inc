/* The immutable sparse map index is the only map geometry touched by each
 * native ray. A bounded interval list replaces a full per-pixel Z buffer. */
#include "GameSprites.h"
#include "DoomMap.h"
#include "GameWorldTextures.h"

#ifndef GAME_RAY_SPANS
#define GAME_RAY_SPANS 32u /* Walls plus foreground floor/ceiling depth strips. */
#endif
#define GAME_VERTICAL_PROJECTION 128
#define GAME_RAY_MAX_BOUNDARIES 48u
#define GAME_SPRITE_MAX_WIDTH 72u
#define GAME_WALL_SWITCH 254u
#define GAME_WALL_EXIT 253u
#define GAME_WALL_USED_SWITCH 252u
#define GAME_WALL_MASKED 251u
#define GAME_WALL_PLANE 250u /* Depth-only floor/ceiling cover; background owns its pixels. */

typedef struct {
    uint16_t depth;
    uint8_t top, bottom, color, u;
    uint8_t texture, texture_u;
    uint16_t texture_v, texture_step; /* Wrapped 32-pixel column, Q8. */
} GameWallSpan;

/* Keep the same bounded capacity, but touch only this frame's contiguous
 * prefix. A 384-byte per-ray stride otherwise spreads even one span/ray
 * across 48 KiB, exceeding the Loader's 40 KiB writable page cache. */
static GameWallSpan game_spans[LITE_RAYS * GAME_RAY_SPANS];
static uint16_t game_span_first[LITE_RAYS];
static uint8_t game_span_count[LITE_RAYS];
static DoomLiteRenderStats game_render_stats;
static uint32_t (*game_render_clock)(void);
#ifndef DOOM_GAME_FULL_PLANE_DETAIL
static uint8_t game_plane_row[LITE_W];
#endif

void DoomLite_SetRenderClock(uint32_t (*clock_us)(void)) {
    game_render_clock = clock_us;
}
static uint32_t game_render_now(void) {
    return game_render_clock ? game_render_clock() : 0u;
}

const DoomLiteRenderStats *DoomLite_GetRenderStats(void) {
    return &game_render_stats;
}
unsigned DoomLite_WorldReadonlyBytes(void) {
    return (unsigned)(GAME_WORLD_PIXEL_BYTES + sizeof(game_world_gray) +
        sizeof(game_world_textures) + sizeof(game_world_flats) + sizeof(game_world_maps) +
        sizeof(game_world_e1m1_sides) + sizeof(game_world_e1m2_sides) +
        sizeof(game_world_e1m1_lines) + sizeof(game_world_e1m2_lines) +
        sizeof(game_world_e1m1_sectors) + sizeof(game_world_e1m2_sectors));
}
unsigned DoomLite_RenderWorkingSetBytes(void) {
    return (unsigned)(sizeof(game_spans) + sizeof(game_span_first) + sizeof(game_span_count) +
                      sizeof(game_render_stats) + sizeof(game_wall_ready) +
                      sizeof(game_render_clock)
#ifndef DOOM_GAME_FULL_PLANE_DETAIL
                      + sizeof(game_plane_row)
#endif
                      );
}

unsigned DoomLite_SpriteReadonlyBytes(void) {
    return (unsigned)(GAME_SPRITE_BYTES + sizeof(game_sprite_gray) +
        sizeof(game_sprites) + sizeof(game_sprite_sequences) +
        sizeof(game_key_types) + sizeof(game_key_sprites) + sizeof(game_static_sprites));
}

static uint8_t game_gray(uint8_t color, uint8_t contrast) {
    /* Keep highlights light, while the stronger mode gives mortar and actor
     * silhouettes more separation. No extra framebuffer or second pass. */
    if (contrast == 0u) return (uint8_t)(48u + color * 207u / 255u);
    if (contrast >= 2u) {
        int value = ((int)color - 128) * 5 / 4 + 136;
        if (value < 0) value = 0;
        if (value > 255) value = 255;
        return (uint8_t)value;
    }
    return color;
}

static int game_project_y(int height, int eye, unsigned depth) {
    if (depth < 12u) depth = 12u;
    return (int)DOOM_GAME_VIEW_H / 2 -
           (height - eye) * GAME_VERTICAL_PROJECTION / (int)depth;
}

static void game_add_span(unsigned ray, int top, int bottom, unsigned depth,
                          unsigned color, unsigned u, int clip_top,
                          int clip_bottom) {
    if (top < clip_top) top = clip_top;
    if (bottom > clip_bottom) bottom = clip_bottom;
    if (top < 0) top = 0;
    if (bottom > (int)DOOM_GAME_VIEW_H) bottom = (int)DOOM_GAME_VIEW_H;
    if (top >= bottom || game_span_count[ray] >= GAME_RAY_SPANS) return;
    GameWallSpan *span = &game_spans[game_span_first[ray] + game_span_count[ray]++];
    *span = (GameWallSpan){.depth = (uint16_t)depth, .top = (uint8_t)top,
                           .bottom = (uint8_t)bottom, .color = (uint8_t)color,
                           .u = (uint8_t)u};
    ++game_render_stats.surfaces;
}

/* A boundary can add several strips before its capacity check. The final
 * slot then belongs to this boundary, not an earlier near surface. Replace
 * that slot if necessary and cover the incoming window, including any plane
 * or wall strip that a full cache rejected. The coarse opaque fallback keeps
 * later sprites behind the boundary and never retains masked/texture state. */
static void game_add_fallback(unsigned ray, int top, int bottom, unsigned depth,
                              unsigned color, unsigned u) {
    if (top < 0) top = 0;
    if (bottom > (int)DOOM_GAME_VIEW_H) bottom = (int)DOOM_GAME_VIEW_H;
    if (top >= bottom) return;
    if (game_span_count[ray] >= GAME_RAY_SPANS) {
        game_span_count[ray] = GAME_RAY_SPANS - 1u;
        --game_render_stats.surfaces; /* Replacement is one cached surface. */
    }
    game_add_span(ray, top, bottom, depth, color, u, top, bottom);
    ++game_render_stats.surface_limit_hits;
    game_render_stats.surface_limit_pixels += (uint32_t)(bottom - top);
}

/* Original sidedef alignment, prepared once per visible span. No texture
 * decompression, modulus or division is done inside the pixel loop. Keeping
 * V in the unclipped world coordinate prevents near walls from stretching
 * their entire image into the visible screen fragment. */
static void game_texture_span(unsigned ray, unsigned before, unsigned texture,
                              int world_u, int anchor, int eye) {
    if (game_span_count[ray] == before || texture == 0u ||
        texture > GAME_WORLD_TEXTURE_COUNT) return;
    GameWallSpan *span = &game_spans[game_span_first[ray] + (before)];
    const GameWorldTexture *asset = &game_world_textures[texture];
    int u = world_u % asset->world_width;
    if (u < 0) u += asset->world_width;
    const unsigned depth = span->depth < 12u ? 12u : span->depth;
    const int v_q8 = (anchor - eye) * 256 +
        ((int)span->top - (int)DOOM_GAME_VIEW_H / 2) * (int)depth * 2;
    span->texture = (uint8_t)texture;
    span->texture_u = (uint8_t)((unsigned)u * 32u / asset->world_width);
    span->texture_v = (uint16_t)((v_q8 * 32 / asset->world_height) & 8191);
    span->texture_step = (uint16_t)(depth * 64u / asset->world_height);
}

static unsigned game_texture_height(unsigned texture) {
    return texture && texture <= GAME_WORLD_TEXTURE_COUNT
        ? game_world_textures[texture].world_height : 128u;
}

static const GameWorldSide *game_world_side(const DoomMap *map, unsigned line,
                                           unsigned front) {
    const GameWorldMap *world = &game_world_maps[map->index];
    if (line >= world->line_count) return NULL;
    const GameWorldLine *edge = &world->lines[line];
    const unsigned side = front ? edge->front_side : edge->back_side;
    return side < world->side_count ? &world->sides[side] : NULL;
}

static int game_world_u(const DoomMap *map, unsigned index, unsigned front,
                        int hit_x, int hit_y, const GameWorldSide *side) {
    const DoomMapLine *line = &map->lines[index];
    const GameWorldLine *edge = &game_world_maps[map->index].lines[index];
    const int dx = front ? hit_x - line->x1 : line->x2 - hit_x;
    const int dy = front ? hit_y - line->y1 : line->y2 - hit_y;
    return side->x_offset + (dx * edge->tangent_x + dy * edge->tangent_y) / 16384;
}

static unsigned game_world_code(const GameWorldTexture *asset, unsigned u,
                                unsigned v) {
    const unsigned index = (u & 31u) * 32u + (v & 31u);
    const unsigned packed = asset->pixels[index >> 1];
    return (index & 1u) ? packed >> 4 : packed & 15u;
}

/* Plane detail is sampled from the player's current sector. This compact
 * renderer does not allocate Doom visplanes; neighbouring stepped sectors
 * retain exact wall clipping but share this background plane projection. */
static void game_plane_row_detail(uint8_t *row, unsigned y,
                                  const DoomLiteGame *game, uint16_t sector) {
    const unsigned horizon = DOOM_GAME_VIEW_H / 2u;
    const uint8_t background = y < horizon
        ? (uint8_t)(237u - y / 7u) : (uint8_t)(205u + y / 10u);
    const GameWorldSector *surface = &game_world_maps[game->map->index].sectors[sector];
    const unsigned texture = y < horizon ? surface->ceiling : surface->floor;
    if (texture == 0u || texture == 255u || texture > GAME_WORLD_FLAT_COUNT || y == horizon) {
        memset(row, game_gray(background, game->contrast), LITE_W);
        return;
    }
    const int floor = DoomLiteGame_FloorHeight(game, sector);
    const int eye = floor + 41;
    const int height = y < horizon ? DoomLiteGame_CeilingHeight(game, sector) - eye : eye - floor;
    const unsigned delta = y < horizon ? horizon - y : y - horizon;
    if (height <= 0) {
        memset(row, game_gray(background, game->contrast), LITE_W);
        return;
    }
    unsigned distance_q8 = (unsigned)height * GAME_VERTICAL_PROJECTION * 256u / delta;
    if (distance_q8 > LITE_MAX_DISTANCE_Q8) distance_q8 = LITE_MAX_DISTANCE_Q8;
    const int fx = sine_q14((uint8_t)(game->facing + 64u));
    const int fy = sine_q14(game->facing);
    const int32_t step_x = (int32_t)(-(int64_t)fy * distance_q8 / (16384 * (LITE_PROJECTION / 2)));
    const int32_t step_y = (int32_t)((int64_t)fx * distance_q8 / (16384 * (LITE_PROJECTION / 2)));
    int32_t x = game->x_q8 + (int32_t)((int64_t)fx * distance_q8 / 16384) - step_x * (LITE_RAYS / 2u);
    int32_t z = game->y_q8 + (int32_t)((int64_t)fy * distance_q8 / 16384) - step_y * (LITE_RAYS / 2u);
    const GameWorldTexture *asset = &game_world_flats[texture];
    game_render_stats.plane_texture_pixels += LITE_W;
    /* Keep large floor/ceiling areas lighter than the wall's darkest joints.
     * This preserves texture detail without turning the low-resolution LCD
     * into a dark checkerboard or drowning the enemy silhouettes. */
    static const uint8_t plane_gray[16] = {
        237,94,104,114,124,134,144,154,164,174,184,194,204,214,224,234
    };
    for (unsigned ray = 0; ray < LITE_RAYS; ++ray) {
        const uint8_t color = game_gray(plane_gray[game_world_code(asset, (unsigned)(x >> 9),
                                                                 (unsigned)(z >> 9))], game->contrast);
        row[ray * 2u] = row[ray * 2u + 1u] = color;
        x += step_x; z += step_y;
    }
}

static void game_background_row(uint8_t *row, unsigned y,
                                const DoomLiteGame *game, uint16_t sector) {
#ifdef DOOM_GAME_FULL_PLANE_DETAIL
    game_plane_row_detail(row, y, game, sector);
#else
    /* Floors/ceilings use square 2x2 screen samples; walls retain their
     * full vertical resolution. An explicit row cache keeps this independent
     * of the wall-painted framebuffer and halves plane texture work. */
    if (!(y & 1u)) game_plane_row_detail(game_plane_row, y, game, sector);
    memcpy(row, game_plane_row, LITE_W);
#endif
}

/* Compare within this DDA interval before dividing. The bound checks retain
 * the exact WAD segment instead of interpreting a cell as one solid square. */
static uint32_t game_line_distance(const DoomMapLine *line, int32_t x,
                                   int32_t y, int32_t dx, int32_t dy,
                                   uint32_t entry, uint32_t exit,
                                   int want_door_u, uint8_t *u) {
    const int32_t ax = (int32_t)line->x1 * 256 - x;
    const int32_t ay = (int32_t)line->y1 * 256 - y;
    const int32_t sx = (int32_t)line->x2 - line->x1;
    const int32_t sy = (int32_t)line->y2 - line->y1;
    int64_t denominator = (int64_t)dx * sy - (int64_t)dy * sx;
    if (!denominator) return UINT32_MAX;
    int64_t along = (int64_t)ax * sy - (int64_t)ay * sx;
    int64_t segment = (int64_t)ax * dy - (int64_t)ay * dx;
    if (denominator < 0) {
        denominator = -denominator;
        along = -along;
        segment = -segment;
    }
    if (along < 0 || segment < 0 || segment > denominator * 256)
        return UINT32_MAX;
    const uint64_t scaled = (uint64_t)along << 14;
    if (scaled < (uint64_t)entry * (uint64_t)denominator ||
        scaled > (uint64_t)exit * (uint64_t)denominator)
        return UINT32_MAX;
    *u = want_door_u ? (uint8_t)((uint64_t)segment * 255u /
                   ((uint64_t)denominator * 256u)) : 0u;
    return (uint32_t)(scaled / (uint64_t)denominator);
}

/* Most WAD edges are axis-aligned. Factor their distance ratio down to one
 * coordinate/direction, and reuse the ray's Q32 reciprocal. The two bounded
 * remainder corrections retain EXACT integer division results; this is not
 * an approximate ray step. Diagonal geometry keeps the general cross-product
 * path above. No 64-bit division is needed for a non-door axis edge. */
static uint32_t game_line_distance_fast(const DoomMapLine *line, int32_t x,
                                        int32_t y, int32_t dx, int32_t dy,
                                        uint32_t entry, uint32_t exit,
                                        int want_door_u, uint8_t *u,
                                        uint32_t reciprocal_x,
                                        uint32_t reciprocal_y) {
    const int32_t sx = (int32_t)line->x2 - line->x1;
    const int32_t sy = (int32_t)line->y2 - line->y1;
    if (sx && sy) return game_line_distance(line, x, y, dx, dy, entry, exit,
                                           want_door_u, u);
    const int vertical = sx == 0;
    const int32_t direction = vertical ? dx : dy;
    if (!direction || (!sx && !sy)) return UINT32_MAX;
    const int32_t ax = (int32_t)line->x1 * 256 - x;
    const int32_t ay = (int32_t)line->y1 * 256 - y;
    const int32_t delta = vertical ? ax : ay;
    if ((direction > 0 && delta < 0) || (direction < 0 && delta > 0))
        return UINT32_MAX;
    const uint32_t positive_direction = direction < 0
        ? (uint32_t)-direction : (uint32_t)direction;
    const uint32_t positive_delta = delta < 0 ? (uint32_t)-delta : (uint32_t)delta;
    const uint64_t scaled = (uint64_t)positive_delta << 14;
    if (scaled < (uint64_t)entry * positive_direction ||
        scaled > (uint64_t)exit * positive_direction) return UINT32_MAX;
    int64_t denominator = vertical ? (int64_t)dx * sy : -(int64_t)dy * sx;
    int64_t segment = (int64_t)ax * dy - (int64_t)ay * dx;
    if (denominator < 0) { denominator = -denominator; segment = -segment; }
    if (segment < 0 || segment > denominator * 256) return UINT32_MAX;
    const uint32_t reciprocal = vertical ? reciprocal_x : reciprocal_y;
    uint32_t distance = (uint32_t)((scaled * reciprocal) >> 32);
    if ((uint64_t)(distance + 1u) * positive_direction <= scaled) ++distance;
    if ((uint64_t)(distance + 1u) * positive_direction <= scaled) ++distance;
    *u = want_door_u ? (uint8_t)((uint64_t)segment * 255u /
                   ((uint64_t)denominator * 256u)) : 0u;
    return distance;
}

static uint8_t game_door_u(const DoomMapLine *line, int32_t x, int32_t y,
                           int32_t dx, int32_t dy) {
    const int32_t ax = (int32_t)line->x1 * 256 - x;
    const int32_t ay = (int32_t)line->y1 * 256 - y;
    const int32_t sx = (int32_t)line->x2 - line->x1;
    const int32_t sy = (int32_t)line->y2 - line->y1;
    int64_t denominator = (int64_t)dx * sy - (int64_t)dy * sx;
    int64_t segment = (int64_t)ax * dy - (int64_t)ay * dx;
    if (denominator < 0) { denominator = -denominator; segment = -segment; }
    return denominator ? (uint8_t)((uint64_t)segment * 255u /
                                   ((uint64_t)denominator * 256u)) : 0u;
}

static uint32_t game_div_direction(uint32_t value, uint32_t direction,
                                   uint32_t reciprocal) {
    uint32_t quotient = (uint32_t)(((uint64_t)value * reciprocal) >> 32);
    /* value fits 32 bits, so at most one correction is required. */
    if ((uint64_t)(quotient + 1u) * direction <= value) ++quotient;
    return quotient;
}

/* Mirror the implemented F2 specials, deliberately excluding walk triggers.
 * 1 = manual door, 2 = remote switch, 3 = exit switch. */
static unsigned game_use_marker(unsigned special) {
    switch (special) {
        case 1: case 26: case 27: case 28: case 31: case 32: case 33:
        case 34: case 117: case 118: return 1u;
        case 23: case 62: case 63: case 71: case 102: case 103: case 123: return 2u;
        case 11: return 3u;
        default: return 0u;
    }
}

/* A sign has a fixed world width rather than covering an entire linedef.
 * On the front side the line's direction runs right-to-left on screen. The
 * reversed coordinate makes the small labels readable from the use side. */
static uint8_t game_sign_u(const DoomMapLine *line, int32_t hit_x,
                           int32_t hit_y, unsigned marker) {
    const int sx = line->x2 - line->x1, sy = line->y2 - line->y1;
    const int horizontal = (sx < 0 ? -sx : sx) >= (sy < 0 ? -sy : sy);
    const int offset = horizontal
        ? (hit_x * 2 - line->x1 - line->x2) * (sx < 0 ? -1 : 1)
        : (hit_y * 2 - line->y1 - line->y2) * (sy < 0 ? -1 : 1);
    const int coordinate = 128 - offset * (marker == 3u ? 2 : 4);
    return (uint8_t)(coordinate < 0 ? 0 : coordinate > 255 ? 255 : coordinate);
}

static void game_cast_spans(const DoomLiteGame *game, unsigned ray,
                            uint16_t angle, int offset, uint16_t start_sector,
                            int eye) {
    const DoomMap *map = game->map;
    const int32_t x = game->x_q8, y = game->y_q8;
    const int32_t dx = sine_q14_fine((uint16_t)(angle + 256u));
    const int32_t dy = sine_q14_fine(angle);
    int gx = (x >> 13) - map->grid_x0 / 32;
    int gy = (y >> 13) - map->grid_y0 / 32;
    const int step_x = dx < 0 ? -1 : 1, step_y = dy < 0 ? -1 : 1;
    const uint32_t abs_x = dx < 0 ? (uint32_t)-dx : (uint32_t)dx;
    const uint32_t abs_y = dy < 0 ? (uint32_t)-dy : (uint32_t)dy;
    const uint32_t reciprocal_x = abs_x ? UINT32_MAX / abs_x : 0u;
    const uint32_t reciprocal_y = abs_y ? UINT32_MAX / abs_y : 0u;
    const uint32_t cell = DOOM_MAP_CELL_SIZE * 256u;
    const int world_gx = x >> 13, world_gy = y >> 13;
    const uint32_t delta_x = abs_x
        ? game_div_direction(cell << 14, abs_x, reciprocal_x) : UINT32_MAX;
    const uint32_t delta_y = abs_y
        ? game_div_direction(cell << 14, abs_y, reciprocal_y) : UINT32_MAX;
    const uint32_t offset_x = dx >= 0
        ? (uint32_t)((world_gx + 1) * (int32_t)cell - x)
        : (uint32_t)(x - world_gx * (int32_t)cell);
    const uint32_t offset_y = dy >= 0
        ? (uint32_t)((world_gy + 1) * (int32_t)cell - y)
        : (uint32_t)(y - world_gy * (int32_t)cell);
    uint32_t next_x = abs_x
        ? game_div_direction(offset_x << 14, abs_x, reciprocal_x) : UINT32_MAX;
    uint32_t next_y = abs_y
        ? game_div_direction(offset_y << 14, abs_y, reciprocal_y) : UINT32_MAX;
    uint32_t entry = 0u;
    uint16_t previous_line = DOOM_MAP_NO_LINE, sector = start_sector;
    int clip_top = 0, clip_bottom = DOOM_GAME_VIEW_H;
    unsigned boundaries = 0u;
    game_span_count[ray] = 0u;
    GAME_SET_DEPTH(ray, 1536u);
    const int correction = sine_q14_fine((uint16_t)(offset + 256));
    ++game_render_stats.rays;
    for (unsigned crossed = 0; crossed < map->grid_width + map->grid_height + 2u;
         ++crossed) {
        if (entry > LITE_MAX_DISTANCE_Q8 || clip_top >= clip_bottom) return;
        if ((unsigned)gx >= map->grid_width || (unsigned)gy >= map->grid_height)
            return;
        ++game_render_stats.cells;
        uint32_t exit = next_x < next_y ? next_x : next_y;
        if (exit > LITE_MAX_DISTANCE_Q8) exit = LITE_MAX_DISTANCE_Q8;
        const DoomMapCell *cell_info = DoomMap_Cell(map, gx, gy);
        uint32_t interval = entry;
        while (cell_info && interval <= exit) {
            uint32_t nearest = UINT32_MAX;
            uint16_t hit_line = DOOM_MAP_NO_LINE;
            for (unsigned ref = cell_info->first_ref;
                 ref < (unsigned)cell_info->first_ref + cell_info->count; ++ref) {
                const uint16_t index = map->cell_refs[ref];
                if (index == previous_line) continue;
                const DoomMapLine *line = &map->lines[index];
                if (line->front_sector != sector && line->back_sector != sector)
                    continue;
                uint8_t u;
                ++game_render_stats.line_tests;
                const uint32_t distance = game_line_distance_fast(line, x, y, dx, dy,
                    interval, exit, 0, &u, reciprocal_x, reciprocal_y);
                if (distance < nearest) {
                    nearest = distance;
                    hit_line = index;
                }
            }
            if (hit_line == DOOM_MAP_NO_LINE) break;
            ++game_render_stats.boundaries;
            const DoomMapLine *line = &map->lines[hit_line];
            const unsigned depth = (unsigned)(((int)(nearest >> 8) * correction) >> 14);
            const unsigned safe_depth = depth > 0u ? depth : 1u;
            const int current_floor = DoomLiteGame_FloorHeight(game, sector);
            const int current_ceiling = DoomLiteGame_CeilingHeight(game, sector);
            const uint16_t other = line->front_sector == sector
                ? line->back_sector : line->front_sector;
            int base = 142 + (int)safe_depth / 28;
            if (base > 202) base = 202;
            const int32_t hit_x = (x >> 8) + (dx * (int32_t)(nearest >> 8) >> 14);
            const int32_t hit_y = (y >> 8) + (dy * (int32_t)(nearest >> 8) >> 14);
            const unsigned brick_u = (unsigned)(hit_x + hit_y) & 63u;
            const unsigned front = line->front_sector == sector;
            const GameWorldSide *side = game_world_side(map, hit_line, front);
            const int world_u = side ? game_world_u(map, hit_line, front, hit_x, hit_y, side) : 0;
            const int row_offset = side ? side->y_offset : 0;
            const int incoming_top = clip_top, incoming_bottom = clip_bottom;
            /* Near-sector planes hide geometry behind this boundary. A
             * descending portal has no lower wall, so wall spans alone let
             * a low actor paint through the foreground platform floor. */
            const int current_top = game_project_y(current_ceiling, eye, safe_depth);
            const int current_bottom = game_project_y(current_floor, eye, safe_depth);
            const GameWorldSector *material = &game_world_maps[map->index].sectors[sector];
            if (material->ceiling != GAME_WORLD_SKY)
                game_add_span(ray, clip_top, current_top, safe_depth,
                               GAME_WALL_PLANE, 0u, clip_top, clip_bottom);
            game_add_span(ray, current_bottom, clip_bottom, safe_depth,
                           GAME_WALL_PLANE, 0u, clip_top, clip_bottom);
            unsigned before = game_span_count[ray];
            if (other == DOOM_MAP_NO_SECTOR || other >= map->sector_count) {
                const unsigned marker = line->front_sector == sector
                    ? game_use_marker(line->special) : 0u;
                const int used = (game->triggered[hit_line >> 3] >> (hit_line & 7u)) & 1u;
                const unsigned style = marker == 3u ? GAME_WALL_EXIT
                    : marker == 2u ? (used ? GAME_WALL_USED_SWITCH : GAME_WALL_SWITCH)
                    : (unsigned)base;
                game_add_span(ray, game_project_y(current_ceiling, eye, safe_depth),
                               game_project_y(current_floor, eye, safe_depth),
                               safe_depth, style,
                               marker >= 2u ? game_sign_u(line, hit_x, hit_y, marker) : brick_u,
                               clip_top, clip_bottom);
                const unsigned texture = side ? side->middle : 0u;
                const int anchor = line->flags & 16u
                    ? current_floor + (int)game_texture_height(texture) : current_ceiling;
                game_texture_span(ray, before, texture, world_u, anchor + row_offset, eye);
                if (game_span_count[ray] >= GAME_RAY_SPANS)
                    game_add_fallback(ray, incoming_top, incoming_bottom, safe_depth,
                                      (unsigned)base, brick_u);
                GAME_SET_DEPTH(ray, safe_depth);
                return;
            }
            const int floor = DoomLiteGame_FloorHeight(game, other);
            const int ceiling = DoomLiteGame_CeilingHeight(game, other);
            const int top = game_project_y(ceiling < current_ceiling ? ceiling : current_ceiling,
                                           eye, safe_depth);
            const int bottom = game_project_y(floor > current_floor ? floor : current_floor,
                                              eye, safe_depth);
            /* A sector that originally has zero headroom is a door. Its
             * ceiling moves vertically, so the light panel lifts gradually. */
            const int door = map->sectors[other].ceiling <= map->sectors[other].floor;
            /* Only the selected door surface needs its normalized U. Avoid
             * touching far sector records and dividing every rejected edge. */
            const uint8_t hit_u = door ? game_door_u(line, x, y, dx, dy) : 0u;
            const unsigned upper_texture = side ? side->upper : 0u;
            const unsigned lower_texture = side ? side->lower : 0u;
            const int upper_anchor = line->flags & 8u ? current_ceiling
                : ceiling + (int)game_texture_height(upper_texture);
            const int lower_anchor = line->flags & 16u ? current_ceiling : floor;
            if (ceiling <= floor) {
                /* A closed raised sector can have two different materials,
                 * e.g. E1M2 computer wall above brown stone. Do not stretch
                 * the upper texture across both tiers. Clamp their common
                 * split so negative headroom never produces overlapping spans. */
                const int join = ceiling < current_floor ? current_floor
                    : ceiling > current_ceiling ? current_ceiling : ceiling;
                if (join < current_ceiling) {
                    game_add_span(ray, game_project_y(current_ceiling, eye, safe_depth),
                                   game_project_y(join, eye, safe_depth),
                                   safe_depth, door ? 255u : (unsigned)base,
                                   door ? hit_u : brick_u, clip_top, clip_bottom);
                    game_texture_span(ray, before, upper_texture, world_u, upper_anchor + row_offset, eye);
                }
                before = game_span_count[ray];
                if (join > current_floor) {
                    game_add_span(ray, game_project_y(join, eye, safe_depth),
                                   game_project_y(current_floor, eye, safe_depth),
                                   safe_depth, (unsigned)base, brick_u, clip_top, clip_bottom);
                    game_texture_span(ray, before, lower_texture, world_u, lower_anchor + row_offset, eye);
                }
                if (game_span_count[ray] >= GAME_RAY_SPANS)
                    game_add_fallback(ray, incoming_top, incoming_bottom, safe_depth,
                                      (unsigned)base, brick_u);
                GAME_SET_DEPTH(ray, safe_depth);
                return;
            }
            if (ceiling < current_ceiling) {
                game_add_span(ray, game_project_y(current_ceiling, eye, safe_depth),
                               top, safe_depth, door ? 255u : (unsigned)base,
                               door ? hit_u : brick_u, clip_top, clip_bottom);
                game_texture_span(ray, before, upper_texture, world_u, upper_anchor + row_offset, eye);
            }
            before = game_span_count[ray];
            if (floor > current_floor) {
                game_add_span(ray, bottom, game_project_y(current_floor, eye, safe_depth),
                               safe_depth, (unsigned)base, brick_u, clip_top, clip_bottom);
                game_texture_span(ray, before, lower_texture, world_u, lower_anchor + row_offset, eye);
            }
            if (top > clip_top) clip_top = top;
            if (bottom < clip_bottom) clip_bottom = bottom;
            if (side && side->middle && clip_top < clip_bottom) {
                /* A two-sided middle is a transparent railing/grate, never
                 * a terminal opaque wall. Keep tracing the sector behind it. */
                const unsigned texture = side->middle;
                const int anchor = (line->flags & 16u
                    ? (floor > current_floor ? floor : current_floor) + (int)game_texture_height(texture)
                    : (ceiling < current_ceiling ? ceiling : current_ceiling)) + row_offset;
                before = game_span_count[ray];
                game_add_span(ray, game_project_y(anchor, eye, safe_depth),
                               game_project_y(anchor - (int)game_texture_height(texture), eye, safe_depth),
                               safe_depth, GAME_WALL_MASKED, 0u, clip_top, clip_bottom);
                game_texture_span(ray, before, texture, world_u, anchor, eye);
            }
            if (game_span_count[ray] >= GAME_RAY_SPANS) {
                game_add_fallback(ray, incoming_top, incoming_bottom, safe_depth,
                                  (unsigned)base, brick_u);
                GAME_SET_DEPTH(ray, safe_depth);
                return;
            }
            if (clip_top >= clip_bottom) {
                GAME_SET_DEPTH(ray, safe_depth);
                return;
            }
            if (++boundaries >= GAME_RAY_MAX_BOUNDARIES ||
                game_span_count[ray] >= GAME_RAY_SPANS - 1u) {
                game_add_fallback(ray, incoming_top, incoming_bottom, safe_depth,
                                  (unsigned)base, brick_u);
                GAME_SET_DEPTH(ray, safe_depth);
                return;
            }
            sector = other;
            previous_line = hit_line;
            interval = nearest + 1u;
        }
        if (next_x < next_y) {
            entry = next_x; gx += step_x; next_x += delta_x;
        } else {
            entry = next_y; gy += step_y; next_y += delta_y;
        }
    }
}

static unsigned game_span_code(const GameWallSpan *span, unsigned y) {
    const unsigned texture_v = (span->texture_v +
        (y - span->top) * span->texture_step) >> 8;
    return game_world_code(&game_world_textures[span->texture], span->texture_u, texture_v);
}

static int game_span_opaque(const GameWallSpan *span, unsigned y) {
    return span->color != GAME_WALL_MASKED || game_span_code(span, y) != 0u;
}

static uint8_t game_span_gray(const GameWallSpan *span, unsigned y) {
    if (span->color == 255u && !span->texture)
        return door_panel_color(y, span->top, span->bottom, span->u);
    const unsigned height = span->bottom - span->top;
    const unsigned v = y - span->top;
    unsigned base = span->color;
    if (span->color >= GAME_WALL_USED_SWITCH && span->color <= GAME_WALL_SWITCH) {
        base = 142u + span->depth / 28u;
        if (base > 202u) base = 202u;
        const unsigned sign_depth = span->depth < 12u ? 12u : span->depth;
        const unsigned plate_height = 20u * GAME_VERTICAL_PROJECTION / sign_depth;
        const int top = (int)DOOM_GAME_VIEW_H / 2 - (int)plate_height / 2;
        if (plate_height >= 8u && span->u >= 48u && span->u <= 208u &&
            (int)y >= top && (int)y < top + (int)plate_height) {
            if (span->u < 56u || span->u > 200u || (int)y == top ||
                (int)y + 1 == top + (int)plate_height)
                return 18u;
            static const uint16_t signs[3][5] = {
                {0x77u, 0x41u, 0x67u, 0x44u, 0x47u}, /* F2 */
                {0x7577u, 0x4522u, 0x6222u, 0x4522u, 0x7572u}, /* EXIT */
                {0x75u, 0x56u, 0x54u, 0x56u, 0x75u}, /* OK */
            };
            const unsigned sign = span->color == GAME_WALL_EXIT ? 1u
                : span->color == GAME_WALL_USED_SWITCH ? 2u : 0u;
            const unsigned columns = sign == 1u ? 15u : 7u;
            const unsigned padding = plate_height / 7u > 1u ? plate_height / 7u : 1u;
            const unsigned letter_height = plate_height - padding * 2u;
            if (span->u >= 80u && span->u < 176u &&
                (int)y >= top + (int)padding &&
                (int)y < top + (int)(padding + letter_height)) {
                const unsigned column = (span->u - 80u) * columns / 96u;
                unsigned row = (unsigned)((int)y - top - (int)padding) * 5u / letter_height;
                if (row > 4u) row = 4u;
                if (signs[sign][row] & (1u << (columns - 1u - column))) return 12u;
            }
            return sign == 2u ? 213u : 246u;
        }
    }
    if (span->texture) {
        const unsigned code = game_span_code(span, y);
        return code ? game_world_gray[code] : 237u;
    }
    const unsigned course = v >= height * 3u / 4u ? 3u
        : v >= height / 2u ? 2u : v >= height / 4u ? 1u : 0u;
    const unsigned u = (span->u + ((course & 1u) ? 32u : 0u)) & 63u;
    const int joint = height >= 24u &&
        (v == height / 4u || v == height / 2u || v == height * 3u / 4u);
    if (height >= 16u && (joint || u < 4u)) return (uint8_t)(base - 72u);
    if (height >= 16u && u < 7u) return (uint8_t)(base + 18u);
    return (uint8_t)base;
}

static inline void game_paint_wall_tile(uint8_t *pixels, unsigned ray,
                                        unsigned first_row, unsigned end_row,
                                        uint8_t contrast) {
    /* Opaque tiers do not overlap, while middle railings do. Paint far to
     * near so their holes retain the correct wall or fallback behind. */
    for (unsigned i = game_span_count[ray]; i > 0u; --i) {
        const GameWallSpan *span = &game_spans[game_span_first[ray] + (i - 1u)];
        if (span->color == GAME_WALL_PLANE) continue;
        const unsigned top = span->top > first_row ? span->top : first_row;
        const unsigned bottom = span->bottom < end_row ? span->bottom : end_row;
        unsigned drawn = 0u;
        for (unsigned y = top; y < bottom; ++y) {
            if (!game_span_opaque(span, y)) continue;
            const uint8_t color = game_gray(game_span_gray(span, y), contrast);
            uint8_t *destination = pixels + y * LITE_W + ray * 2u;
            destination[0] = color;
            destination[1] = color;
            drawn += span->texture ? 2u : 0u;
        }
        game_render_stats.world_texture_pixels += drawn;
    }
}

void DoomLite_RenderGameScene(uint8_t *pixels, const DoomLiteGame *game) {
    game_wall_ready = 0u;
    memset(&game_render_stats, 0, sizeof(game_render_stats));
    if (!pixels || !game || !game->map) return;
    const uint16_t sector = DoomLiteGame_PlayerSector(game);
    const int eye = DoomLiteGame_FloorHeight(game, sector) + 41;
    const uint32_t ray_start = game_render_now();
    unsigned span_cursor = 0u;
    for (unsigned ray = 0; ray < LITE_RAYS; ++ray) {
        const int offset = ((int)ray * 168) / (int)(LITE_RAYS - 1u) - 84;
        game_span_first[ray] = (uint16_t)span_cursor;
        game_cast_spans(game, ray, (uint16_t)((int)game->facing * 4 + offset),
                         offset, sector, eye);
        span_cursor += game_span_count[ray];
    }
    game_render_stats.span_cache_bytes = (uint32_t)(span_cursor * sizeof(GameWallSpan));
    game_render_stats.ray_us = game_render_now() - ray_start;
#ifdef DOOM_LITE_ROW_REFERENCE
    /* Portable test baseline for byte-identical tile rendering comparisons. */
    for (unsigned y = 0; y < DOOM_GAME_VIEW_H; ++y) {
        uint8_t *row = pixels + y * LITE_W;
        const uint32_t plane_start = game_render_now();
        game_background_row(row, y, game, sector);
        game_render_stats.plane_us += game_render_now() - plane_start;
        const uint32_t wall_start = game_render_now();
        for (unsigned ray = 0; ray < LITE_RAYS; ++ray) {
            for (unsigned i = 0; i < game_span_count[ray]; ++i) {
                const GameWallSpan *span = &game_spans[game_span_first[ray] + (i)];
                if (span->color == GAME_WALL_PLANE) continue;
                if (y >= span->top && y < span->bottom && game_span_opaque(span, y)) {
                    const uint8_t color = game_gray(game_span_gray(span, y), game->contrast);
                    row[ray * 2u] = color;
                    row[ray * 2u + 1u] = color;
                    game_render_stats.world_texture_pixels += span->texture ? 2u : 0u;
                    break;
                }
            }
        }
        game_render_stats.wall_us += game_render_now() - wall_start;
    }
#else
    /* Paint eight-row tiles. Spans already partition each ray's vertical
     * window, so there is no need to search every span at every pixel. The
     * framebuffer writes stay within a 2 KiB tile rather than walking the
     * entire 26 KiB view once per column (important for the paged 39gII). */
    for (unsigned first_row = 0u; first_row < DOOM_GAME_VIEW_H; first_row += 8u) {
        const unsigned end_row = first_row + 8u < DOOM_GAME_VIEW_H
            ? first_row + 8u : DOOM_GAME_VIEW_H;
        const uint32_t plane_start = game_render_now();
        for (unsigned y = first_row; y < end_row; ++y) {
            game_background_row(pixels + y * LITE_W, y, game, sector);
        }
        game_render_stats.plane_us += game_render_now() - plane_start;
        const uint32_t wall_start = game_render_now();
        for (unsigned ray = 0u; ray < LITE_RAYS; ++ray)
            game_paint_wall_tile(pixels, ray, first_row, end_row, game->contrast);
        game_render_stats.wall_us += game_render_now() - wall_start;
    }
#endif
    pixels[(DOOM_GAME_VIEW_H / 2u) * LITE_W + LITE_W / 2u] = 12u;
    pixels[(DOOM_GAME_VIEW_H / 2u) * LITE_W + LITE_W / 2u - 1u] = 12u;
    game_wall_ready = 1u;
}

static unsigned game_sprite_rotation(const DoomLiteGame *game, const DoomLiteVisual *visual) {
    /* WAD rotation 1 is the actor's front. Select from the actor-to-viewer
     * vector, independent of the player's look direction. Equivalent to the
     * original Doom R_ProjectSprite nearest-octant rule, with integer math. */
    return DoomLiteGame_Direction8(game->x_q8 - visual->x_q8,
                                  game->y_q8 - visual->y_q8, visual->facing);
}

static int game_sprite_for_actor(uint16_t type, unsigned state, unsigned frame,
                                 unsigned rotation) {
    if (type == 58u) type = 3002u;
    for (unsigned i = 0; i < 4u; ++i) {
        const GameSpriteSequence *sequence = &game_sprite_sequences[i];
        if (sequence->thing_type != type) continue;
        if (!sequence->count[state < 4u ? state : 3u]) return -1;
        if (state >= 4u) {
            state = 3u;
            frame = sequence->count[state] - 1u;
        }
        if (state == 3u && frame >= sequence->count[state])
            frame = sequence->count[state] - 1u;
        else frame %= sequence->count[state];
        return sequence->frame[state][frame][rotation & 7u];
    }
    return -1;
}

static int game_sprite_for_item(uint16_t type) {
    unsigned first = 0u, end = GAME_STATIC_SPRITE_COUNT;
    while (first < end) {
        const unsigned mid = first + (end - first) / 2u;
        if (game_static_sprites[mid].thing_type < type) first = mid + 1u;
        else if (game_static_sprites[mid].thing_type > type) end = mid;
        else return game_static_sprites[mid].sprite;
    }
    return -1;
}

typedef struct {
    uint16_t depth;
    int16_t left, top;
    uint16_t texture;
    uint8_t width, height, spectre, flip;
} AnimatedGameSprite;

static unsigned game_sprite_source_x(const AnimatedGameSprite *sprite,
                                    const GameSpriteAsset *asset, unsigned x) {
    const unsigned source = x * asset->width / sprite->width;
    return sprite->flip ? asset->width - 1u - source : source;
}

static void game_queue_sprite(AnimatedGameSprite sprites[LITE_SPRITE_LIMIT],
                              unsigned *count, const DoomLiteGame *game,
                              int32_t x, int32_t y, uint16_t sector, int eye,
                              int texture, int spectre) {
    if (texture < 0) return;
    const unsigned flip = ((unsigned)texture & GAME_SPRITE_FLIP) != 0u;
    texture &= GAME_SPRITE_INDEX_MASK;
    if (texture >= (int)GAME_SPRITE_COUNT) return;
    ++game_render_stats.sprite_candidates;
    const int32_t dx = x / 256 - game->x_q8 / 256;
    const int32_t dy = y / 256 - game->y_q8 / 256;
    if (dx < -768 || dx > 768 || dy < -768 || dy > 768) return;
    const int32_t fx = sine_q14((uint8_t)(game->facing + 64u));
    const int32_t fy = sine_q14(game->facing);
    const int32_t depth = (dx * fx + dy * fy) >> 14;
    if (depth < 12 || depth > 768) return;
    const int32_t side = (-dx * fy + dy * fx) >> 14;
    const int center = LITE_W / 2 + side * LITE_PROJECTION / depth;
    const GameSpriteAsset *asset = &game_sprites[texture];
    int width = asset->world_width * GAME_VERTICAL_PROJECTION / depth;
    int height = asset->world_height * GAME_VERTICAL_PROJECTION / depth;
    if (width < 2) width = 2;
    if (height < 2) height = 2;
    /* Reduce near-camera sprites proportionally before allocating the small
     * X-sample scratch array. The physical patch's anchor remains grounded. */
    if (width > (int)GAME_SPRITE_MAX_WIDTH) {
        height = height * GAME_SPRITE_MAX_WIDTH / width;
        width = GAME_SPRITE_MAX_WIDTH;
    }
    if (height > 126) { width = width * 126 / height; height = 126; }
    if (width < 1) width = 1;
    /* A mirrored patch reflects its origin as well as its texels. WAD patch
     * offsets are not necessarily half the width, so centering only the
     * unmirrored offset makes paired directions jump sideways. */
    const int left_offset = flip ? (int)asset->world_width - asset->left : asset->left;
    const int left = center - left_offset * width / asset->world_width;
    const int base = game_project_y(DoomLiteGame_FloorHeight(game, sector), eye, (unsigned)depth);
    const int top = base - asset->top * height / asset->world_height;
    if (left + width <= 0 || left >= LITE_W || top + height <= 0 ||
        top >= (int)DOOM_GAME_VIEW_H) return;
    /* Keep the nearest 32 sprites: a distant filled list must not hide an
     * approaching enemy. Draw far-to-near to handle sprite overlap. */
    if (*count == LITE_SPRITE_LIMIT) {
        if (depth >= sprites[0].depth) return;
        memmove(sprites, sprites + 1u, (LITE_SPRITE_LIMIT - 1u) * sizeof(*sprites));
        --*count;
    }
    unsigned at = (*count)++;
    while (at && sprites[at - 1u].depth < depth) {
        sprites[at] = sprites[at - 1u]; --at;
    }
    sprites[at] = (AnimatedGameSprite){.depth = (uint16_t)depth, .left = (int16_t)left,
                                     .top = (int16_t)top, .texture = (uint16_t)texture,
                                     .width = (uint8_t)width, .height = (uint8_t)height,
                                     .spectre = (uint8_t)spectre, .flip = (uint8_t)flip};
}

void DoomLite_RenderGameThings(uint8_t *pixels, const DoomLiteGame *game) {
    if (!game_wall_ready || !pixels || !game || !game->map) return;
    AnimatedGameSprite sprites[LITE_SPRITE_LIMIT];
    unsigned count = 0u;
    const int eye = DoomLiteGame_FloorHeight(game, DoomLiteGame_PlayerSector(game)) + 41;
    for (unsigned i = 0; i < DoomLiteGame_VisualCount(game); ++i) {
        DoomLiteVisual visual;
        if (!DoomLiteGame_GetVisual(game, i, &visual)) continue;
        const int texture = game_sprite_for_actor(visual.thing_type, visual.state, visual.frame,
                                                 game_sprite_rotation(game, &visual));
        game_queue_sprite(sprites, &count, game, visual.x_q8, visual.y_q8,
                           visual.sector, eye, texture,
                           visual.thing_type == 58u);
    }
    for (unsigned i = 0; i < game->map->thing_count; ++i) {
        const DoomMapThing *thing = &game->map->things[i];
        const int texture = game_sprite_for_item(thing->type);
        if (texture < 0 || !DoomLiteGame_ThingActive(game, i)) continue;
        game_queue_sprite(sprites, &count, game, (int32_t)thing->x * 256,
                           (int32_t)thing->y * 256, thing->sector, eye, texture, 0);
    }
    for (unsigned i = 0; i < count; ++i) {
        const AnimatedGameSprite *sprite = &sprites[i];
        const GameSpriteAsset *asset = &game_sprites[sprite->texture];
        const int first_x = sprite->left < 0 ? 0 : sprite->left;
        const int last_x = sprite->left + sprite->width < LITE_W
            ? sprite->left + sprite->width : LITE_W;
        const int first_y = sprite->top < 0 ? 0 : sprite->top;
        const int last_y = sprite->top + sprite->height < (int)DOOM_GAME_VIEW_H
            ? sprite->top + sprite->height : (int)DOOM_GAME_VIEW_H;
        uint8_t source_x[GAME_SPRITE_MAX_WIDTH];
        for (int x = first_x; x < last_x; ++x)
            source_x[x - sprite->left] = (uint8_t)game_sprite_source_x(sprite, asset,
                                                                    (unsigned)(x - sprite->left));
        const unsigned step_y = ((unsigned)asset->height << 16) / sprite->height;
        unsigned source_y = (unsigned)(first_y - sprite->top) * step_y;
        for (int y = first_y; y < last_y; ++y, source_y += step_y) {
            const unsigned source_row = (source_y >> 16) * asset->width;
            for (int x = first_x; x < last_x; ++x) {
                const unsigned ray = (unsigned)x / 2u;
                int visible = 1;
                for (unsigned span = 0; span < game_span_count[ray]; ++span) {
                    const GameWallSpan *wall = &game_spans[game_span_first[ray] + (span)];
                    if (y >= wall->top && y < wall->bottom &&
                        sprite->depth + 2u >= wall->depth &&
                        game_span_opaque(wall, (unsigned)y)) { visible = 0; break; }
                }
                if (!visible) continue;
                const unsigned index = source_row + source_x[x - sprite->left];
                const unsigned code = (asset->pixels[index >> 1] >> ((index & 1u) * 4u)) & 15u;
                if (!code) continue;
                uint8_t color = game_sprite_gray[code];
                if (sprite->spectre) color = (uint8_t)(70u + color * 2u / 3u);
                pixels[y * LITE_W + x] = game_gray(color, game->contrast);
                ++game_render_stats.sprite_pixels;
            }
        }
    }
}

typedef struct { int ox, oy; unsigned scale, divisor; const DoomMap *map; } GameMapView;

static int game_map_x(const GameMapView *view, int32_t x) {
    return (x - view->map->grid_x0) * (int)view->scale /
           (32 * (int)view->divisor) + view->ox;
}
static int game_map_y(const GameMapView *view, int32_t y) {
    return (y - view->map->grid_y0) * (int)view->scale /
           (32 * (int)view->divisor) + view->oy;
}
static void game_map_pixel(uint8_t *pixels, int x, int y, uint8_t color) {
    if (x >= 0 && x < LITE_W && y >= 0 && y < (int)DOOM_GAME_VIEW_H)
        pixels[y * LITE_W + x] = color;
}
static void game_map_line(uint8_t *pixels, int x0, int y0, int x1, int y1,
                          uint8_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (unsigned bound = 0; bound < 2048u; ++bound) {
        game_map_pixel(pixels, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        const int twice = error * 2;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
}

static void game_key_letter(uint8_t *pixels, int x, int y, unsigned key) {
    static const uint8_t letters[3][5] = {
        {6u,5u,6u,5u,6u}, /* B */
        {5u,5u,2u,2u,2u}, /* Y */
        {6u,5u,6u,5u,5u}, /* R */
    };
    for (unsigned row = 0; row < 5u; ++row)
        for (unsigned col = 0; col < 3u; ++col)
            if (letters[key][row] & (1u << (2u - col)))
                game_map_pixel(pixels, x + (int)col - 1, y + (int)row - 2, 0u);
}

static void game_map_use_symbol(uint8_t *pixels, int x, int y,
                                unsigned marker, int used) {
    static const uint8_t letters[2][5] = {
        {6u,5u,5u,5u,6u}, /* D: manual door */
        {7u,4u,7u,1u,7u}, /* S: remote switch */
    };
    const uint8_t ink = used ? 136u : 12u;
    for (int dy = -5; dy <= 5; ++dy)
        for (int dx = -5; dx <= 5; ++dx)
            game_map_pixel(pixels, x + dx, y + dy,
                            dx == -5 || dx == 5 || dy == -5 || dy == 5 ? ink : 250u);
    for (unsigned row = 0; row < 5u; ++row)
        for (unsigned column = 0; column < 3u; ++column)
            if (letters[marker == 2u][row] & (1u << (2u - column)))
                game_map_pixel(pixels, x + (int)column - 1, y + (int)row - 2, ink);
}

void DoomLite_RenderGameMap(uint8_t *pixels, const DoomLiteGame *game, unsigned zoom) {
    game_wall_ready = 0u;
    memset(&game_render_stats, 0, sizeof(game_render_stats));
    if (!pixels || !game || !game->map) return;
    const DoomMap *map = game->map;
    GameMapView view = {(LITE_W - (int)map->grid_width) / 2, 0, 1u, 1u, map};
    /* Both maps fit above the classic bottom bar. Leave nine pixels around
     * the overview so eight-pixel key marks at the level edges stay complete. */
    const unsigned map_width = LITE_W - 18u;
    const unsigned map_height = DOOM_GAME_VIEW_H - 18u;
    if (map->grid_height > map_height) {
        view.scale = map_height;
        view.divisor = map->grid_height;
    }
    if (map->grid_width * view.scale > map_width * view.divisor) {
        view.scale = map_width;
        view.divisor = map->grid_width;
    }
    view.ox = (LITE_W - (int)(map->grid_width * view.scale / view.divisor)) / 2;
    view.oy = ((int)DOOM_GAME_VIEW_H - (int)(map->grid_height * view.scale / view.divisor)) / 2;
    if (zoom >= 2u) {
        view.scale = 2u;
        view.divisor = 1u;
        view.ox = LITE_W / 2 - (game->x_q8 / 256 - map->grid_x0) / 16;
        view.oy = (int)DOOM_GAME_VIEW_H / 2 - (game->y_q8 / 256 - map->grid_y0) / 16;
        if (map->grid_width * 2u <= LITE_W)
            view.ox = (LITE_W - (int)map->grid_width * 2) / 2;
        else {
            if (view.ox > 0) view.ox = 0;
            if (view.ox < LITE_W - (int)map->grid_width * 2)
                view.ox = LITE_W - (int)map->grid_width * 2;
        }
        if (view.oy > 0) view.oy = 0;
        if (view.oy < (int)DOOM_GAME_VIEW_H - (int)map->grid_height * 2)
            view.oy = (int)DOOM_GAME_VIEW_H - (int)map->grid_height * 2;
    }
    memset(pixels, 245, LITE_W * DOOM_GAME_VIEW_H);
    for (unsigned i = 0; i < map->line_count; ++i) {
        const DoomMapLine *line = &map->lines[i];
        uint8_t color = 170u;
        int draw_line = 1;
        if (line->back_sector == DOOM_MAP_NO_SECTOR || line->front_sector == DOOM_MAP_NO_SECTOR)
            color = 20u;
        else {
            const int front_floor = DoomLiteGame_FloorHeight(game, line->front_sector);
            const int back_floor = DoomLiteGame_FloorHeight(game, line->back_sector);
            const int front_ceil = DoomLiteGame_CeilingHeight(game, line->front_sector);
            const int back_ceil = DoomLiteGame_CeilingHeight(game, line->back_sector);
            if (front_ceil <= front_floor || back_ceil <= back_floor) color = 0u;
            else if (front_floor != back_floor || front_ceil != back_ceil) color = 96u;
            else draw_line = 0;
        }
        if (draw_line)
            game_map_line(pixels, game_map_x(&view, line->x1), game_map_y(&view, line->y1),
                           game_map_x(&view, line->x2), game_map_y(&view, line->y2), color);
        const unsigned marker = game_use_marker(line->special);
        if (marker == 3u) {
            const int x = game_map_x(&view, ((int32_t)line->x1 + line->x2) / 2);
            const int y = game_map_y(&view, ((int32_t)line->y1 + line->y2) / 2);
            for (int dy = -6; dy <= 6; ++dy)
                for (int dx = -6; dx <= 6; ++dx) game_map_pixel(pixels, x + dx, y + dy, 250u);
            for (int arm = -5; arm <= 5; ++arm) {
                game_map_pixel(pixels, x + arm, y + arm, 0u);
                game_map_pixel(pixels, x + arm + 1, y + arm, 0u);
                game_map_pixel(pixels, x + arm, y - arm, 0u);
                game_map_pixel(pixels, x + arm + 1, y - arm, 0u);
            }
        } else if (marker) {
            const int x = game_map_x(&view, ((int32_t)line->x1 + line->x2) / 2);
            const int y = game_map_y(&view, ((int32_t)line->y1 + line->y2) / 2);
            game_map_use_symbol(pixels, x, y, marker,
                               (game->triggered[i >> 3] >> (i & 7u)) & 1u);
        }
    }
    for (unsigned i = 0; i < map->thing_count; ++i) {
        const DoomMapThing *thing = &map->things[i];
        if (!DoomLiteGame_ThingActive(game, i)) continue;
        for (unsigned key = 0; key < 3u; ++key) {
            if (thing->type != game_key_types[key]) continue;
            const int x = game_map_x(&view, thing->x), y = game_map_y(&view, thing->y);
            for (int dy = -8; dy <= 8; ++dy)
                for (int dx = -8; dx <= 8; ++dx) {
                    const int d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
                    if (d <= 8) game_map_pixel(pixels, x + dx, y + dy, d >= 6 ? 0u : 250u);
                }
            game_key_letter(pixels, x, y, key);
        }
    }
    const int px = game_map_x(&view, game->x_q8 / 256);
    const int py = game_map_y(&view, game->y_q8 / 256);
    for (int dy = -3; dy <= 3; ++dy)
        for (int dx = -3; dx <= 3; ++dx) game_map_pixel(pixels, px + dx, py + dy, 250u);
    const int heading_x = sine_q14((uint8_t)(game->facing + 64u));
    const int heading_y = sine_q14(game->facing);
    for (int distance = 2; distance <= 8; ++distance)
        game_map_pixel(pixels, px + heading_x * distance / 16384,
                       py + heading_y * distance / 16384, 0u);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) game_map_pixel(pixels, px + dx, py + dy, 0u);
}
