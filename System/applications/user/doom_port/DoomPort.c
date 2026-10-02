/* SkyOS platform bridge for the GPL-licensed GBADoom engine. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomdef.h"
#include "d_event.h"
#include "d_main.h"
#include "global_data.h"
#include "i_system_e32.h"
#include "i_video.h"
#include "z_zone.h"

#include "FreeRTOS.h"
#include "task.h"
#include "SystemUI.h"
#include "keyboard_gii39.h"
#include "sys_llapi.h"

#define DOOM_WIDTH 120u
#define DOOM_HEIGHT 120u
#define DOOM_PIXELS (DOOM_WIDTH * DOOM_HEIGHT)
#define DOOM_X ((LCD_PIX_W - DOOM_WIDTH) / 2u)
#define DOOM_Y ((LCD_PIX_H - DOOM_HEIGHT) / 2u)

extern uint32_t getHeapAllocateSize(void);
extern void *mainzone;

static unsigned short *game_pixels;
static uint8_t *lcd_pixels;
static jmp_buf app_exit;
static volatile int app_running;
static uint16_t previous_key;
static int previous_code;
static unsigned frames;

/* The display FIFO has four entries. Five one-pixel requests issued after
 * the scene force its source buffer to have been consumed before reuse. */
static void display_barrier(void) {
    for (unsigned i = 0; i < 5; ++i)
        ll_disp_put_area((uint8_t *)game_pixels, 0, 0, 0, 0);
}

int SkyOS_DoomGetTics(void) {
    const uint32_t ms = ll_get_time_ms();
    return (int)((ms / 1000u) * TICRATE + ((ms % 1000u) * TICRATE) / 1000u);
}

void I_InitScreen_e32(void) { previous_key = 0; previous_code = 0; frames = 0; }
void I_CreateBackBuffer_e32(void) { memset(game_pixels, 0, DOOM_PIXELS * sizeof(*game_pixels)); }
int I_GetVideoWidth_e32(void) { return DOOM_WIDTH; }
int I_GetVideoHeight_e32(void) { return DOOM_HEIGHT; }
unsigned short *I_GetBackBuffer(void) { return game_pixels; }
unsigned short *I_GetFrontBuffer(void) { return game_pixels; }
void I_SetPallete_e32(const byte *palette) { (void)palette; }

static int map_key(uint16_t key) {
    switch (key) {
    case KEY_UP: return KEYD_UP;
    case KEY_DOWN: return KEYD_DOWN;
    case KEY_LEFT: return KEYD_LEFT;
    case KEY_RIGHT: return KEYD_RIGHT;
    case KEY_F1: return KEYD_B;       /* fire */
    case KEY_F2: return KEYD_A;       /* use / confirm */
    case KEY_F3: return KEYD_L;       /* strafe left */
    case KEY_F4: return KEYD_R;       /* strafe right */
    case KEY_F5: return KEYD_SELECT;  /* automap */
    case KEY_ENTER: return KEYD_START; /* menu */
    default: return 0;
    }
}

static void post_key(evtype_t type, int code) {
    if (!code) return;
    event_t event = {type, code, 0, 0};
    D_PostEvent(&event);
}

void I_ProcessKeyEvents(void) {
    const uint32_t raw = ll_vm_check_key();
    const uint16_t key = raw >> 16 ? (uint16_t)raw : 0;
    if (key == KEY_F6 || key == KEY_ON) I_Quit_e32();
    if (key == previous_key) return;
    post_key(ev_keyup, previous_code);
    previous_key = key;
    previous_code = map_key(key);
    post_key(ev_keydown, previous_code);
}

void I_FinishUpdate_e32(const byte *source, const byte *palette,
                        unsigned width, unsigned height) {
    if (width != DOOM_WIDTH || height != DOOM_HEIGHT) I_Error("screen size");
    const unsigned short *pixels = (const unsigned short *)source;
    for (unsigned i = 0; i < DOOM_PIXELS; ++i) {
        const unsigned color = pixels[i] & 255u;
        lcd_pixels[i] = palette
            ? (uint8_t)((palette[3u * color] * 77u +
                         palette[3u * color + 1u] * 150u +
                         palette[3u * color + 2u] * 29u) >> 8)
            : (uint8_t)color;
    }
    ll_disp_put_area(lcd_pixels, DOOM_X, DOOM_Y,
                     DOOM_X + DOOM_WIDTH - 1u, DOOM_Y + DOOM_HEIGHT - 1u);
    display_barrier();
    if (++frames % 64u == 0u)
        printf("DOOM_FRAME count=%u allocated=%lu\n", frames,
               (unsigned long)getHeapAllocateSize());
}

void I_Quit_e32(void) { longjmp(app_exit, 1); }

void I_Error(const char *message, ...) {
    va_list args;
    va_start(args, message);
    printf("DOOM_ERROR ");
    vprintf(message, args);
    printf("\n");
    va_end(args);
    longjmp(app_exit, 2);
}

static void doom_task(void *unused) {
    (void)unused;
    game_pixels = (unsigned short *)SystemUIBorrowFrameBuffer();
    lcd_pixels = NULL;
    if (!game_pixels) {
        printf("DOOM_ERROR ui framebuffer\n");
        goto done;
    }
    lcd_pixels = malloc(DOOM_PIXELS);
    if (!lcd_pixels) {
        printf("DOOM_ERROR lcd framebuffer\n");
        goto done;
    }
    while (ll_vm_check_key() >> 16) vTaskDelay(pdMS_TO_TICKS(20));
    printf("DOOM_START allocated=%lu\n", (unsigned long)getHeapAllocateSize());

    const int outcome = setjmp(app_exit);
    if (!outcome) {
        I_PreInitGraphics();
        Z_Init();
        InitGlobals();
        D_DoomMain();
    }
    printf("DOOM_EXIT outcome=%d frames=%u allocated=%lu\n", outcome,
           frames, (unsigned long)getHeapAllocateSize());
    if (mainzone) { free(mainzone); mainzone = NULL; }
    _g = NULL;
    display_barrier();
done:
    if (lcd_pixels) free(lcd_pixels);
    SystemUIResume();
    app_running = 0;
    vTaskDelete(NULL);
}

void DoomPort_Start(void) {
    if (app_running) return;
    app_running = 1;
    if (xTaskCreate(doom_task, "Doom", 4096, NULL,
                    configMAX_PRIORITIES - 3, NULL) != pdPASS) {
        app_running = 0;
        printf("DOOM_ERROR task allocation\n");
    }
}
