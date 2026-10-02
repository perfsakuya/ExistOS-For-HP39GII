/* Fast, intentionally simplified E1M1 walk-through for the 39gII LCD.
 * Geometry comes from the freely licensed Freedoom E1M1 map. This renderer
 * does not emulate Doom sectors, sprites, monsters, doors, or game rules. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef DOOM_LITE_RAY_TEST
#include "FreeRTOS.h"
#include "task.h"
#include "SystemUI.h"
#include "keyboard_gii39.h"
#include "sys_llapi.h"
#endif
#include "E1M1Grid.h"

#define LITE_W LCD_PIX_W
#define LITE_H LCD_PIX_H
#define LITE_RAY_PIXELS 2u
#define LITE_RAYS (LITE_W / LITE_RAY_PIXELS)
#define LITE_CELL_Q8 (E1M1_CELL * 256u)
#define LITE_MAX_DISTANCE_Q8 (1536u * 256u)

/* Sine for the first quarter turn in Q14, including both endpoints. */
static const int16_t quarter_sine[65] = {
       0, 402, 804, 1205, 1606, 2006, 2404, 2801, 3196, 3590, 3981,
    4370, 4756, 5139, 5520, 5897, 6270, 6639, 7005, 7366, 7723, 8076,
    8423, 8765, 9102, 9434, 9760, 10080, 10394, 10702, 11003, 11297,
    11585, 11866, 12140, 12406, 12665, 12916, 13160, 13395, 13623,
    13842, 14053, 14256, 14449, 14635, 14811, 14978, 15137, 15286,
    15426, 15557, 15679, 15791, 15893, 15986, 16069, 16143, 16207,
    16261, 16305, 16340, 16364, 16379, 16384,
};

#ifndef DOOM_LITE_RAY_TEST
volatile int SkyOS_DoomLiteRunning;
static uint8_t barrier_pixel;
#endif

static int sine_q14(uint8_t angle) {
    const unsigned quadrant = angle >> 6;
    const unsigned index = angle & 63u;
    if (quadrant == 0u) return quarter_sine[index];
    if (quadrant == 1u) return quarter_sine[64u - index];
    if (quadrant == 2u) return -quarter_sine[index];
    return -quarter_sine[64u - index];
}

/* Four substeps per table entry make adjacent two-pixel columns distinct. */
static int sine_q14_fine(uint16_t angle) {
    const uint8_t base = (uint8_t)(angle >> 2);
    const int fraction = (int)(angle & 3u);
    const int low = sine_q14(base);
    const int high = sine_q14((uint8_t)(base + 1u));
    return low + ((high - low) * fraction) / 4;
}

static int wall_at_q8(int32_t x, int32_t y) {
    const int gx = (x >> 13) - E1M1_GX0;
    const int gy = (y >> 13) - E1M1_GY0;
    if ((unsigned)gx >= E1M1_WIDTH || (unsigned)gy >= E1M1_HEIGHT)
        return 1;
    return e1m1_grid[gy * E1M1_WIDTH + gx];
}

typedef struct {
    uint32_t distance_q8;
    int gx;
    int gy;
} RayHit;

/* Cross only grid boundaries. The old eight-unit march checked each wall
 * cell up to four times and could step past thin rasterized features. */
static RayHit cast_ray(int32_t x, int32_t y, uint16_t angle) {
    const int32_t dx = sine_q14_fine((uint16_t)(angle + 256u));
    const int32_t dy = sine_q14_fine(angle);
    int gx = x >> 13, gy = y >> 13;
    const int step_x = dx < 0 ? -1 : 1;
    const int step_y = dy < 0 ? -1 : 1;
    const uint32_t abs_x = dx < 0 ? (uint32_t)-dx : (uint32_t)dx;
    const uint32_t abs_y = dy < 0 ? (uint32_t)-dy : (uint32_t)dy;
    const uint32_t cell = LITE_CELL_Q8;
    const uint32_t delta_x = abs_x ? (cell << 14) / abs_x : UINT32_MAX;
    const uint32_t delta_y = abs_y ? (cell << 14) / abs_y : UINT32_MAX;
    const uint32_t offset_x = dx >= 0
        ? (uint32_t)((gx + 1) * (int32_t)cell - x)
        : (uint32_t)(x - gx * (int32_t)cell);
    const uint32_t offset_y = dy >= 0
        ? (uint32_t)((gy + 1) * (int32_t)cell - y)
        : (uint32_t)(y - gy * (int32_t)cell);
    uint32_t next_x = abs_x ? (offset_x << 14) / abs_x : UINT32_MAX;
    uint32_t next_y = abs_y ? (offset_y << 14) / abs_y : UINT32_MAX;
    RayHit hit = { LITE_MAX_DISTANCE_Q8, gx, gy };
    for (unsigned crossed = 0; crossed < E1M1_WIDTH + E1M1_HEIGHT; ++crossed) {
        if (next_x < next_y) {
            hit.distance_q8 = next_x;
            if (hit.distance_q8 > LITE_MAX_DISTANCE_Q8) break;
            gx += step_x;
            next_x += delta_x;
        } else {
            hit.distance_q8 = next_y;
            if (hit.distance_q8 > LITE_MAX_DISTANCE_Q8) break;
            gy += step_y;
            next_y += delta_y;
        }
        hit.gx = gx;
        hit.gy = gy;
        const int ix = gx - E1M1_GX0, iy = gy - E1M1_GY0;
        if ((unsigned)ix >= E1M1_WIDTH || (unsigned)iy >= E1M1_HEIGHT ||
            e1m1_grid[iy * E1M1_WIDTH + ix])
            return hit;
    }
    hit.distance_q8 = LITE_MAX_DISTANCE_Q8;
    return hit;
}

#ifndef DOOM_LITE_RAY_TEST
static void present(uint8_t *pixels) {
    ll_disp_put_area(pixels, 0, 0, LITE_W - 1u, LITE_H - 1u);
    /* The display queue borrows its source. Drain the full-screen draw
     * before the next frame modifies the shared UI framebuffer. */
    for (unsigned i = 0; i < 5u; ++i)
        ll_disp_put_area(&barrier_pixel, 0, 0, 0, 0);
}
#endif

static void draw_scene(uint8_t *pixels, int32_t x, int32_t y, uint8_t facing) {
    uint8_t wall_top[LITE_RAYS];
    uint8_t wall_bottom[LITE_RAYS];
    uint8_t wall_shade[LITE_RAYS];
    for (unsigned ray = 0; ray < LITE_RAYS; ++ray) {
        const int offset = ((int)ray * 168) / (int)(LITE_RAYS - 1u) - 84;
        const uint16_t angle = (uint16_t)((int)facing * 4 + offset);
        const RayHit hit = cast_ray(x, y, angle);
        int distance = ((int)(hit.distance_q8 >> 8) *
                        sine_q14_fine((uint16_t)(offset + 256))) >> 14;
        if (distance < 24) distance = 24;
        int height = 8192 / distance;
        if (height > LITE_H) height = LITE_H;
        const int top = (LITE_H - height) / 2;
        const int bottom = top + height;
        int shade = 86 + distance / 16;
        if (shade > 185) shade = 185;
        if ((hit.gx ^ hit.gy) & 1) shade += 12;
        if (shade > 200) shade = 200;
        wall_top[ray] = (uint8_t)top;
        wall_bottom[ray] = (uint8_t)bottom;
        wall_shade[ray] = (uint8_t)shade;
    }
    /* Write consecutive pixels across each LCD row. This avoids jumping
     * through the 32 KiB framebuffer once per column and screen row. */
    for (unsigned yrow = 0; yrow < LITE_H; ++yrow) {
        uint8_t *row = pixels + yrow * LITE_W;
        const uint8_t sky = (uint8_t)(226u - yrow / 5u);
        const uint8_t floor = (uint8_t)(173u + yrow / 4u);
        for (unsigned ray = 0; ray < LITE_RAYS; ++ray) {
            const uint8_t color = yrow < wall_top[ray] ? sky
                : yrow < wall_bottom[ray] ? wall_shade[ray] : floor;
            for (unsigned copy = 0; copy < LITE_RAY_PIXELS; ++copy)
                row[ray * LITE_RAY_PIXELS + copy] = color;
        }
    }
    /* Two-pixel crosshair. */
    pixels[(LITE_H / 2u) * LITE_W + LITE_W / 2u] = 24;
    pixels[(LITE_H / 2u) * LITE_W + LITE_W / 2u - 1u] = 24;
}

#ifndef DOOM_LITE_RAY_TEST
static void draw_map(uint8_t *pixels, int32_t x, int32_t y) {
    const int ox = (LITE_W - E1M1_WIDTH) / 2;
    const int oy = (LITE_H - E1M1_HEIGHT) / 2;
    memset(pixels, 245, LITE_W * LITE_H);
    for (int gy = 0; gy < E1M1_HEIGHT; ++gy) {
        uint8_t *row = pixels + (oy + gy) * LITE_W + ox;
        for (int gx = 0; gx < E1M1_WIDTH; ++gx)
            row[gx] = e1m1_grid[gy * E1M1_WIDTH + gx] ? 44 : 210;
    }
    const int px = (x >> 13) - E1M1_GX0 + ox;
    const int py = (y >> 13) - E1M1_GY0 + oy;
    if (px >= 1 && px < LITE_W - 1 && py >= 1 && py < LITE_H - 1) {
        for (int yy = -1; yy <= 1; ++yy)
            for (int xx = -1; xx <= 1; ++xx)
                pixels[(py + yy) * LITE_W + px + xx] = 0;
    }
}

static void move_player(int32_t *x, int32_t *y, uint8_t facing, int forward) {
    const int32_t dx = (sine_q14((uint8_t)(facing + 64u)) * 12 * forward) >> 6;
    const int32_t dy = (sine_q14(facing) * 12 * forward) >> 6;
    const int32_t next_x = *x + dx;
    const int32_t next_y = *y + dy;
    /* A small two-axis clearance keeps the player out of rasterized walls. */
    if (!wall_at_q8(next_x + 8 * 256, *y) &&
        !wall_at_q8(next_x - 8 * 256, *y)) *x = next_x;
    if (!wall_at_q8(*x, next_y + 8 * 256) &&
        !wall_at_q8(*x, next_y - 8 * 256)) *y = next_y;
}

static void doom_lite_task(void *unused) {
    (void)unused;
    uint8_t *pixels = SystemUIBorrowFrameBuffer();
    if (!pixels) {
        printf("DOOMLITE_ERROR framebuffer\n");
        SystemUIResume();
        SkyOS_DoomLiteRunning = 0;
        vTaskDelete(NULL);
        return;
    }
    int32_t x = E1M1_START_X * 256, y = E1M1_START_Y * 256;
    uint8_t facing = 0;
    int map_mode = 0;
    uint16_t previous_key = 0;
    uint32_t next_key_ms = 0, next_frame_ms = 0;
    uint32_t batch_start_ms;
    uint32_t previous_frame_ms = 0, max_interval_ms = 0;
    uint32_t batch_render_ms = 0, batch_queue_ms = 0;
    unsigned frames = 0;

    while (ll_vm_check_key() >> 16) vTaskDelay(pdMS_TO_TICKS(20));
    batch_start_ms = ll_get_time_ms();
    for (;;) {
        const uint32_t now = ll_get_time_ms();
        const uint32_t raw = ll_vm_check_key();
        const uint16_t key = raw >> 16 ? (uint16_t)raw : 0;
        if (key == KEY_F6 || key == KEY_ON) break;
        if (key && (key != previous_key || (int32_t)(now - next_key_ms) >= 0)) {
            if (key == KEY_LEFT) facing -= 3u;
            if (key == KEY_RIGHT) facing += 3u;
            if (key == KEY_UP) move_player(&x, &y, facing, 1);
            if (key == KEY_DOWN) move_player(&x, &y, facing, -1);
            if (key == KEY_F5 && key != previous_key) map_mode = !map_mode;
            next_key_ms = now + 90u;
        }
        previous_key = key;

        if ((int32_t)(now - next_frame_ms) >= 0) {
            const uint32_t render_begin = ll_get_time_ms();
            if (previous_frame_ms) {
                const uint32_t interval = render_begin - previous_frame_ms;
                if (interval > max_interval_ms) max_interval_ms = interval;
            }
            previous_frame_ms = render_begin;
            if (map_mode) draw_map(pixels, x, y);
            else draw_scene(pixels, x, y, facing);
            const uint32_t render_end = ll_get_time_ms();
            present(pixels);
            const uint32_t end = ll_get_time_ms();
            batch_render_ms += render_end - render_begin;
            batch_queue_ms += end - render_end;
            if (++frames % 32u == 0u) {
                printf("DOOMLITE_PERF frames=32 elapsed_ms=%lu render_ms=%lu queue_ms=%lu max_interval_ms=%lu map=%d\n",
                       (unsigned long)(end - batch_start_ms),
                       (unsigned long)batch_render_ms,
                       (unsigned long)batch_queue_ms,
                       (unsigned long)max_interval_ms, map_mode);
                batch_start_ms = end;
                batch_render_ms = batch_queue_ms = max_interval_ms = 0;
            }
            next_frame_ms = render_begin + 33u;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    printf("DOOMLITE_EXIT frames=%u\n", frames);
    for (unsigned release_wait = 0; (ll_vm_check_key() >> 16) &&
            release_wait < 50u; ++release_wait)
        vTaskDelay(pdMS_TO_TICKS(20));
    SystemUIResume();
    SkyOS_DoomLiteRunning = 0;
    vTaskDelete(NULL);
}

void DoomLite_Start(void) {
    if (SkyOS_DoomLiteRunning) return;
    SkyOS_DoomLiteRunning = 1;
    if (xTaskCreate(doom_lite_task, "DoomLite", 2048, NULL,
                    configMAX_PRIORITIES - 3, NULL) != pdPASS) {
        SkyOS_DoomLiteRunning = 0;
        printf("DOOMLITE_ERROR task allocation\n");
    }
}
#endif
