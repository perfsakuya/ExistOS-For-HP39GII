/* Build with a host C compiler: gcc -std=c11 -O2 -Wall -Wextra -Werror
 * tools/doom/check_lite_rays.c
 * System/applications/user/doom_lite/DoomLiteGame.c -lm -o check_lite_rays.exe
 * This checks the production trigonometry against libm, compares rays with a
 * one-unit reference march, and checks the exact renderer's frame bounds.
 * Optional arguments: preview.pgm [facing] [print-ray-table], or
 * --game-preview preview.pgm [start|key|enemy|door|door-open], or
 * --game-map-preview preview.pgm [open|key-collected|zoom]. */
#define DOOM_LITE_RAY_TEST
#define LCD_PIX_W 256
#define LCD_PIX_H 127
#include <math.h>
#include <stdlib.h>
#include "../../System/applications/user/doom_lite/DoomLite.c"
#include "../../System/applications/user/doom_lite/DoomLiteHud.c"

static uint32_t reference_distance(int32_t x, int32_t y, uint16_t angle) {
    const int32_t dx = sine_q14_fine((uint16_t)(angle + 256u));
    const int32_t dy = sine_q14_fine(angle);
    for (uint32_t distance = 256u; distance <= LITE_MAX_DISTANCE_Q8;
         distance += 256u) {
        const int32_t rx = x + (int32_t)(((int64_t)dx * distance) >> 14);
        const int32_t ry = y + (int32_t)(((int64_t)dy * distance) >> 14);
        if (wall_at_q8(rx, ry)) return distance;
    }
    return LITE_MAX_DISTANCE_Q8;
}

static int check_game_portals(void) {
    uint8_t closed[E1M1_DOOR_COUNT] = {0};
    uint8_t open[E1M1_DOOR_COUNT];
    uint8_t frame[LITE_W * LITE_H + 16u];
    memset(open, 1, sizeof(open));

    /* E1M1 start must be in navigable space for every initial view. */
    for (unsigned angle = 0; angle < 1024u; angle += 128u) {
        const RayHit hit = cast_game_ray(E1M1_START_X * 256,
                                         E1M1_START_Y * 256,
                                         (uint16_t)angle, closed);
        if (!hit.distance_q8 || hit.distance_q8 > LITE_MAX_DISTANCE_Q8) {
            fprintf(stderr, "game start ray invalid at angle=%u: %u Q8\n",
                    angle, hit.distance_q8);
            return 11;
        }
    }

    /* Door sector 10 spans y=512..544. Approaching its boundary must see
     * a close wall when closed and the room behind it when open. */
    const int32_t x = 832 * 256, y = 488 * 256;
    const RayHit shut = cast_game_ray(x, y, 256u, closed);
    const RayHit pass = cast_game_ray(x, y, 256u, open);
    if (shut.distance_q8 != 24u * 256u ||
        pass.distance_q8 <= shut.distance_q8 + 32u * 256u ||
        shut.door_id != 0u || pass.door_id != E1M1_PORTAL_NO_DOOR ||
        shut.door_u < 118u || shut.door_u > 137u) {
        fprintf(stderr,
                "door ray invalid: closed=%u door=%u u=%u, open=%u door=%u Q8\n",
                shut.distance_q8, shut.door_id, shut.door_u,
                pass.distance_q8, pass.door_id);
        return 12;
    }

    /* Rendered closed door needs a distinct dark centre seam. Its opened
     * state must reveal the wall beyond, with no stale door overlay. */
    uint8_t closed_frame[LITE_W * LITE_H];
    uint8_t open_frame[LITE_W * LITE_H];
    DoomLite_RenderGameFrame(closed_frame, x, y, 64u, 0, closed);
    DoomLite_RenderGameFrame(open_frame, x, y, 64u, 0, open);
    const unsigned sample = 48u * LITE_W + LITE_W / 2u;
    if (closed_frame[sample] > 50u ||
        open_frame[sample] == closed_frame[sample]) {
        fprintf(stderr, "door panel missing: closed=%u open=%u\n",
                closed_frame[sample], open_frame[sample]);
        return 27;
    }

    /* A ray beginning precisely on the WAD door line sees the closed door
     * at distance zero, while the opened door cannot self-occlude. */
    const RayHit boundary_shut = cast_game_ray(832 * 256, 512 * 256,
                                               256u, closed);
    const RayHit boundary_open = cast_game_ray(832 * 256, 512 * 256,
                                               256u, open);
    if (boundary_shut.distance_q8 != 0u ||
        boundary_open.distance_q8 < 16u * 256u ||
        boundary_shut.door_id != 0u ||
        boundary_open.door_id != E1M1_PORTAL_NO_DOOR) {
        fprintf(stderr, "door boundary invalid: closed=%u/%u open=%u/%u Q8\n",
                boundary_shut.distance_q8, boundary_shut.door_id,
                boundary_open.distance_q8, boundary_open.door_id);
        return 13;
    }

    /* A permanent wall adjacent to that door stays solid in both states. */
    const RayHit side_shut = cast_game_ray(880 * 256, 528 * 256,
                                           0u, closed);
    const RayHit side_open = cast_game_ray(880 * 256, 528 * 256,
                                           0u, open);
    if (side_shut.distance_q8 != 16u * 256u ||
        side_open.distance_q8 != side_shut.distance_q8 ||
        side_shut.door_id != E1M1_PORTAL_NO_DOOR ||
        side_open.door_id != E1M1_PORTAL_NO_DOOR) {
        fprintf(stderr, "adjacent wall invalid: closed=%u open=%u Q8\n",
                side_shut.distance_q8, side_open.distance_q8);
        return 14;
    }

    /* The new entry point must preserve the same frame bounds as Lite. */
    for (unsigned state = 0; state < 2u; ++state) {
        memset(frame, 0xa5, sizeof(frame));
        DoomLite_RenderGameFrame(frame + 8u, x, y, 64u, (int)state,
                                 state ? open : closed);
        for (unsigned i = 0; i < 8u; ++i) {
            if (frame[i] != 0xa5 || frame[sizeof(frame) - 1u - i] != 0xa5) {
                fprintf(stderr, "game frame guard changed at state=%u\n",
                        state);
                return 15;
            }
        }
    }
    printf("game portal rays: start, door, boundary, static wall, frame bounds passed\n");
    return 0;
}

static int check_exit_alcove(void) {
    const uint8_t doors[E1M1_DOOR_COUNT] = {0};
    /* E1M1's exit switch is the west wall of a 16-by-32-unit recess. The
     * original Lite grid stops at x=-384; precise gameplay geometry must
     * reach the WAD line at x=-400 without changing Lite's rendering. */
    const RayHit game_approach = cast_game_ray(-344 * 256, 1296 * 256,
                                               512u, doors);
    const RayHit lite_approach = cast_ray(-344 * 256, 1296 * 256, 512u);
    const RayHit game_near = cast_game_ray(-360 * 256, 1296 * 256,
                                           512u, doors);
    const RayHit lite_near = cast_ray(-360 * 256, 1296 * 256, 512u);
    if (game_approach.distance_q8 != 56u * 256u ||
        lite_approach.distance_q8 != 8u * 256u ||
        game_near.distance_q8 != 40u * 256u ||
        lite_near.distance_q8 != 24u * 256u) {
        fprintf(stderr, "exit recess distance changed: game=%u,%u lite=%u,%u Q8\n",
                game_approach.distance_q8, game_near.distance_q8,
                lite_approach.distance_q8, lite_near.distance_q8);
        return 25;
    }
    const int32_t inside_x = -392 * 256, inside_y = 1296 * 256;
    const RayHit west = cast_game_ray(inside_x, inside_y, 512u, doors);
    const RayHit north = cast_game_ray(inside_x, inside_y, 768u, doors);
    const RayHit south = cast_game_ray(inside_x, inside_y, 256u, doors);
    const RayHit east = cast_game_ray(inside_x, inside_y, 0u, doors);
    if (west.distance_q8 != 8u * 256u ||
        north.distance_q8 != 16u * 256u ||
        south.distance_q8 != 16u * 256u ||
        east.distance_q8 <= 32u * 256u) {
        fprintf(stderr, "exit recess walls/opening changed: W=%u N=%u S=%u E=%u Q8\n",
                west.distance_q8, north.distance_q8,
                south.distance_q8, east.distance_q8);
        return 26;
    }
    printf("exit recess rays: exact switch wall, side walls, east opening, Lite unchanged passed\n");
    return 0;
}

static int check_game_sprites(void) {
    DoomLiteGame game;
    uint8_t frame[LITE_W * LITE_H];
    unsigned key_index = E1M1_THING_COUNT;
    unsigned enemy_index = E1M1_THING_COUNT;
    DoomLiteGame_Init(&game);
    for (unsigned i = 0; i < E1M1_THING_COUNT; ++i) {
        if (e1m1_things[i].type == 5u &&
            DoomLiteGame_ThingActive(&game, i)) key_index = i;
        if (enemy_index == E1M1_THING_COUNT && game.enemy_hp[i])
            enemy_index = i;
    }
    if (key_index == E1M1_THING_COUNT ||
        enemy_index == E1M1_THING_COUNT) return 16;
    memset(game.enemy_hp, 0, sizeof(game.enemy_hp));
    game.x_q8 = ((int32_t)e1m1_things[key_index].x - 64) * 256;
    game.y_q8 = (int32_t)e1m1_things[key_index].y * 256;
    game.facing = 0;
    for (unsigned ray = 0; ray < LITE_RAYS; ++ray)
        game_wall_depth[ray] = 1536u;
    game_wall_ready = 1;
    memset(frame, 245, sizeof(frame));
    DoomLite_RenderGameThings(frame, &game);
    unsigned pixels = 0;
    for (unsigned i = 0; i < sizeof(frame); ++i)
        pixels += frame[i] != 245u;
    if (!pixels) {
        fprintf(stderr, "active blue key was not rendered\n");
        return 17;
    }
    game.collected[key_index >> 3] |= (uint8_t)(1u << (key_index & 7u));
    memset(frame, 245, sizeof(frame));
    DoomLite_RenderGameThings(frame, &game);
    for (unsigned i = 0; i < sizeof(frame); ++i) {
        if (frame[i] != 245u) {
            fprintf(stderr, "collected blue key remained visible\n");
            return 18;
        }
    }
    game.collected[key_index >> 3] &=
        (uint8_t)~(1u << (key_index & 7u));
    for (unsigned ray = 0; ray < LITE_RAYS; ++ray)
        game_wall_depth[ray] = 8u;
    memset(frame, 245, sizeof(frame));
    DoomLite_RenderGameThings(frame, &game);
    for (unsigned i = 0; i < sizeof(frame); ++i) {
        if (frame[i] != 245u) {
            fprintf(stderr, "blue key rendered through near wall\n");
            return 19;
        }
    }
    for (unsigned ray = 0; ray < LITE_RAYS; ++ray)
        game_wall_depth[ray] = 1536u;
    game.collected[key_index >> 3] |= (uint8_t)(1u << (key_index & 7u));
    game.enemy_hp[enemy_index] = 20u;
    game.x_q8 = ((int32_t)e1m1_things[enemy_index].x - 64) * 256;
    game.y_q8 = (int32_t)e1m1_things[enemy_index].y * 256;
    memset(frame, 245, sizeof(frame));
    DoomLite_RenderGameThings(frame, &game);
    pixels = 0;
    for (unsigned i = 0; i < sizeof(frame); ++i)
        pixels += frame[i] != 245u;
    if (!pixels) {
        fprintf(stderr, "live enemy was not rendered\n");
        return 20;
    }
    unsigned shades = 0u;
    for (unsigned color = 0; color < 256u; ++color) {
        if (color == 245u) continue;
        for (unsigned i = 0; i < sizeof(frame); ++i)
            if (frame[i] == color) { ++shades; break; }
    }
    if (shades < 4u) {
        fprintf(stderr, "enemy sprite lost grayscale detail: %u shades\n", shades);
        return 35;
    }
    game.enemy_hp[enemy_index] = 0u;
    memset(frame, 245, sizeof(frame));
    DoomLite_RenderGameThings(frame, &game);
    for (unsigned i = 0; i < sizeof(frame); ++i) {
        if (frame[i] != 245u) {
            fprintf(stderr, "dead enemy remained visible\n");
            return 21;
        }
    }
    printf("game sprites: blue key and enemy visibility, pickup, death, wall occlusion passed\n");
    return 0;
}

static int check_game_map_markers(void) {
    DoomLiteGame game;
    uint8_t frame[LITE_W * LITE_H + 16u];
    uint8_t plain[LITE_W * LITE_H];
    DoomLiteGame_Init(&game);
    memset(frame, 0xa5, sizeof(frame));
    DoomLite_RenderFrame(frame + 8u, game.x_q8, game.y_q8, 0u, 1);
    memcpy(plain, frame + 8u, sizeof(plain));
    DoomLite_RenderGameMap(frame + 8u, &game, 1u);
    for (unsigned i = 0; i < 8u; ++i) {
        if (frame[i] != 0xa5 || frame[sizeof(frame) - 1u - i] != 0xa5) {
            fprintf(stderr, "game map overlay crossed framebuffer bounds\n");
            return 28;
        }
    }
    /* The overview reserves 16 rows for the large HUD. Known E1M1 cells:
     * door (114,67), key (156,69), exit (73,89), player (75,59). */
    if (frame[67u * LITE_W + 114u + 8u] != 0u ||
        frame[63u * LITE_W + 156u + 8u] != 0u ||
        frame[69u * LITE_W + 156u + 8u] != 250u ||
        frame[89u * LITE_W + 73u + 8u] != 0u ||
        frame[59u * LITE_W + 82u + 8u] != 0u) {
        fprintf(stderr, "game map door/key/exit/heading marks absent\n");
        return 29;
    }
    game.door_open[0] = 1u;
    game.collected[87u >> 3] |= (uint8_t)(1u << (87u & 7u));
    game.facing = 64u;
    DoomLite_RenderGameMap(frame + 8u, &game, 1u);
    if (frame[67u * LITE_W + 114u + 8u] != 250u ||
        frame[63u * LITE_W + 156u + 8u] == 0u ||
        frame[59u * LITE_W + 82u + 8u] == 0u ||
        frame[66u * LITE_W + 75u + 8u] != 0u) {
        fprintf(stderr, "game map state/heading did not update\n");
        return 30;
    }
    DoomLite_RenderGameMap(frame + 8u, &game, 2u);
    DoomLite_DrawGameHud(frame + 8u, &game, "NEED BLUE", 2u);
    if (frame[71u * LITE_W + 22u + 8u] != 0u ||
        frame[78u * LITE_W + 22u + 8u] != 0u) {
        fprintf(stderr, "zoom map does not follow player/heading\n");
        return 36;
    }
    for (unsigned i = 0; i < 8u; ++i)
        if (frame[i] != 0xa5 || frame[sizeof(frame) - 1u - i] != 0xa5) {
            fprintf(stderr, "zoom map/HUD crossed framebuffer bounds\n");
            return 37;
        }
    DoomLite_RenderFrame(frame + 8u, game.x_q8, game.y_q8, 0u, 1);
    if (memcmp(frame + 8u, plain, sizeof(plain)) != 0) {
        fprintf(stderr, "Lite map changed after Game overlay\n");
        return 31;
    }
    printf("game map: large marks, 2x follow, HUD bounds, Lite isolation passed\n");
    return 0;
}

static int check_sprite_clipping(void) {
    uint8_t frame[LITE_W * LITE_H + 16u];
    for (unsigned texture = 0; texture < E1M1_SPRITE_COUNT; ++texture) {
        for (unsigned side = 0; side < 2u; ++side) {
            DoomLiteGame game;
            DoomLiteGame_Init(&game);
            memset(game.enemy_hp, 0, sizeof(game.enemy_hp));
            memset(game.collected, 255, sizeof(game.collected));
            unsigned target = 0u;
            while (target < E1M1_THING_COUNT &&
                   (e1m1_things[target].type != e1m1_sprites[texture].thing_type ||
                    !(e1m1_things[target].options & 2u) ||
                    (e1m1_things[target].options & 16u)))
                ++target;
            if (target == E1M1_THING_COUNT) return 38;
            game.enemy_hp[target] = 100u;
            game.collected[target >> 3] &= (uint8_t)~(1u << (target & 7u));
            game.x_q8 = ((int32_t)e1m1_things[target].x - 12) * 256;
            game.y_q8 = ((int32_t)e1m1_things[target].y + (side ? 6 : -6)) * 256;
            game.facing = 0u;
            for (unsigned ray = 0; ray < LITE_RAYS; ++ray)
                game_wall_depth[ray] = 1536u;
            game_wall_ready = 1u;
            memset(frame, 0xa5, sizeof(frame));
            DoomLite_RenderGameThings(frame + 8u, &game);
            unsigned drawn = 0u;
            for (unsigned i = 8u; i < sizeof(frame) - 8u; ++i)
                drawn += frame[i] != 0xa5;
            if (!drawn) return 40;
            for (unsigned i = 0; i < 8u; ++i)
                if (frame[i] != 0xa5 || frame[sizeof(frame) - 1u - i] != 0xa5)
                    return 39;
        }
    }
    printf("packed sprites: left/right clipping and maximum scale bounds passed\n");
    return 0;
}

int main(int argc, char **argv) {
    const int game_check = check_game_portals();
    if (game_check) return game_check;
    const int exit_check = check_exit_alcove();
    if (exit_check) return exit_check;
    const int sprite_check = check_game_sprites();
    if (sprite_check) return sprite_check;
    const int map_check = check_game_map_markers();
    if (map_check) return map_check;
    const int clip_check = check_sprite_clipping();
    if (clip_check) return clip_check;
    for (unsigned angle = 0; angle < 1024u; ++angle) {
        const int actual = sine_q14_fine((uint16_t)angle);
        const double expected = sin((double)angle * (6.283185307179586 / 1024.0)) * 16384.0;
        if (actual < -16384 || actual > 16384 ||
            fabs((double)actual - expected) > 4.0) {
            fprintf(stderr, "sine error at angle=%u: actual=%d expected=%.2f\n",
                    angle, actual, expected);
            return 9;
        }
    }
    const int start_gx = (E1M1_START_X / E1M1_CELL) - E1M1_GX0;
    const int start_gy = (E1M1_START_Y / E1M1_CELL) - E1M1_GY0;
    unsigned points = 0, rays = 0;
    uint32_t worst_late_q8 = 0;
    uint8_t frame[LITE_W * LITE_H + 16u];
    for (int oy = -6; oy <= 6 && points < 12u; ++oy) {
        for (int ox = -6; ox <= 6 && points < 12u; ++ox) {
            const int ix = start_gx + ox, iy = start_gy + oy;
            if ((unsigned)ix >= E1M1_WIDTH || (unsigned)iy >= E1M1_HEIGHT ||
                e1m1_grid[iy * E1M1_WIDTH + ix]) continue;
            const int gx = ix + E1M1_GX0, gy = iy + E1M1_GY0;
            const int32_t x = (gx * E1M1_CELL + E1M1_CELL / 2) * 256;
            const int32_t y = (gy * E1M1_CELL + E1M1_CELL / 2) * 256;
            for (unsigned angle = 0; angle < 1024u; ++angle) {
                const RayHit hit = cast_ray(x, y, (uint16_t)angle);
                const uint32_t reference = reference_distance(x, y,
                                                                (uint16_t)angle);
                if (hit.distance_q8 > LITE_MAX_DISTANCE_Q8) return 1;
                if (hit.distance_q8 < LITE_MAX_DISTANCE_Q8) {
                    const int hx = hit.gx - E1M1_GX0;
                    const int hy = hit.gy - E1M1_GY0;
                    if ((unsigned)hx < E1M1_WIDTH &&
                        (unsigned)hy < E1M1_HEIGHT &&
                        !e1m1_grid[hy * E1M1_WIDTH + hx]) return 2;
                }
                if (hit.distance_q8 > reference) {
                    const uint32_t late = hit.distance_q8 - reference;
                    if (late > worst_late_q8) worst_late_q8 = late;
                    if (late > 512u) {
                        fprintf(stderr,
                                "ray behind reference at (%d,%d) angle=%u by %u Q8\n",
                                gx, gy, angle, late);
                        return 3;
                    }
                }
                ++rays;
            }
            memset(frame, 0xa5, sizeof(frame));
            for (unsigned facing = 0; facing < 256u; facing += 32u) {
                DoomLite_RenderFrame(frame + 8u, x, y, (uint8_t)facing, 0);
                for (unsigned i = 0; i < 8u; ++i) {
                    if (frame[i] != 0xa5 ||
                        frame[sizeof(frame) - 1u - i] != 0xa5) {
                        fprintf(stderr, "frame guard changed at (%d,%d) facing=%u\n",
                                gx, gy, facing);
                        return 5;
                    }
                }
            }
            DoomLite_RenderFrame(frame + 8u, x, y, 0, 1);
            for (unsigned i = 0; i < 8u; ++i) {
                if (frame[i] != 0xa5 ||
                    frame[sizeof(frame) - 1u - i] != 0xa5) {
                    fprintf(stderr, "map frame guard changed at (%d,%d)\n",
                            gx, gy);
                    return 10;
                }
            }
            ++points;
        }
    }
    if (points != 12u) return 4;
    printf("checked %u fine-angle rays and %u rendered views from %u open cells; worst late=%u Q8\n",
           rays, points * 8u, points, worst_late_q8);
    if (argc > 2 && strcmp(argv[1], "--game-map-preview") == 0) {
        DoomLiteGame game;
        DoomLiteGame_Init(&game);
        if (argc > 3 && strcmp(argv[3], "open") == 0)
            game.door_open[0] = 1u;
        if (argc > 3 && strcmp(argv[3], "key-collected") == 0)
            game.collected[87u >> 3] |= (uint8_t)(1u << (87u & 7u));
        const unsigned zoom = argc > 3 && strcmp(argv[3], "zoom") == 0 ? 2u : 1u;
        DoomLite_RenderGameMap(frame + 8u, &game, zoom);
        DoomLite_DrawGameHud(frame + 8u, &game, NULL, zoom);
        FILE *preview = fopen(argv[2], "wb");
        if (!preview) return 32;
        fprintf(preview, "P5\n%d %d\n255\n", LITE_W, LITE_H);
        if (fwrite(frame + 8u, 1u, LITE_W * LITE_H, preview) !=
            LITE_W * LITE_H) {
            fclose(preview);
            return 33;
        }
        if (fclose(preview)) return 34;
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "--game-preview") == 0) {
        DoomLiteGame game;
        DoomLiteGame_Init(&game);
        if (argc > 3 && strcmp(argv[3], "key") == 0) {
            game.x_q8 = 2192 * 256;
            game.y_q8 = 512 * 256;
            game.facing = 64;
        } else if (argc > 3 && strcmp(argv[3], "enemy") == 0) {
            game.x_q8 = (816 - 80) * 256;
            game.y_q8 = 448 * 256;
            game.facing = 0;
        } else if (argc > 3 && strcmp(argv[3], "exit") == 0) {
            game.x_q8 = -288 * 256;
            game.y_q8 = 1296 * 256;
            game.facing = 128;
        } else if (argc > 3 &&
                   (strcmp(argv[3], "door") == 0 ||
                    strcmp(argv[3], "door-open") == 0)) {
            game.x_q8 = 832 * 256;
            game.y_q8 = 384 * 256;
            game.facing = 64;
            if (strcmp(argv[3], "door-open") == 0)
                game.door_open[0] = 1u;
        }
        DoomLite_RenderGameFrame(frame + 8u, game.x_q8, game.y_q8,
                                  game.facing, 0, game.door_open);
        /* Keep door previews unobstructed by the nearby starting enemy. */
        if (argc <= 3 ||
            (strcmp(argv[3], "door") != 0 &&
             strcmp(argv[3], "door-open") != 0))
            DoomLite_RenderGameThings(frame + 8u, &game);
        DoomLite_DrawGameHud(frame + 8u, &game, NULL, 0u);
        FILE *preview = fopen(argv[2], "wb");
        if (!preview) return 22;
        fprintf(preview, "P5\n%d %d\n255\n", LITE_W, LITE_H);
        if (fwrite(frame + 8u, 1u, LITE_W * LITE_H, preview) !=
            LITE_W * LITE_H) {
            fclose(preview);
            return 23;
        }
        if (fclose(preview)) return 24;
        return 0;
    }
    if (argc > 1) {
        const unsigned facing = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 0u;
        DoomLite_RenderFrame(frame + 8u, E1M1_START_X * 256,
                             E1M1_START_Y * 256, (uint8_t)facing, 0);
        FILE *preview = fopen(argv[1], "wb");
        if (!preview) return 6;
        fprintf(preview, "P5\n%d %d\n255\n", LITE_W, LITE_H);
        if (fwrite(frame + 8u, 1u, LITE_W * LITE_H, preview) !=
            LITE_W * LITE_H) {
            fclose(preview);
            return 7;
        }
        if (fclose(preview)) return 8;
        if (argc > 3) {
            for (unsigned ray = 0; ray < LITE_RAYS; ++ray) {
                const int offset = ((int)ray * 168) / (int)(LITE_RAYS - 1u) - 84;
                const RayHit hit = cast_ray(E1M1_START_X * 256,
                                            E1M1_START_Y * 256,
                                            (uint16_t)((int)facing * 4 + offset));
                printf("ray=%u offset=%d dx=%d dy=%d distance=%u reference=%u cell=(%d,%d)\n",
                       ray, offset,
                       sine_q14_fine((uint16_t)((int)facing * 4 + offset + 256)),
                       sine_q14_fine((uint16_t)((int)facing * 4 + offset)),
                       hit.distance_q8 >> 8,
                       reference_distance(E1M1_START_X * 256,
                                          E1M1_START_Y * 256,
                                          (uint16_t)((int)facing * 4 + offset)) >> 8,
                       hit.gx, hit.gy);
            }
        }
    }
    return 0;
}
