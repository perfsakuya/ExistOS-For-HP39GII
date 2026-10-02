#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"
#include "SystemUI.h"
#include "keyboard_gii39.h"
#include "sys_llapi.h"
#include "DoomProbeRender.h"

#define PROBE_ZONE_PRIMARY (160u * 1024u)
#define PROBE_ZONE_FALLBACK (128u * 1024u)
#define PROBE_BURST_FRAMES 16u
#define PROBE_RENDER_FRAMES 64u
#define PROBE_X ((LCD_PIX_W - DOOM_PROBE_W) / 2)
#define PROBE_Y ((LCD_PIX_H - DOOM_PROBE_H) / 2)

extern uint32_t getHeapAllocateSize(void);
extern uint32_t TotalAllocatableSize;

static void fill_screen_pattern(uint8_t *pixels) {
    unsigned y;
    for (y = 0; y < LCD_PIX_H; ++y) {
        unsigned x;
        for (x = 0; x < LCD_PIX_W; ++x) {
            pixels[y * LCD_PIX_W + x] =
                ((x / 16u + y / 16u) & 1u) ? 210u : 45u;
        }
    }
}

static int touch_and_check(uint8_t *zone, unsigned bytes) {
    unsigned offset;
    for (offset = 0; offset < bytes; offset += 1024u)
        zone[offset] = (uint8_t)(offset / 1024u ^ 0x5au);
    for (offset = 0; offset < bytes; offset += 1024u)
        if (zone[offset] != (uint8_t)(offset / 1024u ^ 0x5au))
            return 0;
    return 1;
}

/* Six FIFO entries cannot all fit in the loader's four-entry queue at once.
 * After a scene request followed by five persistent-buffer sentinels, the
 * scene has completed before the fifth sentinel can be enqueued. */
static void display_barrier(uint8_t *ui_pixels) {
    unsigned i;
    for (i = 0; i < 5; ++i)
        ll_disp_put_area(ui_pixels, 0, 0, 0, 0);
}

static void present_scene(uint8_t *scene, uint8_t *ui_pixels,
                          uint8_t angle) {
    const uint32_t started = ll_get_time_us();
    DoomProbe_Render(scene, angle);
    const uint32_t rendered = ll_get_time_us();
    ll_disp_put_area(scene, PROBE_X, PROBE_Y,
                     PROBE_X + DOOM_PROBE_W - 1,
                     PROBE_Y + DOOM_PROBE_H - 1);
    const uint32_t queued = ll_get_time_us();
    printf("DOOMPROBE_SCENE angle=%u render_us=%lu queue_us=%lu\n",
           (unsigned)angle, (unsigned long)(rendered - started),
           (unsigned long)(queued - rendered));
    display_barrier(ui_pixels);
}

static void doom_probe_task(void *unused) {
    uint8_t *ui_pixels;
    uint8_t *scene;
    uint8_t *zone = NULL;
    unsigned zone_bytes = 0;
    unsigned i;
    uint32_t physical_free = 0;
    uint32_t physical_total = 0;
    uint16_t previous_key = 0;
    int held = 0;
    uint8_t angle = 0;
    (void)unused;

    ui_pixels = SystemUIBorrowFrameBuffer();
    if (!ui_pixels) {
        printf("DOOMPROBE_ERROR ui_framebuffer\n");
        SystemUIResume();
        vTaskDelete(NULL);
        return;
    }

    const uint32_t before = getHeapAllocateSize();
    scene = (uint8_t *)malloc(DOOM_PROBE_W * DOOM_PROBE_H);
    if (!scene) {
        printf("DOOMPROBE_ERROR scene_allocation\n");
        SystemUIResume();
        vTaskDelete(NULL);
        return;
    }

    zone = (uint8_t *)malloc(PROBE_ZONE_PRIMARY);
    if (zone) {
        zone_bytes = PROBE_ZONE_PRIMARY;
    } else {
        zone = (uint8_t *)malloc(PROBE_ZONE_FALLBACK);
        if (zone) zone_bytes = PROBE_ZONE_FALLBACK;
    }
    const int zone_ok = zone ? touch_and_check(zone, zone_bytes) : 0;
    ll_mem_phy_info(&physical_free, &physical_total);
    printf("DOOMPROBE_MEMORY before=%lu allocated=%lu total=%lu "
           "zone=%u touched=%d phy_free=%lu phy_total=%lu\n",
           (unsigned long)before,
           (unsigned long)getHeapAllocateSize(),
           (unsigned long)TotalAllocatableSize,
           zone_bytes, zone_ok,
           (unsigned long)physical_free,
           (unsigned long)physical_total);

    fill_screen_pattern(ui_pixels);
    const uint32_t burst_start = ll_get_time_us();
    for (i = 0; i < PROBE_BURST_FRAMES; ++i)
        ll_disp_put_area(ui_pixels, 0, 0, LCD_PIX_W - 1, LCD_PIX_H - 1);
    const uint32_t burst_end = ll_get_time_us();
    printf("DOOMPROBE_LCD queued=%u elapsed_us=%lu area=%u\n",
           PROBE_BURST_FRAMES,
           (unsigned long)(burst_end - burst_start),
           LCD_PIX_W * LCD_PIX_H);

    /* Force every full-screen request through the FIFO before the next
     * drawing changes its shared input buffer. */
    display_barrier(ui_pixels);

    const uint32_t render_start = ll_get_time_us();
    for (i = 0; i < PROBE_RENDER_FRAMES; ++i)
        DoomProbe_Render(scene, (uint8_t)(i * 4u));
    const uint32_t render_end = ll_get_time_us();
    printf("DOOMPROBE_RENDER frames=%u elapsed_us=%lu pixels=%u\n",
           PROBE_RENDER_FRAMES,
           (unsigned long)(render_end - render_start),
           DOOM_PROBE_W * DOOM_PROBE_H);

    present_scene(scene, ui_pixels, angle);
    while (ll_vm_check_key() >> 16) vTaskDelay(pdMS_TO_TICKS(20));

    for (;;) {
        const uint32_t raw = ll_vm_check_key();
        if (raw >> 16) {
            const uint16_t key = (uint16_t)raw;
            if (!held || key != previous_key) {
                if (key == KEY_F6 || key == KEY_ON) break;
                if (key == KEY_LEFT || key == KEY_RIGHT) {
                    angle = (uint8_t)(angle + (key == KEY_LEFT ? -8 : 8));
                    present_scene(scene, ui_pixels, angle);
                }
                held = 1;
                previous_key = key;
            }
        } else {
            held = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    /* present_scene() drained its source buffer before this point. */
    if (zone) free(zone);
    free(scene);
    printf("DOOMPROBE_EXIT allocated=%lu\n",
           (unsigned long)getHeapAllocateSize());
    SystemUIResume();
    vTaskDelete(NULL);
}

void DoomProbe_Start(void) {
    const BaseType_t created =
        xTaskCreate(doom_probe_task, "DoomProbe", 2048, NULL,
                    configMAX_PRIORITIES - 3, NULL);
    if (created != pdPASS)
        printf("DOOMPROBE_ERROR task_allocation\n");
}
