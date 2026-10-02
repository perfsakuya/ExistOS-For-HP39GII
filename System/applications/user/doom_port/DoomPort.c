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
#define DOOM_LCD_WIDTH (DOOM_WIDTH * 2u)
#define DOOM_LCD_PIXELS (DOOM_LCD_WIDTH * DOOM_HEIGHT)
#define DOOM_FRAME_GUARD 4096u
#define DOOM_X ((LCD_PIX_W - DOOM_LCD_WIDTH) / 2u)
#define DOOM_Y ((LCD_PIX_H - DOOM_HEIGHT) / 2u)

extern uint32_t getHeapAllocateSize(void);
extern void *mainzone;

static unsigned short *game_pixels;
static uint8_t *game_buffer_allocation;
static uint8_t *lcd_pixels;
static jmp_buf app_exit;
static volatile int app_running;
static uint16_t previous_key;
static int previous_code;
static unsigned frames;
static uint8_t gray_palette[256];
static const byte *gray_palette_source;
static uint8_t gray_palette_ready;
static uint32_t last_frame_ms;
static uint32_t perf_wall_ms;
static uint32_t perf_convert_ms;
static uint32_t perf_display_ms;
static unsigned perf_samples;
static uint32_t phase_logic_ms;
static uint32_t phase_draw_ms;
static unsigned phase_samples;
static uint32_t render_setup_ms;
static uint32_t render_bsp_ms;
static uint32_t render_planes_ms;
static uint32_t render_masked_ms;
static unsigned render_samples;

/* The display FIFO has four entries. Five one-pixel requests issued after
 * the scene force its source buffer to have been consumed before reuse. */
static void display_barrier(void) {
    for (unsigned i = 0; i < 5; ++i)
        ll_disp_put_area((uint8_t *)game_pixels, 0, 0, 0, 0);
}

static int check_game_buffer(void) {
    if (!game_buffer_allocation) return 1;
    const uint8_t *before = game_buffer_allocation;
    const uint8_t *after = (const uint8_t *)game_pixels + DOOM_LCD_PIXELS;
    for (unsigned i = 0; i < DOOM_FRAME_GUARD; ++i) {
        if (before[i] != 0xa5u) {
            printf("DOOM_GUARD underrun offset=%u value=%u\n", i, before[i]);
            return 0;
        }
        if (after[i] != 0xa5u) {
            printf("DOOM_GUARD overrun offset=%u value=%u\n", i, after[i]);
            return 0;
        }
    }
    return 1;
}

int SkyOS_DoomGetTics(void) {
    const uint32_t ms = ll_get_time_ms();
    return (int)((ms / 1000u) * TICRATE + ((ms % 1000u) * TICRATE) / 1000u);
}

unsigned SkyOS_DoomNowMs(void) { return ll_get_time_ms(); }

void SkyOS_DoomProfilePhases(unsigned logic_ms, unsigned draw_ms) {
    phase_logic_ms += logic_ms;
    phase_draw_ms += draw_ms;
    if (++phase_samples == 16u) {
        printf("DOOM_PHASE frames=%u logic_ms=%lu draw_ms=%lu\n",
               phase_samples, (unsigned long)phase_logic_ms,
               (unsigned long)phase_draw_ms);
        phase_logic_ms = phase_draw_ms = 0;
        phase_samples = 0;
    }
}

void SkyOS_DoomProfileRender(unsigned setup_ms, unsigned bsp_ms,
                            unsigned planes_ms, unsigned masked_ms) {
    render_setup_ms += setup_ms;
    render_bsp_ms += bsp_ms;
    render_planes_ms += planes_ms;
    render_masked_ms += masked_ms;
    if (++render_samples == 16u) {
        printf("DOOM_RENDER frames=%u setup_ms=%lu bsp_ms=%lu planes_ms=%lu masked_ms=%lu\n",
               render_samples, (unsigned long)render_setup_ms,
               (unsigned long)render_bsp_ms, (unsigned long)render_planes_ms,
               (unsigned long)render_masked_ms);
        render_setup_ms = render_bsp_ms = render_planes_ms = render_masked_ms = 0;
        render_samples = 0;
    }
}

void I_InitScreen_e32(void) {
    previous_key = 0;
    previous_code = 0;
    frames = 0;
    gray_palette_source = NULL;
    gray_palette_ready = 0;
    last_frame_ms = 0;
    perf_wall_ms = perf_convert_ms = perf_display_ms = 0;
    perf_samples = 0;
    phase_logic_ms = phase_draw_ms = 0;
    phase_samples = 0;
    render_setup_ms = render_bsp_ms = render_planes_ms = render_masked_ms = 0;
    render_samples = 0;
}
void I_CreateBackBuffer_e32(void) { memset(game_pixels, 0, DOOM_PIXELS * sizeof(*game_pixels)); }
int I_GetVideoWidth_e32(void) { return DOOM_WIDTH; }
int I_GetVideoHeight_e32(void) { return DOOM_HEIGHT; }
unsigned short *I_GetBackBuffer(void) { return game_pixels; }
unsigned short *I_GetFrontBuffer(void) { return game_pixels; }
void I_SetPallete_e32(const byte *palette) {
    for (unsigned i = 0; i < 256u; ++i) {
        gray_palette[i] = palette
            ? (uint8_t)((palette[3u * i] * 77u +
                         palette[3u * i + 1u] * 150u +
                         palette[3u * i + 2u] * 29u) >> 8)
            : (uint8_t)i;
    }
    gray_palette_source = palette;
    gray_palette_ready = 1;
}

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
    if (!check_game_buffer()) I_Error("framebuffer guard");
    const uint32_t convert_start_ms = ll_get_time_ms();
    if (!gray_palette_ready || palette != gray_palette_source)
        I_SetPallete_e32(palette);
    // The engine writes two palette indices into each 16-bit pixel: the 3D
    // renderer duplicates them, while the automap and menus draw at 240 px.
    // Read both bytes so odd columns are not dropped from the LCD output.
    for (unsigned i = 0; i < DOOM_LCD_PIXELS; ++i)
        lcd_pixels[i] = gray_palette[source[i]];
    const uint32_t display_start_ms = ll_get_time_ms();
    ll_disp_put_area(lcd_pixels, DOOM_X, DOOM_Y,
                     DOOM_X + DOOM_LCD_WIDTH - 1u, DOOM_Y + DOOM_HEIGHT - 1u);
    display_barrier();
    const uint32_t frame_done_ms = ll_get_time_ms();
    if (last_frame_ms) {
        perf_wall_ms += frame_done_ms - last_frame_ms;
        perf_convert_ms += display_start_ms - convert_start_ms;
        perf_display_ms += frame_done_ms - display_start_ms;
        if (++perf_samples == 16u) {
            const uint32_t other_ms = perf_wall_ms > perf_convert_ms + perf_display_ms
                ? perf_wall_ms - perf_convert_ms - perf_display_ms : 0u;
            printf("DOOM_PERF frames=%u wall_ms=%lu convert_ms=%lu display_ms=%lu other_ms=%lu\n",
                   perf_samples, (unsigned long)perf_wall_ms,
                   (unsigned long)perf_convert_ms, (unsigned long)perf_display_ms,
                   (unsigned long)other_ms);
            perf_wall_ms = perf_convert_ms = perf_display_ms = 0;
            perf_samples = 0;
        }
    }
    last_frame_ms = frame_done_ms;
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
    uint8_t *ui_pixels = SystemUIBorrowFrameBuffer();
    game_pixels = NULL;
    game_buffer_allocation = NULL;
    lcd_pixels = NULL;
    if (!ui_pixels) {
        printf("DOOM_ERROR ui framebuffer\n");
        goto done;
    }
    // A game render must not overwrite objects adjoining the UI framebuffer.
    // Keep both guards so any engine draw outside 240 x 120 is reported.
    game_buffer_allocation = malloc(DOOM_LCD_PIXELS + 2u * DOOM_FRAME_GUARD);
    if (!game_buffer_allocation) {
        printf("DOOM_ERROR game framebuffer\n");
        goto done;
    }
    memset(game_buffer_allocation, 0xa5,
           DOOM_LCD_PIXELS + 2u * DOOM_FRAME_GUARD);
    game_pixels = (unsigned short *)(game_buffer_allocation + DOOM_FRAME_GUARD);
    lcd_pixels = malloc(DOOM_LCD_PIXELS);
    if (!lcd_pixels) {
        printf("DOOM_ERROR lcd framebuffer\n");
        goto done;
    }
    printf("DOOM_BUFFERS ui=%p game=%p lcd=%p\n",
           ui_pixels, game_pixels, lcd_pixels);
    // Remove the suspended UI from the unused LCD margins before the first
    // game frame.
    memset(ui_pixels, 0, LCD_PIX_W * LCD_PIX_H);
    ll_disp_put_area(ui_pixels, 0, 0, LCD_PIX_W - 1u, LCD_PIX_H - 1u);
    display_barrier();
    while (ll_vm_check_key() >> 16) vTaskDelay(pdMS_TO_TICKS(20));
    printf("DOOM_START allocated=%lu\n", (unsigned long)getHeapAllocateSize());

    const int outcome = setjmp(app_exit);
    if (!outcome) {
        I_PreInitGraphics();
        // Keep swap enabled after exit: dirty zone pages may remain cached
        // and must be writable to the FTL when they are later evicted.
        ll_mem_swap_enable(true);
        Z_Init();
        InitGlobals();
        D_DoomMain();
    }
    printf("DOOM_EXIT outcome=%d frames=%u allocated=%lu\n", outcome,
           frames, (unsigned long)getHeapAllocateSize());
    mainzone = NULL; // The SkyOS Doom zone is a fixed VM RAM region, not malloc storage.
    _g = NULL;
    display_barrier();
done:
    if (game_pixels) check_game_buffer();
    if (lcd_pixels) free(lcd_pixels);
    if (game_buffer_allocation) free(game_buffer_allocation);
    game_buffer_allocation = NULL;
    game_pixels = NULL;
    printf("DOOM_CLEANUP resuming_ui\n");
    SystemUIResume();
    printf("DOOM_CLEANUP ui_resumed\n");
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
