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
    (void)unused;

    pixels = SystemUIBorrowFrameBuffer();
    if (!pixels) {
        printf("Vapor demo: no UI framebuffer\n");
        SystemUIResume();
        vTaskDelete(NULL);
        return;
    }

    SkyVapor_Init(pixels);
    ll_disp_put_area(pixels, 0, 0, LCD_PIX_W - 1, LCD_PIX_H - 1);

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
                    const uint32_t changed = SkyVapor_Press((uint8_t)button);
                    if (changed) present_rows(pixels, changed);
                }
                held = 1;
                previous = key;
            }
        } else {
            held = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    SystemUIResume();
    vTaskDelete(NULL);
}

void VaporDemo_Start(void) {
    if (xTaskCreate(vapor_task, "VaporDemo", 2048, NULL,
                    configMAX_PRIORITIES - 3, NULL) != pdPASS) {
        printf("Vapor demo: failed to create task\n");
    }
}
