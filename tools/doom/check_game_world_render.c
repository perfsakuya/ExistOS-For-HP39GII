/* Real-map texture/alpha regression checks. No device access or timing claims.
 * Compile with DoomLiteGame.c and DoomMap.c, as for check_game_renderer.c. */
#define DOOM_LITE_RAY_TEST
#define LCD_PIX_W 256
#define LCD_PIX_H 127
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include "../../System/applications/user/doom_lite/DoomLite.c"
#include "../../System/applications/user/doom_lite/DoomLiteHud.c"

static uint8_t scene[LCD_PIX_W * LCD_PIX_H];
static uint8_t actual[LCD_PIX_W * LCD_PIX_H];
static uint8_t unoccluded[LCD_PIX_W * LCD_PIX_H];
static GameWallSpan saved_spans[LITE_RAYS * GAME_RAY_SPANS];
static uint16_t saved_first[LITE_RAYS];
static uint8_t saved_count[LITE_RAYS];

/* Decode the documented asset format independently of game_world_code. */
static unsigned asset_code(const GameWorldTexture *asset, unsigned column,
                           unsigned texture_row) {
    const unsigned offset = (column % 32u) * 16u + (texture_row % 32u) / 2u;
    const unsigned byte = asset->pixels[offset];
    return texture_row % 2u ? byte / 16u : byte % 16u;
}

static unsigned span_asset_code(const GameWallSpan *span, unsigned screen_y) {
    const unsigned texture_row = (span->texture_v +
        (screen_y - span->top) * span->texture_step) / 256u;
    return asset_code(&game_world_textures[span->texture],
                      span->texture_u, texture_row);
}

static void check_v_projection(const GameWallSpan *span, int anchor, int eye) {
    const GameWorldTexture *asset = &game_world_textures[span->texture];
    const unsigned depth = span->depth < 12u ? 12u : span->depth;
    for (unsigned y = span->top; y < span->bottom; ++y) {
        /* Invert the world-to-screen projection in double precision rather
         * than repeating the renderer's integer preparation formula. */
        const double world_v = anchor - eye +
            ((double)y - DOOM_GAME_VIEW_H / 2u) * depth / 128.0;
        double expected = fmod(world_v * (32.0 * 256.0) / asset->world_height, 8192.0);
        if (expected < 0) expected += 8192.0;
        const unsigned cached = (span->texture_v +
            (y - span->top) * span->texture_step) % 8192u;
        double error = fabs(expected - cached);
        if (error > 4096.0) error = 8192.0 - error;
        /* Preparation rounds at most one Q8 unit; each incremental screen
         * row truncates less than one further Q8 unit. */
        assert(error <= (double)(y - span->top) + 2.0);
    }
}

static void check_u_and_clipped_v(void) {
    const DoomMap *map = DoomMap_Get(1u);
    const GameWorldSide *front = game_world_side(map, 307u, 1u);
    const GameWorldSide *back = game_world_side(map, 307u, 0u);
    /* WAD line 307 is (440,-1504)->(440,-1372), length 132;
     * sidedef offsets are +20/-40. This independently fixes both origins. */
    assert(front && back);
    assert(game_world_u(map, 307u, 1u, 440, -1440, front) == 84);
    assert(game_world_u(map, 307u, 0u, 440, -1440, back) == 28);
    assert(game_world_u(map, 307u, 1u, 440, -1504, front) == 20);
    assert(game_world_u(map, 307u, 0u, 440, -1372, back) == -40);
    unsigned diagonal_checks = 0u;
    for (unsigned i = 0; i < map->line_count; ++i) {
        const DoomMapLine *line = &map->lines[i];
        const int dx = line->x2 - line->x1, dy = line->y2 - line->y1;
        if (!dx || !dy) continue;
        const GameWorldSide *side = game_world_side(map, i, 1u);
        if (!side) continue;
        const int hx = (line->x1 + line->x2) / 2;
        const int hy = (line->y1 + line->y2) / 2;
        const double expected = side->x_offset +
            ((hx - line->x1) * (double)dx + (hy - line->y1) * (double)dy) /
            hypot(dx, dy);
        assert(fabs(game_world_u(map, i, 1u, hx, hy, side) - expected) < 1.1);
        ++diagonal_checks;
    }
    assert(diagonal_checks > 0u);
    /* BROWNGRN keeps its 64x128 WAD dimensions. A tall near-camera wall
     * clipped to rows 11..91 must begin inside the texture, not at row zero. */
    const unsigned texture = game_world_side(map, 1388u, 1u)->lower;
    assert(game_world_textures[texture].world_height == 128u);
    game_span_count[0] = 0u;
    game_span_first[0] = 0u;
    game_add_span(0u, -999, 999, 16u, 142u, 0u, 11, 92);
    game_texture_span(0u, 0u, texture, -40, 160, 121);
    const GameWallSpan *span = &game_spans[game_span_first[0] + (0)];
    assert(span->top == 11u && span->bottom == 92u);
    assert(span->texture_u == 12u); /* (-40 mod 64) * 32/64. */
    assert(span->texture_v == 2184u); /* World V=34.125, /128*32*256. */
    check_v_projection(span, 160, 121);
    puts("world alignment: front/back offsets, diagonal U, clipped V passed");
}

static void check_closed_tiers(void) {
    DoomLiteGame game;
    assert(DoomLiteGame_InitMap(&game, 1u));
    game.x_q8 = 528 * 256; game.y_q8 = -1736 * 256; game.facing = 64u;
    const unsigned sector = DoomLiteGame_PlayerSector(&game);
    assert(sector == 80u && !DoomLiteGame_IsSolid(&game, game.x_q8, game.y_q8));
    const GameWorldSide *side = game_world_side(game.map, 1388u, 1u);
    assert(side->upper != side->lower && side->upper && side->lower);
    game_cast_spans(&game, 64u, 256u, 0, (uint16_t)sector, 121);
    /* At 16 units from the wall the entire view is below world height 160.
     * Closed sector 85 has computer material above and brown stone below. */
    assert(game_span_count[64] == 1u);
    const GameWallSpan *span = &game_spans[game_span_first[64] + (0)];
    assert(span->depth == 16u && span->top == 0u && span->bottom == DOOM_GAME_VIEW_H);
    assert(span->texture == side->lower);
    check_v_projection(span, 160 + side->y_offset, 121);
    puts("world closed tiers: legal E1M2 camera sees lower BROWNGRN passed");
}

static void check_moving_door(void) {
    DoomLiteGame game;
    assert(DoomLiteGame_InitMap(&game, 0u));
    game.x_q8 = 832 * 256; game.y_q8 = 384 * 256; game.facing = 64u;
    const uint16_t sector = DoomLiteGame_PlayerSector(&game);
    const int eye = DoomLiteGame_FloorHeight(&game, sector) + 41;
    const unsigned door_texture = game_world_side(game.map, 577u, 1u)->upper;
    unsigned checked = 0u, first_phase = 0u;
    for (unsigned lift = 0u; lift <= 64u; lift += 32u) {
        game.sector_state[10].ceiling = (int16_t)(game.map->sectors[10].floor + (int)lift);
        game_cast_spans(&game, 64u, 256u, 0, sector, eye);
        for (unsigned i = 0u; i < game_span_count[64]; ++i) {
            const GameWallSpan *span = &game_spans[game_span_first[64] + (i)];
            if (span->color != 255u || span->texture != door_texture) continue;
            /* Independently locate the door boundary using all map records;
             * keep the WAD flag/offset, not an assumed normalized panel. */
            unsigned line_index = DOOM_MAP_NO_LINE;
            for (unsigned l = 0; l < game.map->line_count; ++l) {
                const DoomMapLine *line = &game.map->lines[l];
                if (line->front_sector != 10u && line->back_sector != 10u) continue;
                uint8_t unused;
                const uint32_t distance = game_line_distance(line, game.x_q8, game.y_q8,
                    0, 16384, 0u, LITE_MAX_DISTANCE_Q8, 0, &unused);
                if (distance != UINT32_MAX && distance / 256u == span->depth) line_index = l;
            }
            assert(line_index != DOOM_MAP_NO_LINE);
            const DoomMapLine *line = &game.map->lines[line_index];
            const uint16_t door_front = line->front_sector == 10u
                ? line->back_sector : line->front_sector;
            const GameWorldSide *side = game_world_side(game.map, line_index, line->front_sector == door_front);
            assert(side && span->texture == side->upper);
            const int anchor = (line->flags & 8u ? DoomLiteGame_CeilingHeight(&game, door_front)
                : game.sector_state[10].ceiling + (int)game_world_textures[span->texture].world_height)
                + side->y_offset;
            check_v_projection(span, anchor, eye);
            if (!checked) first_phase = span->texture_v;
            else assert(first_phase != span->texture_v);
            ++checked;
        }
    }
    assert(checked == 3u);
    puts("world moving door: three roof heights retain WAD texture anchor passed");
}

static void check_real_rail_and_actor(void) {
    DoomLiteGame game;
    assert(DoomLiteGame_InitMap(&game, 1u));
    game.x_q8 = 480 * 256; game.y_q8 = -1440 * 256; game.facing = 128u;
    assert(!DoomLiteGame_IsSolid(&game, game.x_q8, game.y_q8));
    DoomLite_RenderGameScene(scene, &game);
    unsigned holes_over_wall = 0u, solid_over_wall = 0u;
    const unsigned material = game_world_side(game.map, 307u, 1u)->middle;
    assert(game_span_count[64] > 0u);
    const GameWallSpan *central = &game_spans[game_span_first[64] + (0)];
    assert(central->color == GAME_WALL_MASKED && central->texture == material);
    assert(central->depth == 40u && central->texture_u == 10u);
    check_v_projection(central, 234, 153); /* Ceiling 240 plus sidedef Y=-6. */
    for (unsigned ray = 0; ray < LITE_RAYS; ++ray) {
        for (unsigned i = 0; i < game_span_count[ray]; ++i) {
            const GameWallSpan *mask = &game_spans[game_span_first[ray] + (i)];
            if (mask->color != GAME_WALL_MASKED || mask->texture != material) continue;
            for (unsigned y = mask->top; y < mask->bottom; ++y) {
                if (y == DOOM_GAME_VIEW_H / 2u && (ray == 63u || ray == 64u)) continue;
                for (unsigned j = i + 1u; j < game_span_count[ray]; ++j) {
                    const GameWallSpan *far = &game_spans[game_span_first[ray] + (j)];
                    if (far->color == GAME_WALL_MASKED || far->color == GAME_WALL_PLANE || !far->texture ||
                        y < far->top || y >= far->bottom) continue;
                    const unsigned mask_code = span_asset_code(mask, y);
                    const unsigned code = mask_code ? mask_code : span_asset_code(far, y);
                    const uint8_t expected = code ? game_world_gray[code] : 237u;
                    assert(scene[y * LITE_W + ray * 2u] == expected);
                    if (mask_code) ++solid_over_wall; else ++holes_over_wall;
                    break;
                }
            }
        }
    }
    assert(holes_over_wall > 0u && solid_over_wall > 0u);

    /* Controlled single actor behind the real railing, with all pickups
     * collected. This exercises production scene+Things compositing; it does
     * not claim an AI walking route through this position was tested. */
    unsigned thing = 0;
    while (thing < game.map->thing_count && game.map->things[thing].type != 3004u) ++thing;
    assert(thing < game.map->thing_count);
    memset(game.collected, 255, sizeof(game.collected));
    memset(game.thing_actor, 255, sizeof(game.thing_actor));
    game.actor_count = 0u;
    assert(!DoomLiteGame_IsSolid(&game, 432 * 256, -1440 * 256));
    game.actor_count = 1u;
    game.actors[0] = (DoomLiteActor){.x_q8=432 * 256, .y_q8=-1440 * 256,
        .thing_index=(uint16_t)thing, .sector=DoomMap_SectorAt(game.map, 432, -1440),
        .health=20, .state=DL_ACTOR_WALK};
    memcpy(saved_spans, game_spans, sizeof(saved_spans));
    memcpy(saved_count, game_span_count, sizeof(saved_count));
    memcpy(saved_first, game_span_first, sizeof(saved_first));
    memcpy(actual, scene, sizeof(actual));
    DoomLite_RenderGameThings(actual, &game);
    memcpy(unoccluded, scene, sizeof(unoccluded));
    memset(game_span_count, 0, sizeof(game_span_count));
    DoomLite_RenderGameThings(unoccluded, &game);
    memcpy(game_spans, saved_spans, sizeof(saved_spans));
    memcpy(game_span_count, saved_count, sizeof(saved_count));
    memcpy(game_span_first, saved_first, sizeof(saved_first));
    unsigned actor_holes = 0u, actor_solid = 0u;
    for (unsigned y = 0; y < DOOM_GAME_VIEW_H; ++y) for (unsigned x = 0; x < LITE_W; ++x) {
        const unsigned pixel = y * LITE_W + x;
        if (unoccluded[pixel] == scene[pixel]) continue;
        const unsigned ray = x / 2u;
        int blocked = 0, masked = 0;
        for (unsigned i = 0; i < saved_count[ray]; ++i) {
            const GameWallSpan *wall = &saved_spans[saved_first[ray] + (i)];
            if (y < wall->top || y >= wall->bottom || wall->depth > 50u) continue;
            if (wall->color == GAME_WALL_MASKED && wall->texture == material) {
                masked = 1;
                if (span_asset_code(wall, y)) blocked = 1;
            } else if (wall->color != GAME_WALL_MASKED) blocked = 1;
            else if (span_asset_code(wall, y)) blocked = 1;
        }
        assert(actual[pixel] == (blocked ? scene[pixel] : unoccluded[pixel]));
        if (masked) {
            if (blocked) ++actor_solid; else ++actor_holes;
        }
    }
    assert(actor_holes > 0u && actor_solid > 0u);
    printf("world masked railing: far wall holes=%u solid=%u; actor holes=%u blocked=%u passed\n",
           holes_over_wall, solid_over_wall, actor_holes, actor_solid);
}

static void check_full_cache_fallback(void) {
    DoomLiteGame game;
    assert(DoomLiteGame_InitMap(&game, 0u));
    game.facing = 0u;
    unsigned thing = 0u;
    while (thing < game.map->thing_count && game.map->things[thing].type != 3004u) ++thing;
    assert(thing < game.map->thing_count);
    memset(game.collected, 255, sizeof(game.collected));
    memset(game.thing_actor, 255, sizeof(game.thing_actor));
    game.actor_count = 1u;
    game.actors[0] = (DoomLiteActor){.x_q8=game.x_q8 + 128 * 256, .y_q8=game.y_q8,
        .thing_index=(uint16_t)thing, .sector=DoomLiteGame_PlayerSector(&game),
        .health=20, .state=DL_ACTOR_WALK};

    for (unsigned last_masked = 0u; last_masked < 2u; ++last_masked) {
        memset(&game_render_stats, 0, sizeof(game_render_stats));
        memset(game_span_count, 0, sizeof(game_span_count));
        for (unsigned ray = 0; ray < LITE_RAYS; ++ray) {
            game_span_first[ray] = (uint16_t)(ray * GAME_RAY_SPANS);
            /* Earlier near surfaces lie outside this boundary's incoming
             * window. The last slot is a newly appended plane or railing
             * strip inside it; a rejected later wall must not leave a hole. */
            game_add_span(ray, 0, 8, 16u, 180u, 63u, 0, DOOM_GAME_VIEW_H);
            for (unsigned i = 1u; i < GAME_RAY_SPANS - 1u; ++i)
                game_add_span(ray, 0, 8, 16u, GAME_WALL_PLANE, 0u, 0, DOOM_GAME_VIEW_H);
            game_add_span(ray, 68, 101, 64u,
                          last_masked ? GAME_WALL_MASKED : GAME_WALL_PLANE,
                          0u, 8, DOOM_GAME_VIEW_H);
            if (last_masked)
                game_texture_span(ray, GAME_RAY_SPANS - 1u, 37u, 84, 234, 153);
            assert(game_span_count[ray] == GAME_RAY_SPANS);
        }
        memcpy(saved_spans, game_spans, sizeof(saved_spans));
        memcpy(saved_first, game_span_first, sizeof(saved_first));
        for (unsigned ray = 0; ray < LITE_RAYS; ++ray) {
            game_add_fallback(ray, 8, DOOM_GAME_VIEW_H, 64u, 200u, 63u);
            assert(game_span_count[ray] == GAME_RAY_SPANS);
            assert(memcmp(&game_spans[game_span_first[ray]], &saved_spans[saved_first[ray]],
                          (GAME_RAY_SPANS - 1u) * sizeof(GameWallSpan)) == 0);
            const GameWallSpan *fallback = &game_spans[game_span_first[ray] + (GAME_RAY_SPANS - 1u)];
            assert(fallback->top == 8u && fallback->bottom == DOOM_GAME_VIEW_H);
            assert(fallback->depth == 64u && fallback->color == 200u);
            assert(!fallback->texture && !fallback->texture_u &&
                   !fallback->texture_v && !fallback->texture_step);
        }
        assert(game_render_stats.surfaces == LITE_RAYS * GAME_RAY_SPANS);
        assert(game_render_stats.surface_limit_hits == LITE_RAYS);
        assert(game_render_stats.surface_limit_pixels == LITE_RAYS * (DOOM_GAME_VIEW_H - 8u));

        /* Exercise the production tile painter, including its far-to-near
         * order and plane skip, rather than duplicating its drawing loop. */
        memset(scene, 129, sizeof(scene));
        for (unsigned first = 0u; first < DOOM_GAME_VIEW_H; first += 8u) {
            const unsigned end = first + 8u < DOOM_GAME_VIEW_H ? first + 8u : DOOM_GAME_VIEW_H;
            for (unsigned ray = 0u; ray < LITE_RAYS; ++ray)
                game_paint_wall_tile(scene, ray, first, end, game.contrast);
        }
        for (unsigned y = 0u; y < DOOM_GAME_VIEW_H; ++y) for (unsigned ray = 0u; ray < LITE_RAYS; ++ray) {
            const GameWallSpan *expected = &game_spans[game_span_first[ray] + (y < 8u ? 0u : GAME_RAY_SPANS - 1u)];
            assert(scene[y * LITE_W + ray * 2u] == game_span_gray(expected, y));
            assert(scene[y * LITE_W + ray * 2u + 1u] == game_span_gray(expected, y));
        }
        for (unsigned pixel = LITE_W * DOOM_GAME_VIEW_H; pixel < sizeof(scene); ++pixel)
            assert(scene[pixel] == 129u);

        /* A real trooper sprite at depth 128 must be hidden by the fallback,
         * while the same cache must preserve one in front at depth 56. */
        memcpy(saved_count, game_span_count, sizeof(saved_count));
        memcpy(saved_first, game_span_first, sizeof(saved_first));
        game_wall_ready = 1u;
        memcpy(actual, scene, sizeof(actual));
        DoomLite_RenderGameThings(actual, &game);
        assert(memcmp(actual, scene, sizeof(scene)) == 0);
        memset(game_span_count, 0, sizeof(game_span_count));
        memcpy(unoccluded, scene, sizeof(unoccluded));
        DoomLite_RenderGameThings(unoccluded, &game);
        assert(memcmp(unoccluded, scene, sizeof(scene)) != 0);
        memcpy(game_span_count, saved_count, sizeof(saved_count));
        memcpy(game_span_first, saved_first, sizeof(saved_first));
        game.actors[0].x_q8 = game.x_q8 + 56 * 256;
        memcpy(actual, scene, sizeof(actual));
        DoomLite_RenderGameThings(actual, &game);
        memset(game_span_count, 0, sizeof(game_span_count));
        memcpy(unoccluded, scene, sizeof(unoccluded));
        DoomLite_RenderGameThings(unoccluded, &game);
        assert(memcmp(actual, unoccluded, sizeof(actual)) == 0);
        assert(memcmp(actual, scene, sizeof(scene)) != 0);
        game.actors[0].x_q8 = game.x_q8 + 128 * 256;
    }

    /* The one remaining free slot also works, and accounting clips at the
     * view bounds. An empty request must not report imaginary filled area. */
    memset(&game_render_stats, 0, sizeof(game_render_stats));
    game_span_count[0] = 0u;
    for (unsigned i = 0u; i < GAME_RAY_SPANS - 1u; ++i)
        game_add_span(0u, 0, 8, 16u, GAME_WALL_PLANE, 0u, 0, DOOM_GAME_VIEW_H);
    game_add_fallback(0u, -9, LCD_PIX_H, 64u, 200u, 63u);
    assert(game_span_count[0] == GAME_RAY_SPANS);
    assert(game_render_stats.surfaces == GAME_RAY_SPANS);
    assert(game_render_stats.surface_limit_hits == 1u);
    assert(game_render_stats.surface_limit_pixels == DOOM_GAME_VIEW_H);
    game_add_fallback(0u, 101, 80, 64u, 200u, 63u);
    assert(game_render_stats.surface_limit_hits == 1u);
    assert(game_render_stats.surface_limit_pixels == DOOM_GAME_VIEW_H);
    puts("world full cache: last plane/railing replaced, incoming window painted, near surfaces and sprite depth retained passed");
}

int main(void) {
    check_u_and_clipped_v();
    check_closed_tiers();
    check_moving_door();
    check_real_rail_and_actor();
    check_full_cache_fallback();
    puts("WORLD_RENDER_OK real-map materials, cache projection and alpha occlusion passed");
    return 0;
}
