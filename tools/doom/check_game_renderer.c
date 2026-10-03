/* Portable native renderer verification. Compile with DoomLiteGame.c and
 * DoomMap.c; host timings are diagnostic only and are NOT ARM FPS estimates.
 * Optional argument: preview directory (existing directory).
 */
#define DOOM_LITE_RAY_TEST
#define LCD_PIX_W 256
#define LCD_PIX_H 127
#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include "../../System/applications/user/doom_lite/DoomLite.c"
#include "../../System/applications/user/doom_lite/DoomLiteHud.c"

static unsigned frames_checked;
static uint64_t total_line_tests, total_cells, total_surfaces;
static unsigned max_line_tests, max_cells, max_surfaces, surface_limit_frames;
static unsigned max_limit_pixels, total_limit_hits, total_limit_pixels;

static uint32_t frame_hash(const uint8_t *pixels) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < DOOM_GAME_VIEW_H * LCD_PIX_W; ++i)
        hash = (hash ^ pixels[i]) * 16777619u;
    return hash;
}
static void guard(const uint8_t *frame) {
    for (unsigned i = 0; i < 16u; ++i) {
        assert(frame[i] == 0xa5u);
        assert(frame[16u + LCD_PIX_W * LCD_PIX_H + i] == 0xa5u);
    }
}
static void check_packing(void) {
    unsigned cursor = 0u;
    for (unsigned ray = 0u; ray < LITE_RAYS; ++ray) {
        assert(game_span_first[ray] == cursor);
        assert(game_span_count[ray] <= GAME_RAY_SPANS);
        cursor += game_span_count[ray];
        assert(cursor <= LITE_RAYS * GAME_RAY_SPANS);
    }
    assert(game_render_stats.surfaces == cursor);
    assert(game_render_stats.span_cache_bytes == cursor * sizeof(GameWallSpan));
}
static void check_unused_span_tail(uint8_t *frame, const DoomLiteGame *game) {
    memset(frame, 0xa5, LCD_PIX_W * LCD_PIX_H + 32u);
    DoomLite_RenderGameScene(frame + 16u, game);
    check_packing();
    const uint32_t before = frame_hash(frame + 16u);
    const unsigned active = game_render_stats.span_cache_bytes / sizeof(GameWallSpan);
    assert(active < LITE_RAYS * GAME_RAY_SPANS);
    uint8_t *tail = (uint8_t *)&game_spans[active];
    const unsigned unused = (unsigned)sizeof(game_spans) - game_render_stats.span_cache_bytes;
    memset(tail, 0xa7, unused);
    DoomLite_RenderGameScene(frame + 16u, game);
    check_packing();
    assert(game_render_stats.span_cache_bytes == active * sizeof(GameWallSpan));
    assert(frame_hash(frame + 16u) == before);
    for (unsigned i = 0u; i < unused; ++i) assert(tail[i] == 0xa7u);
    guard(frame);
}
static void check_tile_pixels(const uint8_t *pixels, const DoomLiteGame *game) {
    uint8_t reference[DOOM_GAME_VIEW_H * LCD_PIX_W];
    for (unsigned y = 0; y < DOOM_GAME_VIEW_H; ++y) {
        game_background_row(reference + y * LCD_PIX_W, y, game,
                            DoomLiteGame_PlayerSector(game));
        for (unsigned ray = 0; ray < LITE_RAYS; ++ray) {
            for (unsigned i = 0; i < game_span_count[ray]; ++i) {
                const GameWallSpan *span = &game_spans[game_span_first[ray] + (i)];
                if (span->color == GAME_WALL_PLANE) continue;
                if (y >= span->top && y < span->bottom && game_span_opaque(span, y)) {
                    const uint8_t color = game_gray(game_span_gray(span, y), game->contrast);
                    reference[y * LCD_PIX_W + ray * 2u] = color;
                    reference[y * LCD_PIX_W + ray * 2u + 1u] = color;
                    break;
                }
            }
        }
    }
    reference[(DOOM_GAME_VIEW_H / 2u) * LITE_W + LITE_W / 2u] = 12u;
    reference[(DOOM_GAME_VIEW_H / 2u) * LITE_W + LITE_W / 2u - 1u] = 12u;
    assert(memcmp(pixels, reference, sizeof(reference)) == 0);
}
static void checked_frame(uint8_t *frame, DoomLiteGame *game, int map) {
    memset(frame, 0xa5, LCD_PIX_W * LCD_PIX_H + 32u);
    if (map) DoomLite_RenderGameMap(frame + 16u, game, (unsigned)map);
    else {
        DoomLite_RenderGameScene(frame + 16u, game);
        check_packing();
        check_tile_pixels(frame + 16u, game);
        for (unsigned i = DOOM_GAME_VIEW_H * LCD_PIX_W;
             i < LCD_PIX_W * LCD_PIX_H; ++i) assert(frame[16u + i] == 0xa5u);
        DoomLite_RenderGameThings(frame + 16u, game);
        for (unsigned i = DOOM_GAME_VIEW_H * LCD_PIX_W;
             i < LCD_PIX_W * LCD_PIX_H; ++i) assert(frame[16u + i] == 0xa5u);
        DoomLite_DrawGameWeapon(frame + 16u, game);
    }
    DoomLite_DrawGameHud(frame + 16u, game, NULL, (unsigned)map);
    guard(frame);
    const DoomLiteRenderStats *stats = DoomLite_GetRenderStats();
    assert(stats->line_tests < 150000u);
    assert(stats->surfaces <= LITE_RAYS * GAME_RAY_SPANS);
    total_line_tests += stats->line_tests;
    total_cells += stats->cells;
    total_surfaces += stats->surfaces;
    if (max_line_tests < stats->line_tests) max_line_tests = stats->line_tests;
    if (max_cells < stats->cells) max_cells = stats->cells;
    if (max_surfaces < stats->surfaces) max_surfaces = stats->surfaces;
    surface_limit_frames += stats->surface_limit_hits != 0u;
    total_limit_hits += stats->surface_limit_hits;
    total_limit_pixels += stats->surface_limit_pixels;
    if (max_limit_pixels < stats->surface_limit_pixels) max_limit_pixels = stats->surface_limit_pixels;
    ++frames_checked;
}
static void preview(const char *directory, const char *name, const uint8_t *frame) {
    if (!directory) return;
    char path[1024];
    const int length = snprintf(path, sizeof(path), "%s/%s.pgm", directory, name);
    assert(length > 0 && (unsigned)length < sizeof(path));
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P5\n256 127\n255\n");
    assert(fwrite(frame + 16u, 1, LCD_PIX_W * LCD_PIX_H, file) == LCD_PIX_W * LCD_PIX_H);
    assert(fclose(file) == 0);
}
static void check_doors(uint8_t *frame, const char *directory) {
    DoomLiteGame game;
    assert(DoomLiteGame_InitMap(&game, 0u));
    game.x_q8 = 832 * 256;
    game.y_q8 = 384 * 256;
    game.facing = 64u;
    checked_frame(frame, &game, 0);
    const uint32_t closed = frame_hash(frame + 16u);
    preview(directory, "door-closed", frame);
    game.sector_state[10].ceiling = (int16_t)(game.map->sectors[10].floor + 32);
    checked_frame(frame, &game, 0);
    const uint32_t half = frame_hash(frame + 16u);
    preview(directory, "door-half", frame);
    game.sector_state[10].ceiling = (int16_t)(game.map->sectors[10].lowest_ceiling - 4);
    checked_frame(frame, &game, 0);
    const uint32_t open = frame_hash(frame + 16u);
    preview(directory, "door-open", frame);
    assert(closed != half && half != open && closed != open);
    printf("door visuals: closed=%08x half=%08x open=%08x; three distinct frames\n",
           closed, half, open);
}

static void check_actor_frames(uint8_t *frame, const char *directory) {
    DoomLiteGame game;
    assert(DoomLiteGame_InitMap(&game, 1u));
    memset(game.collected, 255, sizeof(game.collected));
    game.actor_count = 1u;
    game.facing = 0u;
    for (unsigned sector = 0; sector < game.map->sector_count; ++sector)
        game.sector_state[sector].floor = 0;
    game.actors[0].x_q8 = game.x_q8 + 128 * 256;
    game.actors[0].y_q8 = game.y_q8;
    for (unsigned family = 0; family < 4u; ++family) {
        unsigned thing = 0u;
        while (thing < game.map->thing_count &&
               game.map->things[thing].type != game_sprite_sequences[family].thing_type)
            ++thing;
        assert(thing < game.map->thing_count);
        game.actors[0].thing_index = (uint16_t)thing;
        uint32_t walk_hash = 0u, corpse_hash = 0u;
        for (unsigned state = 0; state <= DL_ACTOR_CORPSE; ++state) {
            const unsigned count = state >= 4u ? 1u : game_sprite_sequences[family].count[state];
            for (unsigned animation = 0; animation < count; ++animation) {
                game.actors[0].state = (uint8_t)state;
                game.actors[0].frame = (uint8_t)animation;
                memset(frame, 0xa5, LCD_PIX_W * LCD_PIX_H + 32u);
                memset(frame + 16u, 237, LCD_PIX_W * LCD_PIX_H);
                memset(game_span_count, 0, sizeof(game_span_count));
                game_wall_ready = 1u;
                DoomLite_RenderGameThings(frame + 16u, &game);
                guard(frame);
                unsigned drawn = 0u;
                for (unsigned i = 0; i < DOOM_GAME_VIEW_H * LCD_PIX_W; ++i)
                    drawn += frame[16u + i] != 237u;
                assert(drawn > 0u);
                for (unsigned i = DOOM_GAME_VIEW_H * LCD_PIX_W;
                     i < LCD_PIX_W * LCD_PIX_H; ++i) assert(frame[16u + i] == 237u);
                const uint32_t hash = frame_hash(frame + 16u);
                if (state == DL_ACTOR_WALK && !animation) walk_hash = hash;
                if (state == DL_ACTOR_CORPSE) corpse_hash = hash;
                if (directory && (state == DL_ACTOR_WALK || state == DL_ACTOR_CORPSE) && !animation) {
                    char name[64];
                    snprintf(name, sizeof(name), "actor-%u-%s", family,
                             state == DL_ACTOR_WALK ? "walk" : "corpse");
                    preview(directory, name, frame);
                }
            }
        }
        assert(walk_hash != corpse_hash);
        /* A full-height opaque wall must hide the actor including corpse. */
        for (unsigned ray = 0; ray < LITE_RAYS; ++ray) {
            game_span_first[ray] = (uint16_t)ray;
            game_span_count[ray] = 1u;
            game_spans[game_span_first[ray] + (0)] = (GameWallSpan){.depth=8u, .top=0u, .bottom=DOOM_GAME_VIEW_H};
        }
        memset(frame + 16u, 237, LCD_PIX_W * LCD_PIX_H);
        DoomLite_RenderGameThings(frame + 16u, &game);
        for (unsigned i = 0; i < LCD_PIX_W * LCD_PIX_H; ++i)
            assert(frame[16u + i] == 237u);
    }
    printf("actor visuals: four families, every front frame, grounded corpses, wall occlusion passed\n");
}

static void check_sparse_ray_reference(void) {
    unsigned comparisons = 0u;
    for (unsigned map = 0; map < 2u; ++map) {
        DoomLiteGame game;
        assert(DoomLiteGame_InitMap(&game, map));
        /* Flatten all sectors so every two-sided line is a pure portal.
         * The exact all-linedef reference then independently locates the
         * nearest terminal wall without cell/ref/BSP traversal shortcuts.
         * The separate platform check covers actual stepped plane clipping. */
        for (unsigned i = 0; i < game.map->sector_count; ++i) {
            game.sector_state[i].floor = 0;
            game.sector_state[i].ceiling = 256;
        }
        for (unsigned origin = 0; origin < 6u; ++origin) {
            if (origin) {
                const DoomLiteActor *actor = &game.actors[(origin * 3u) % game.actor_count];
                game.x_q8 = actor->x_q8;
                game.y_q8 = actor->y_q8;
            }
            const uint16_t sector = DoomLiteGame_PlayerSector(&game);
            for (unsigned angle = 1u; angle < 1024u; angle += 16u) {
                const int32_t dx = sine_q14_fine((uint16_t)(angle + 256u));
                const int32_t dy = sine_q14_fine((uint16_t)angle);
                uint32_t nearest = LITE_MAX_DISTANCE_Q8;
                for (unsigned i = 0; i < game.map->line_count; ++i) {
                    const DoomMapLine *line = &game.map->lines[i];
                    if (line->front_sector != DOOM_MAP_NO_SECTOR &&
                        line->back_sector != DOOM_MAP_NO_SECTOR) continue;
                    uint8_t u;
                    const uint32_t distance = game_line_distance(line, game.x_q8,
                        game.y_q8, dx, dy, 0u, LITE_MAX_DISTANCE_Q8, 0, &u);
                    if (distance < nearest) nearest = distance;
                }
                game_cast_spans(&game, 0u, (uint16_t)angle, 0, sector, 41);
                unsigned expected = nearest >> 8;
                if (!expected) expected = 1u;
                const unsigned actual = game_wall_depth[0];
                if (actual + 2u < expected || expected + 2u < actual) {
                    fprintf(stderr, "sparse ray differs %s origin=%u angle=%u sector=%u actual=%u reference=%u\n",
                            game.map->name, origin, angle, sector, actual, expected);
                    assert(0);
                }
                ++comparisons;
            }
        }
    }
    printf("sparse ray reference: %u rays over two maps match all-linedef nearest walls\n", comparisons);
}

static void check_items_and_spectre(uint8_t *frame, const char *directory) {
    DoomLiteGame game;
    DoomMap synthetic_map;
    DoomMapThing synthetic;
    for (unsigned item = 0; item < GAME_STATIC_SPRITE_COUNT; ++item) {
        unsigned thing = 0u;
        for (unsigned map = 0; map < 2u; ++map) {
            assert(DoomLiteGame_InitMap(&game, map));
            for (thing = 0u; thing < game.map->thing_count; ++thing)
                if (game.map->things[thing].type == game_static_sprites[item].thing_type &&
                    DoomLiteGame_ThingActive(&game, thing)) break;
            if (thing < game.map->thing_count) break;
        }
        if (thing >= game.map->thing_count) {
            /* Optional pickups/weapons may be absent in both release maps.
             * Render them in a real flat-map geometry to check the same path. */
            assert(game_static_sprites[item].thing_type == 2019u ||
                   game_static_sprites[item].thing_type == 2022u ||
                   game_static_sprites[item].thing_type == 2025u ||
                   (game_static_sprites[item].thing_type >= 2001u &&
                    game_static_sprites[item].thing_type <= 2006u));
            synthetic = (DoomMapThing){.x=192, .y=0, .type=game_static_sprites[item].thing_type,
                                       .options=2u, .sector=0u};
            synthetic_map = *DoomTestMap_Get();
            synthetic_map.things = &synthetic;
            synthetic_map.thing_count = 1u;
            assert(DoomLiteGame_InitData(&game, &synthetic_map));
            thing = 0u;
        }
        game.actor_count = 0u;
        game.facing = 0u;
        memset(game.collected, 255, sizeof(game.collected));
        game.collected[thing >> 3] &= (uint8_t)~(1u << (thing & 7u));
        for (unsigned s = 0; s < game.map->sector_count; ++s)
            game.sector_state[s].floor = 0;
        game.x_q8 = ((int32_t)game.map->things[thing].x - 128) * 256;
        game.y_q8 = (int32_t)game.map->things[thing].y * 256;
        memset(frame, 0xa5, LCD_PIX_W * LCD_PIX_H + 32u);
        memset(frame + 16u, 237, LCD_PIX_W * LCD_PIX_H);
        memset(game_span_count, 0, sizeof(game_span_count));
        game_wall_ready = 1u;
        DoomLite_RenderGameThings(frame + 16u, &game);
        guard(frame);
        unsigned drawn = 0u;
        for (unsigned pixel = 0; pixel < DOOM_GAME_VIEW_H * LCD_PIX_W; ++pixel)
            drawn += frame[16u + pixel] != 237u;
        assert(drawn);
        game.collected[thing >> 3] |= (uint8_t)(1u << (thing & 7u));
        memset(frame + 16u, 237, LCD_PIX_W * LCD_PIX_H);
        DoomLite_RenderGameThings(frame + 16u, &game);
        for (unsigned pixel = 0; pixel < LCD_PIX_W * LCD_PIX_H; ++pixel)
            assert(frame[16u + pixel] == 237u);
    }
    assert(DoomLiteGame_InitMap(&game, 1u));
    unsigned spectre = 0u, demon = 0u;
    while (spectre < game.map->thing_count && game.map->things[spectre].type != 58u) ++spectre;
    while (demon < game.map->thing_count && game.map->things[demon].type != 3002u) ++demon;
    assert(spectre < game.map->thing_count && demon < game.map->thing_count);
    game.actor_count = 1u;
    game.facing = 0u;
    memset(game.collected, 255, sizeof(game.collected));
    for (unsigned s = 0; s < game.map->sector_count; ++s) game.sector_state[s].floor = 0;
    game.actors[0].x_q8 = game.x_q8 + 128 * 256;
    game.actors[0].y_q8 = game.y_q8;
    uint32_t hashes[2];
    for (unsigned kind = 0; kind < 2u; ++kind) {
        game.actors[0].thing_index = (uint16_t)(kind ? spectre : demon);
        memset(frame + 16u, 237, LCD_PIX_W * LCD_PIX_H);
        memset(game_span_count, 0, sizeof(game_span_count));
        game_wall_ready = 1u;
        DoomLite_RenderGameThings(frame + 16u, &game);
        hashes[kind] = frame_hash(frame + 16u);
    }
    assert(hashes[0] != hashes[1]);
    preview(directory, "spectre", frame);
    printf("items/spectre: three keys and twelve current-map survival pickups disappear on collection; spectre shares demon pixels with distinct grayscale\n");
}

static void check_axis_intersections(void) {
    uint32_t random = 0x39d00d42u;
    unsigned axis_tests = 0u, visible_tests = 0u;
    for (unsigned map = 0; map < 2u; ++map) {
        const DoomMap *data = DoomMap_Get(map);
        for (unsigned test = 0; test < 100000u; ++test) {
            random = random * 1664525u + 1013904223u;
            const DoomMapLine *line = &data->lines[random % data->line_count];
            if (line->x1 != line->x2 && line->y1 != line->y2) continue;
            random = random * 1664525u + 1013904223u;
            const uint16_t angle = (uint16_t)(random & 1023u);
            const int32_t dx = sine_q14_fine((uint16_t)(angle + 256u));
            const int32_t dy = sine_q14_fine(angle);
            random = random * 1664525u + 1013904223u;
            const int32_t x = (int32_t)line->x1 * 256 + (int32_t)(random % 262144u) - 131072;
            random = random * 1664525u + 1013904223u;
            const int32_t y = (int32_t)line->y1 * 256 + (int32_t)(random % 262144u) - 131072;
            const uint32_t abs_x = dx < 0 ? (uint32_t)-dx : (uint32_t)dx;
            const uint32_t abs_y = dy < 0 ? (uint32_t)-dy : (uint32_t)dy;
            uint8_t reference_u = 0u, fast_u = 0u;
            const uint32_t reference = game_line_distance(line, x, y, dx, dy,
                0u, LITE_MAX_DISTANCE_Q8, 1, &reference_u);
            const uint32_t fast = game_line_distance_fast(line, x, y, dx, dy,
                0u, LITE_MAX_DISTANCE_Q8, 1, &fast_u,
                abs_x ? UINT32_MAX / abs_x : 0u, abs_y ? UINT32_MAX / abs_y : 0u);
            assert(reference == fast);
            if (reference != UINT32_MAX) {
                assert(reference_u == fast_u);
                assert(reference_u == game_door_u(line, x, y, dx, dy));
                ++visible_tests;
            }
            ++axis_tests;
        }
    }
    printf("axis reciprocal: %u axis intersections (%u visible) match generic distance and door U exactly\n",
           axis_tests, visible_tests);
}

static void check_switch_signs(uint8_t *frame, const char *directory) {
    /* Letter padding must keep the foreground detached from the border.
     * The projected glyph also stays the same when a tall wall is clipped. */
    const unsigned f2[5] = {0x77u,0x41u,0x67u,0x44u,0x47u};
    GameWallSpan sample = {.depth=64u, .top=0u, .bottom=DOOM_GAME_VIEW_H,
                           .color=GAME_WALL_SWITCH, .u=128u};
    for (unsigned row = 0u; row < 5u; ++row) {
        for (unsigned column = 0u; column < 7u; ++column) {
            sample.u = (uint8_t)(80u + (column * 2u + 1u) * 96u / 14u);
            const unsigned y = 35u + row * 6u + 3u;
            const unsigned expected = (f2[row] & (1u << (6u - column))) ? 12u : 246u;
            assert(game_span_gray(&sample, y) == expected);
            sample.top = 20u; sample.bottom = 90u;
            assert(game_span_gray(&sample, y) == expected);
            sample.top = 0u; sample.bottom = DOOM_GAME_VIEW_H;
        }
    }
    const unsigned critical[3] = {34u, 93u, 1904u};
    for (unsigned test = 0; test < 4u; ++test) {
        DoomLiteGame game;
        assert(DoomLiteGame_InitMap(&game, 1u));
        game.actor_count = 0u;
        memset(game.collected, 255, sizeof(game.collected));
        unsigned index;
        if (test < 3u) index = critical[test];
        else {
            for (index = 0u; index < game.map->line_count; ++index)
                if (game.map->lines[index].special == 11u) break;
            assert(index < game.map->line_count);
        }
        const DoomMapLine *line = &game.map->lines[index];
        const int sx = line->x2 - line->x1, sy = line->y2 - line->y1;
        const int axis = (sx < 0 ? -sx : sx) > (sy < 0 ? -sy : sy)
            ? (sx < 0 ? -sx : sx) : (sy < 0 ? -sy : sy);
        unsigned marked = 0u;
        for (unsigned distance = 64u; distance >= 16u && !marked; distance /= 2u) {
            const int nx = sy * (int)distance / axis, ny = -sx * (int)distance / axis;
            game.x_q8 = (((int32_t)line->x1 + line->x2) / 2 + nx) * 256;
            game.y_q8 = (((int32_t)line->y1 + line->y2) / 2 + ny) * 256;
            game.facing = (uint8_t)(nx > 0 ? 128 : nx < 0 ? 0 : ny > 0 ? 192 : 64);
            checked_frame(frame, &game, 0);
            for (unsigned ray = 0u; ray < LITE_RAYS; ++ray)
                for (unsigned span = 0u; span < game_span_count[ray]; ++span)
                    marked += game_spans[game_span_first[ray] + (span)].color == (test == 3u ? GAME_WALL_EXIT : GAME_WALL_SWITCH);
        }
        assert(marked);
        const uint32_t active_hash = frame_hash(frame + 16u);
        char name[64]; snprintf(name, sizeof(name), "e1m2-%s-line-%u", test == 3u ? "exit" : "switch", index);
        preview(directory, name, frame);
        if (test < 3u) {
            game.triggered[index >> 3] |= (uint8_t)(1u << (index & 7u));
            checked_frame(frame, &game, 0);
            assert(active_hash != frame_hash(frame + 16u));
            snprintf(name, sizeof(name), "e1m2-switch-used-line-%u", index);
            preview(directory, name, frame);
        }
        checked_frame(frame, &game, 2);
        snprintf(name, sizeof(name), "e1m2-switch-map-line-%u", index);
        preview(directory, name, frame);
    }
    assert(game_use_marker(2u) == 0u && game_use_marker(97u) == 0u &&
           game_use_marker(120u) == 0u && game_use_marker(88u) == 0u);
    printf("switch visuals: E1M2 linedefs 34/93/1904 have F2/OK plaques, exit has EXIT, map marks only F2 specials; frame guards passed\n");
}

int main(int argc, char **argv) {
    const char *directory = argc > 1 ? argv[1] : NULL;
    uint8_t frame[LCD_PIX_W * LCD_PIX_H + 32u];
    assert(!wall_at_q8(E1M1_START_X * 256, E1M1_START_Y * 256));
    const clock_t begin = clock();
    for (unsigned map = 0; map < 2u; ++map) {
        DoomLiteGame game;
        assert(DoomLiteGame_InitMap(&game, map));
        uint8_t lite_before[LCD_PIX_W * LCD_PIX_H];
        check_unused_span_tail(frame, &game);
        DoomLite_RenderFrame(lite_before, E1M1_START_X * 256, E1M1_START_Y * 256, 0u, 0);
        uint32_t contrast_hash[3];
        for (unsigned contrast = 0; contrast < 3u; ++contrast) {
            game.contrast = (uint8_t)contrast;
            checked_frame(frame, &game, 0);
            contrast_hash[contrast] = frame_hash(frame + 16u);
            char name[64];
            snprintf(name, sizeof(name), "e1m%u-start-c%u", map + 1u, contrast + 1u);
            preview(directory, name, frame);
        }
        assert(contrast_hash[0] != contrast_hash[1] && contrast_hash[1] != contrast_hash[2]);
        game.contrast = 1u;
        unsigned spawn_limits = 0u, spawn_limit_pixels = 0u;
        for (unsigned facing = 0; facing < 256u; facing += 4u) {
            game.facing = (uint8_t)facing;
            checked_frame(frame, &game, 0);
            spawn_limits += DoomLite_GetRenderStats()->surface_limit_hits;
            spawn_limit_pixels += DoomLite_GetRenderStats()->surface_limit_pixels;
            if (DoomLite_GetRenderStats()->surface_limit_hits && directory) {
                char name[64]; snprintf(name, sizeof(name), "e1m%u-limit-facing-%u", map + 1u, facing);
                preview(directory, name, frame);
            }
        }
        printf("GAME_RENDER_SPAWN level=%u headings=64 surface_limit_hits=%u surface_limit_pixels=%u\n",
               map + 1u, spawn_limits, spawn_limit_pixels);
        game.facing = (uint8_t)(game.map->start_angle * 256u / 360u);
        for (unsigned zoom = 1u; zoom <= 2u; ++zoom) {
            checked_frame(frame, &game, (int)zoom);
            char name[64]; snprintf(name, sizeof(name), "e1m%u-map-%u", map + 1u, zoom);
            preview(directory, name, frame);
        }
        /* Sample living actor locations to exercise narrow portals and
         * clipped sprites beyond the initial scene, without fabricated FPS. */
        for (unsigned actor = 0; actor < game.actor_count; actor += 3u) {
            game.x_q8 = game.actors[actor].x_q8;
            game.y_q8 = game.actors[actor].y_q8;
            for (unsigned facing = 0; facing < 256u; facing += 32u) {
                game.facing = (uint8_t)facing;
                checked_frame(frame, &game, 0);
            }
        }
        DoomLite_RenderFrame(frame + 16u, E1M1_START_X * 256, E1M1_START_Y * 256, 0u, 0);
        assert(memcmp(frame + 16u, lite_before, sizeof(lite_before)) == 0);
        printf("%s: start, contrasts, 64 headings, map, actor-location views, Lite isolation passed\n", game.map->name);
    }
    check_doors(frame, directory);
    check_actor_frames(frame, directory);
    printf("GAME_RENDER_CACHE frames=%u surface_limit_frames=%u surface_limit_hits=%u surface_limit_pixels=%u max_limit_pixels=%u span_bss=%u\n",
           frames_checked, surface_limit_frames, total_limit_hits, total_limit_pixels,
           max_limit_pixels, (unsigned)(sizeof(game_spans) + sizeof(game_span_first) + sizeof(game_span_count)));
    check_sparse_ray_reference();
    check_items_and_spectre(frame, directory);
    check_axis_intersections();
    check_switch_signs(frame, directory);
    const double elapsed_ms = 1000.0 * (double)(clock() - begin) / CLOCKS_PER_SEC;
    printf("GAME_RENDER_HOST frames=%u host_cpu_ms=%.3f line_tests=%llu cells=%llu surfaces=%llu max_line_tests=%u max_cells=%u max_surfaces=%u surface_limit_frames=%u surface_limit_hits=%u surface_limit_pixels=%u max_limit_pixels=%u span_bss=%u animation_bytes=%u; NOT_ARM_FPS\n",
           frames_checked, elapsed_ms, (unsigned long long)total_line_tests,
           (unsigned long long)total_cells, (unsigned long long)total_surfaces,
           max_line_tests, max_cells, max_surfaces, surface_limit_frames,
           total_limit_hits, total_limit_pixels, max_limit_pixels,
           (unsigned)(sizeof(game_spans) + sizeof(game_span_first) + sizeof(game_span_count)), GAME_SPRITE_BYTES);
    return 0;
}
