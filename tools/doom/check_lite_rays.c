/* Build with a host C compiler: gcc -std=c11 -O2 -Wall -Wextra -Werror
 * tools/doom/check_lite_rays.c
 * System/applications/user/doom_lite/DoomLiteGame.c
 * System/applications/user/doom_lite/DoomMap.c -lm -o check_lite_rays.exe
 * This checks the production trigonometry against libm, compares rays with a
 * one-unit reference march, and checks the exact renderer's frame bounds.
 * Optional arguments: preview.pgm [facing] [print-ray-table], or
 * Game scene/actors are covered separately by check_game_renderer.c. */
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

int main(int argc, char **argv) {
    const int game_check = check_game_portals();
    if (game_check) return game_check;
    const int exit_check = check_exit_alcove();
    if (exit_check) return exit_check;
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
