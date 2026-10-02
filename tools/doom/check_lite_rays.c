/* Build with a host C compiler: gcc -std=c11 -O2 -Wall -Wextra -Werror
 * tools/doom/check_lite_rays.c -lm -o check_lite_rays.exe
 * This checks the production trigonometry against libm, compares rays with a
 * one-unit reference march, and checks the exact renderer's frame bounds.
 * Optional arguments: preview.pgm [facing] [print-ray-table]. */
#define DOOM_LITE_RAY_TEST
#define LCD_PIX_W 256
#define LCD_PIX_H 127
#include <math.h>
#include <stdlib.h>
#include "../../System/applications/user/doom_lite/DoomLite.c"

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

int main(int argc, char **argv) {
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
                draw_scene(frame + 8u, x, y, (uint8_t)facing);
                for (unsigned i = 0; i < 8u; ++i) {
                    if (frame[i] != 0xa5 ||
                        frame[sizeof(frame) - 1u - i] != 0xa5) {
                        fprintf(stderr, "frame guard changed at (%d,%d) facing=%u\n",
                                gx, gy, facing);
                        return 5;
                    }
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
        draw_scene(frame + 8u, E1M1_START_X * 256, E1M1_START_Y * 256,
                   (uint8_t)facing);
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
