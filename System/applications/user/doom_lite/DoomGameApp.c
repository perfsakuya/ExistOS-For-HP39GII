/* SkyOS E1M1 gameplay shell: 35 Hz rules and the compact Lite renderer. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "SystemUI.h"
#include "keyboard_gii39.h"
#include "sys_llapi.h"
#include "DoomLiteGame.h"
#include "DoomLiteRender.h"

#define GAME_PIXELS (LCD_PIX_W * LCD_PIX_H)
#define GAME_FRAME_PERIOD_US 33000u
#define GAME_TIC_SCALE 35u
#define GAME_TIC_THRESHOLD 1000000u
#define GAME_MAX_DELTA_US 250000u
#define GAME_MAX_CATCHUP_TICS 6u
#define GAME_PERF_FRAMES 32u
/* KEY_F1 is 0, so an unpressed keyboard must use a distinct value. */
#define GAME_NO_KEY UINT16_MAX

volatile int SkyOS_DoomGameRunning;
static DoomLiteGame game_state;
static uint8_t display_barrier_pixel;

typedef struct {
    uint32_t wall_start_ms;
    uint32_t logic_total_us, logic_max_us;
    uint32_t render_total_us, render_max_us;
    uint32_t lcd_total_us, lcd_max_us;
    uint32_t interval_total_us, interval_max_us;
    uint32_t last_render_us;
    unsigned frames, logic_ticks, interval_samples, dropped_ticks;
} GamePerf;

static void record_time(uint32_t elapsed, uint32_t *total, uint32_t *max) {
    *total += elapsed;
    if (elapsed > *max) *max = elapsed;
}

/* Millisecond fields use rounded microsecond samples, so short logic ticks
 * remain visible even when each individual call takes less than one ms. */
static unsigned long rounded_ms(uint32_t elapsed_us) {
    return (unsigned long)((elapsed_us + 500u) / 1000u);
}

static void log_perf(GamePerf *perf, const DoomLiteGame *game,
                     int map_mode, uint32_t now_ms) {
    printf("DOOMG_PERF frames=%u elapsed_ms=%lu logic_ticks=%u "
           "logic_total_ms=%lu logic_max_ms=%lu logic_total_us=%lu logic_max_us=%lu "
           "render_total_ms=%lu render_max_ms=%lu render_total_us=%lu render_max_us=%lu "
           "lcd_total_ms=%lu lcd_max_ms=%lu lcd_total_us=%lu lcd_max_us=%lu "
           "interval_samples=%u interval_total_ms=%lu interval_max_ms=%lu "
           "dropped_ticks=%u hp=%u ammo=%u kills=%u blue=%u map=%d "
           "x=%ld y=%ld\n",
           perf->frames, (unsigned long)(now_ms - perf->wall_start_ms),
           perf->logic_ticks,
           rounded_ms(perf->logic_total_us), rounded_ms(perf->logic_max_us),
           (unsigned long)perf->logic_total_us,
           (unsigned long)perf->logic_max_us,
           rounded_ms(perf->render_total_us), rounded_ms(perf->render_max_us),
           (unsigned long)perf->render_total_us,
           (unsigned long)perf->render_max_us,
           rounded_ms(perf->lcd_total_us), rounded_ms(perf->lcd_max_us),
           (unsigned long)perf->lcd_total_us,
           (unsigned long)perf->lcd_max_us,
           perf->interval_samples,
           rounded_ms(perf->interval_total_us),
           rounded_ms(perf->interval_max_us),
           perf->dropped_ticks, (unsigned)game->health, (unsigned)game->ammo,
           (unsigned)game->kills, (unsigned)game->blue_key, map_mode,
           (long)(game->x_q8 >> 8), (long)(game->y_q8 >> 8));
    const uint32_t last_render_us = perf->last_render_us;
    memset(perf, 0, sizeof(*perf));
    perf->wall_start_ms = now_ms;
    perf->last_render_us = last_render_us;
}

static const uint8_t glyph_digits[10][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7},
    {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7},
    {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7},
    {7, 5, 7, 1, 7},
};

static const uint8_t glyph_letters[26][5] = {
    {2, 5, 7, 5, 5}, {6, 5, 6, 5, 6}, {3, 4, 4, 4, 3},
    {6, 5, 5, 5, 6}, {7, 4, 6, 4, 7}, {7, 4, 6, 4, 4},
    {3, 4, 5, 5, 3}, {5, 5, 7, 5, 5}, {7, 2, 2, 2, 7},
    {1, 1, 1, 5, 2}, {5, 5, 6, 5, 5}, {4, 4, 4, 4, 7},
    {5, 7, 7, 5, 5}, {5, 7, 7, 7, 5}, {7, 5, 5, 5, 7},
    {6, 5, 6, 4, 4}, {7, 5, 5, 7, 1}, {6, 5, 6, 5, 5},
    {3, 4, 7, 1, 6}, {7, 2, 2, 2, 2}, {5, 5, 5, 5, 7},
    {5, 5, 5, 5, 2}, {5, 5, 7, 7, 5}, {5, 5, 2, 5, 5},
    {5, 5, 2, 2, 2}, {7, 1, 2, 4, 7},
};

static uint8_t glyph_row(char character, unsigned row) {
    if (character >= '0' && character <= '9')
        return glyph_digits[character - '0'][row];
    if (character >= 'A' && character <= 'Z')
        return glyph_letters[character - 'A'][row];
    if (character == ':') return row == 1u || row == 3u ? 2u : 0u;
    return 0u;
}

static void draw_text(uint8_t *pixels, unsigned x, unsigned y,
                      const char *message, unsigned scale) {
    for (; *message; ++message, x += 4u * scale) {
        if (x + 3u * scale > LCD_PIX_W || y + 5u * scale > LCD_PIX_H)
            break;
        for (unsigned row = 0; row < 5u; ++row) {
            const uint8_t bits = glyph_row(*message, row);
            for (unsigned col = 0; col < 3u; ++col) {
                if (!(bits & (4u >> col))) continue;
                for (unsigned sy = 0; sy < scale; ++sy)
                    for (unsigned sx = 0; sx < scale; ++sx)
                        pixels[(y + row * scale + sy) * LCD_PIX_W +
                               x + col * scale + sx] = 16u;
            }
        }
    }
}

static void draw_number(uint8_t *pixels, unsigned x, unsigned y,
                        unsigned number, unsigned places) {
    unsigned divisor = places == 3u ? 100u : 10u;
    while (places--) {
        const char digit[2] = {(char)('0' + (number / divisor) % 10u), 0};
        draw_text(pixels, x, y, digit, 1u);
        x += 4u;
        divisor /= 10u;
    }
}

static void draw_hud(uint8_t *pixels, const DoomLiteGame *game,
                     const char *message) {
    memset(pixels, 237, 8u * LCD_PIX_W);
    draw_text(pixels, 2u, 1u, "HP", 1u);
    draw_number(pixels, 11u, 1u, game->health, 3u);
    draw_text(pixels, 28u, 1u, "AM", 1u);
    draw_number(pixels, 37u, 1u, game->ammo, 3u);
    draw_text(pixels, 53u, 1u, "K", 1u);
    draw_number(pixels, 58u, 1u,
                game->kills > 99u ? 99u : game->kills, 2u);
    draw_text(pixels, 71u, 1u, "B", 1u);
    draw_number(pixels, 76u, 1u, game->blue_key ? 1u : 0u, 2u);
    if (message) draw_text(pixels, 94u, 1u, message, 1u);
}

static void draw_end_panel(uint8_t *pixels, const char *title,
                           unsigned title_x) {
    const unsigned left = 64u, top = 43u, width = 128u, height = 42u;
    for (unsigned y = top; y < top + height; ++y) {
        uint8_t *row = pixels + y * LCD_PIX_W + left;
        memset(row, y == top || y == top + height - 1u ? 20 : 233, width);
        row[0] = row[width - 1u] = 20u;
    }
    draw_text(pixels, title_x, 52u, title, 2u);
    draw_text(pixels, 114u, 72u, "F6 EXIT", 1u);
}

static const char *message_for_event(uint32_t flags) {
    if (flags & DL_EVENT_EXIT) return "EXIT";
    if (flags & DL_EVENT_BLUE_KEY) return "BLUE KEY";
    if (flags & DL_EVENT_NEED_BLUE) return "NEED BLUE";
    if (flags & DL_EVENT_DOOR_OPEN) return "DOOR OPEN";
    if (flags & DL_EVENT_DOOR_CLOSE) return "DOOR CLOSE";
    if (flags & DL_EVENT_KILL) return "KILL";
    if (flags & DL_EVENT_SHOT_HIT) return "SHOT HIT";
    if (flags & DL_EVENT_SHOT_MISS) return "SHOT MISS";
    if (flags & DL_EVENT_HURT) return "HURT";
    if (flags & DL_EVENT_PICKUP) return "PICKUP";
    return NULL;
}

static uint16_t read_buttons(uint16_t key) {
    switch (key) {
        case KEY_UP: return DL_GAME_UP;
        case KEY_DOWN: return DL_GAME_DOWN;
        case KEY_LEFT: return DL_GAME_LEFT;
        case KEY_RIGHT: return DL_GAME_RIGHT;
        case KEY_F1: return DL_GAME_FIRE;
        case KEY_F2: return DL_GAME_USE;
        default: return 0u;
    }
}

static void present(uint8_t *pixels) {
    ll_disp_put_area(pixels, 0, 0, LCD_PIX_W - 1u, LCD_PIX_H - 1u);
    /* LCD DMA borrows pixels. Queue saturation drains the full frame before
     * this task may render into the shared framebuffer again. */
    for (unsigned i = 0; i < 5u; ++i)
        ll_disp_put_area(&display_barrier_pixel, 0, 0, 0, 0);
}

static void doom_game_task(void *unused) {
    (void)unused;
    const uint32_t task_start_ms = ll_get_time_ms();
    printf("DOOMG_BOOT phase=task_start ms=%lu\n",
           (unsigned long)task_start_ms);
    uint8_t *pixels = SystemUIBorrowFrameBuffer();
    if (!pixels) {
        printf("DOOMG_BOOT phase=framebuffer_error ms=%lu\n",
               (unsigned long)ll_get_time_ms());
        SystemUIResume();
        SkyOS_DoomGameRunning = 0;
        vTaskDelete(NULL);
        return;
    }
    printf("DOOMG_BOOT phase=framebuffer ms=%lu pixels=%u\n",
           (unsigned long)ll_get_time_ms(), (unsigned)GAME_PIXELS);
    DoomLiteGame_Init(&game_state);
    printf("DOOMG_BOOT phase=game_init ms=%lu x=%ld y=%ld hp=%u ammo=%u enemies=%u\n",
           (unsigned long)ll_get_time_ms(),
           (long)(game_state.x_q8 >> 8), (long)(game_state.y_q8 >> 8),
           (unsigned)game_state.health, (unsigned)game_state.ammo,
           (unsigned)game_state.total_enemies);

    while (ll_vm_check_key() >> 16) vTaskDelay(pdMS_TO_TICKS(20));
    printf("DOOMG_BOOT phase=ready ms=%lu\n",
           (unsigned long)ll_get_time_ms());
    GamePerf perf = {0};
    perf.wall_start_ms = ll_get_time_ms();
    uint32_t previous_us = ll_get_time_us();
    uint32_t accumulator = 0u;
    uint32_t message_until_tick = 0u;
    const char *message = NULL;
    uint16_t previous_key = GAME_NO_KEY;
    uint16_t pending_actions = 0u;
    unsigned total_frames = 0u;
    int map_mode = 0;

    for (;;) {
        const uint32_t now_us = ll_get_time_us();
        uint32_t elapsed_us = now_us - previous_us;
        previous_us = now_us;
        if (elapsed_us > GAME_MAX_DELTA_US) {
            perf.dropped_ticks += (elapsed_us - GAME_MAX_DELTA_US) /
                                  (GAME_TIC_THRESHOLD / GAME_TIC_SCALE);
            elapsed_us = GAME_MAX_DELTA_US;
        }
        accumulator += elapsed_us * GAME_TIC_SCALE;

        const uint32_t raw_key = ll_vm_check_key();
        const uint16_t key = raw_key >> 16 ? (uint16_t)raw_key : GAME_NO_KEY;
        if (key == KEY_F6 || key == KEY_ON) break;
        if (key == KEY_F5 && previous_key != KEY_F5) {
            map_mode = !map_mode;
            printf("DOOMG_EVENT tick=%lu flags=0x0000 map=%d hp=%u ammo=%u kills=%u blue=%u completed=%u\n",
                   (unsigned long)game_state.ticks, map_mode,
                   (unsigned)game_state.health, (unsigned)game_state.ammo,
                   (unsigned)game_state.kills, (unsigned)game_state.blue_key,
                   (unsigned)game_state.completed);
        }
        /* Keep a short F1/F2 tap until the next 35 Hz tick; the gameplay
         * module still performs held-key edge detection for longer presses. */
        if (key == KEY_F1 && previous_key != KEY_F1)
            pending_actions |= DL_GAME_FIRE;
        if (key == KEY_F2 && previous_key != KEY_F2)
            pending_actions |= DL_GAME_USE;
        previous_key = key;
        uint16_t buttons = read_buttons(key) | pending_actions;

        unsigned run = 0u;
        while (accumulator >= GAME_TIC_THRESHOLD &&
               run < GAME_MAX_CATCHUP_TICS) {
            const uint32_t logic_start_us = ll_get_time_us();
            const uint32_t events = DoomLiteGame_Step(&game_state, buttons);
            pending_actions = 0u;
            buttons = read_buttons(key);
            const uint32_t logic_us = ll_get_time_us() - logic_start_us;
            record_time(logic_us, &perf.logic_total_us, &perf.logic_max_us);
            ++perf.logic_ticks;
            ++run;
            accumulator -= GAME_TIC_THRESHOLD;
            if (events) {
                printf("DOOMG_EVENT tick=%lu flags=0x%04lx hp=%u ammo=%u kills=%u blue=%u completed=%u\n",
                       (unsigned long)game_state.ticks, (unsigned long)events,
                       (unsigned)game_state.health, (unsigned)game_state.ammo,
                       (unsigned)game_state.kills,
                       (unsigned)game_state.blue_key,
                       (unsigned)game_state.completed);
                message = message_for_event(events);
                message_until_tick = game_state.ticks + 70u;
            }
        }
        if (accumulator >= GAME_TIC_THRESHOLD) {
            perf.dropped_ticks += accumulator / GAME_TIC_THRESHOLD;
            accumulator %= GAME_TIC_THRESHOLD;
        }

        const uint32_t render_start_us = ll_get_time_us();
        if (!perf.last_render_us ||
            render_start_us - perf.last_render_us >= GAME_FRAME_PERIOD_US) {
            if (perf.last_render_us) {
                record_time(render_start_us - perf.last_render_us,
                            &perf.interval_total_us, &perf.interval_max_us);
                ++perf.interval_samples;
            }
            perf.last_render_us = render_start_us;
            DoomLite_RenderGameFrame(pixels, game_state.x_q8, game_state.y_q8,
                                     game_state.facing, map_mode,
                                     game_state.door_open);
            if (map_mode)
                DoomLite_RenderGameMapOverlay(pixels, &game_state);
            else
                DoomLite_RenderGameThings(pixels, &game_state);
            if (message && (int32_t)(game_state.ticks - message_until_tick) >= 0)
                message = NULL;
            draw_hud(pixels, &game_state, message);
            if (game_state.completed)
                draw_end_panel(pixels, "E1M1 CLEAR", 88u);
            else if (!game_state.health)
                draw_end_panel(pixels, "GAME OVER", 92u);
            const uint32_t render_us = ll_get_time_us() - render_start_us;
            record_time(render_us, &perf.render_total_us, &perf.render_max_us);
            const uint32_t lcd_start_us = ll_get_time_us();
            present(pixels);
            const uint32_t lcd_us = ll_get_time_us() - lcd_start_us;
            record_time(lcd_us, &perf.lcd_total_us, &perf.lcd_max_us);
            ++total_frames;
            if (++perf.frames == GAME_PERF_FRAMES)
                log_perf(&perf, &game_state, map_mode, ll_get_time_ms());
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (perf.frames || perf.logic_ticks)
        log_perf(&perf, &game_state, map_mode, ll_get_time_ms());
    printf("DOOMG_EXIT phase=key frames=%u ticks=%lu ms=%lu\n",
           total_frames, (unsigned long)game_state.ticks,
           (unsigned long)ll_get_time_ms());
    const uint32_t release_start_ms = ll_get_time_ms();
    for (unsigned i = 0; (ll_vm_check_key() >> 16) && i < 50u; ++i)
        vTaskDelay(pdMS_TO_TICKS(20));
    printf("DOOMG_EXIT phase=key_release wait_ms=%lu\n",
           (unsigned long)(ll_get_time_ms() - release_start_ms));
    printf("DOOMG_EXIT phase=ui_resume_begin ms=%lu\n",
           (unsigned long)ll_get_time_ms());
    SystemUIResume();
    printf("DOOMG_EXIT phase=ui_resume_done ms=%lu\n",
           (unsigned long)ll_get_time_ms());
    SkyOS_DoomGameRunning = 0;
    printf("DOOMG_EXIT phase=task_delete ms=%lu\n",
           (unsigned long)ll_get_time_ms());
    vTaskDelete(NULL);
}

void DoomGame_Start(void) {
    if (SkyOS_DoomGameRunning) return;
    SkyOS_DoomGameRunning = 1;
    if (xTaskCreate(doom_game_task, "DoomGame", 2048, NULL,
                    configMAX_PRIORITIES - 3, NULL) != pdPASS) {
        SkyOS_DoomGameRunning = 0;
        printf("DOOMG_BOOT phase=task_allocation_error ms=%lu\n",
               (unsigned long)ll_get_time_ms());
    }
}
