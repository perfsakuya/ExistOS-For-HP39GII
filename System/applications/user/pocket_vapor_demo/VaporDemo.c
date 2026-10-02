#include <stdint.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "SystemUI.h"
#include "keyboard_gii39.h"
#include "sys_llapi.h"
#include "VaporDisplay.h"
#include "profile.h"

#define VAPOR_CELL_H 8
#define VAPOR_TOP ((LCD_PIX_H - VP_GRID_H * VAPOR_CELL_H) / 2)

extern uint32_t getHeapAllocateSize(void);
extern uint32_t TotalAllocatableSize;

static int map_button(uint16_t key) {
    switch (key) {
    case KEY_ENTER: return 0; /* Pocket A: reset */
    case KEY_RIGHT: return 4;
    case KEY_LEFT: return 5;
    case KEY_UP: return 6;
    case KEY_DOWN: return 7;
    default: return -1;
    }
}

static void present_rows(uint8_t *pixels, uint32_t changed) {
    uint32_t row;
    for (row = 0; row < VP_GRID_H; ++row) {
        if (!(changed & (1u << row))) continue;
        const uint32_t y0 = VAPOR_TOP + row * VAPOR_CELL_H;
        /* The display queue borrows this persistent UI framebuffer. Each
         * supplied area is tightly packed because the row spans full width. */
        ll_disp_put_area(pixels + y0 * LCD_PIX_W, 0, y0,
                         LCD_PIX_W - 1, y0 + VAPOR_CELL_H - 1);
    }
}

static void vapor_task(void *unused) {
    uint8_t *pixels;
    uint16_t previous = 0;
    int held = 0;
    uint32_t init_start, init_end, bench_end, queue_end;
    unsigned i;
    (void)unused;

    pixels = SystemUIBorrowFrameBuffer();
    if (!pixels) {
        printf("Vapor demo: no UI framebuffer\n");
        SystemUIResume();
        vTaskDelete(NULL);
        return;
    }

    init_start = ll_get_time_us();
    SkyVapor_Init(pixels);
    init_end = ll_get_time_us();
    /* Measure the generated state/render path without display-queue traffic.
     * Each pair ends at count zero, leaving the initial screen unchanged. */
    for (i = 0; i < 32; ++i) {
        SkyVapor_Press(6); /* Up */
        SkyVapor_Press(7); /* Down */
    }
    bench_end = ll_get_time_us();
    ll_disp_put_area(pixels, 0, 0, LCD_PIX_W - 1, LCD_PIX_H - 1);
    queue_end = ll_get_time_us();
    printf("SKYVAPOR_BOOT init_us=%lu render64_us=%lu queue_us=%lu "
           "alloc=%lu total=%lu\n",
           (unsigned long)(init_end - init_start),
           (unsigned long)(bench_end - init_end),
           (unsigned long)(queue_end - bench_end),
           (unsigned long)getHeapAllocateSize(),
           (unsigned long)TotalAllocatableSize);

    /* Do not interpret the ENTER key that launched the app as reset. */
    while (ll_vm_check_key() >> 16) vTaskDelay(pdMS_TO_TICKS(20));

    for (;;) {
        const uint32_t raw = ll_vm_check_key();
        if (raw >> 16) {
            const uint16_t key = raw & 0xffff;
            if (!held || key != previous) {
                if (key == KEY_F6 || key == KEY_ON) break;
                const int button = map_button(key);
                if (button >= 0) {
                    const uint32_t render_start = ll_get_time_us();
                    const uint32_t changed = SkyVapor_Press((uint8_t)button);
                    const uint32_t render_end = ll_get_time_us();
                    if (changed) present_rows(pixels, changed);
                    const uint32_t queue_end = ll_get_time_us();
                    printf("SKYVAPOR_KEY key=%u rows=%lu render_us=%lu "
                           "queue_us=%lu alloc=%lu\n",
                           (unsigned)key, (unsigned long)changed,
                           (unsigned long)(render_end - render_start),
                           (unsigned long)(queue_end - render_end),
                           (unsigned long)getHeapAllocateSize());
                }
                held = 1;
                previous = key;
            }
        } else {
            held = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    printf("SKYVAPOR_EXIT alloc=%lu\n", (unsigned long)getHeapAllocateSize());
    SystemUIResume();
    vTaskDelete(NULL);
}

void VaporDemo_Start(void) {
    const uint32_t before = getHeapAllocateSize();
    const BaseType_t created = xTaskCreate(vapor_task, "VaporDemo", 2048, NULL,
                                           configMAX_PRIORITIES - 3, NULL);
    printf("SKYVAPOR_TASK created=%ld alloc_before=%lu alloc_after=%lu\n",
           (long)created, (unsigned long)before,
           (unsigned long)getHeapAllocateSize());
    if (created != pdPASS) {
        printf("Vapor demo: failed to create task\n");
    }
}
