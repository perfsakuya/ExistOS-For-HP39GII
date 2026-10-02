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
#include "../doom_lite/DoomLiteRender.h"

#define DOOM_WIDTH 120u
#define DOOM_HEIGHT 120u
#define DOOM_PIXELS (DOOM_WIDTH * DOOM_HEIGHT)
#define DOOM_LCD_WIDTH (DOOM_WIDTH * 2u)
#define DOOM_LCD_PIXELS (DOOM_LCD_WIDTH * DOOM_HEIGHT)
#define DOOM_FRAME_GUARD 4096u
#define DOOM_NO_KEY UINT16_MAX /* KEY_F1 is zero; zero cannot mean no key. */
#define DOOM_X ((LCD_PIX_W - DOOM_LCD_WIDTH) / 2u)
#define DOOM_Y ((LCD_PIX_H - DOOM_HEIGHT) / 2u)


extern uint32_t getHeapAllocateSize(void);
extern void *mainzone;
extern volatile uint32_t ulCriticalNesting;

static unsigned short *game_pixels;
static uint8_t *game_buffer_allocation;
static uint8_t *lcd_pixels;
static jmp_buf app_exit;
static volatile int app_running;
static uint16_t previous_key;
static int previous_code;
static unsigned frames;
static volatile int exit_requested;
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
static uint32_t phase_logic_max_ms;
static uint32_t phase_draw_max_ms;
static unsigned phase_samples;
static uint32_t tic_sum_ms;
static uint32_t tic_max_ms;
static unsigned tic_samples;
static uint32_t render_setup_ms;
static uint32_t render_bsp_ms;
static uint32_t render_planes_ms;
static uint32_t render_masked_ms;
static unsigned render_samples;
int SkyOS_DoomFlatWalls;
int SkyOS_DoomFastMode;
volatile int SkyOS_DoomFastRunning;
static uint8_t *fast_pixels;
static uint32_t fast_batch_start_ms;
static uint32_t fast_render_ms;
static uint32_t fast_present_ms;
static uint32_t fast_last_frame_ms;
static uint32_t fast_max_interval_ms;
static uint32_t fast_max_render_ms;
static uint32_t fast_max_present_ms;
static unsigned fast_frames;
static unsigned fast_total_frames;

/* The display FIFO has four entries. Five one-pixel requests issued after
 * the scene force its source buffer to have been consumed before reuse. */
static void display_barrier(void) {
    static uint8_t barrier_pixel;
    for (unsigned i = 0; i < 5; ++i)
        ll_disp_put_area(&barrier_pixel, 0, 0, 0, 0);
}

/* A small status strip makes original game state visible in the fast mode.
 * The Lite scene itself is still a static approximation of E1M1 geometry. */
static void fast_glyph(uint8_t *pixels, unsigned x, char glyph) {
    static const uint8_t digits[10][5] = {
        {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7},
        {5,5,7,1,1}, {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1},
        {7,5,7,5,7}, {7,5,7,1,7}
    };
    static const uint8_t h[5] = {5,5,7,5,5};
    static const uint8_t p[5] = {7,5,7,4,4};
    static const uint8_t k[5] = {5,5,6,5,5};
    static const uint8_t b[5] = {6,5,6,5,6};
    const uint8_t *rows = glyph >= '0' && glyph <= '9' ? digits[glyph - '0']
        : glyph == 'H' ? h : glyph == 'P' ? p
        : glyph == 'K' ? k : b;
    for (unsigned y = 0; y < 5u; ++y)
        for (unsigned bit = 0; bit < 3u; ++bit)
            if (rows[y] & (4u >> bit))
                pixels[(y + 1u) * LCD_PIX_W + x + bit] = 12u;
}

static void fast_number(uint8_t *pixels, unsigned x, unsigned value,
                        unsigned places) {
    unsigned divisor = places == 3u ? 100u : 10u;
    while (places--) {
        fast_glyph(pixels, x, (char)('0' + value / divisor % 10u));
        x += 4u;
        divisor /= 10u;
    }
}

void SkyOS_DoomFastFrame(fixed_t x, fixed_t y, angle_t angle,
                         int map_mode, int health, int kills,
                         int has_blue_key) {
    if (!fast_pixels) I_Error("fast framebuffer unavailable");
    const uint32_t start_ms = ll_get_time_ms();
    if (fast_last_frame_ms) {
        const uint32_t interval = start_ms - fast_last_frame_ms;
        if (interval > fast_max_interval_ms) fast_max_interval_ms = interval;
    }
    fast_last_frame_ms = start_ms;
    DoomLite_RenderFrame(fast_pixels, x >> 8, y >> 8,
                         (uint8_t)(angle >> 24), map_mode);
    memset(fast_pixels, 236, LCD_PIX_W);
    fast_glyph(fast_pixels, 2u, 'H');
    fast_glyph(fast_pixels, 6u, 'P');
    fast_number(fast_pixels, 12u,
                health < 0 ? 0u : health > 999 ? 999u : (unsigned)health, 3u);
    fast_glyph(fast_pixels, 30u, 'K');
    fast_number(fast_pixels, 36u,
                kills < 0 ? 0u : kills > 99 ? 99u : (unsigned)kills, 2u);
    fast_glyph(fast_pixels, 49u, 'B');
    if (has_blue_key) {
        for (unsigned row = 1u; row < 6u; ++row)
            memset(fast_pixels + row * LCD_PIX_W + 54u, 12, 4u);
    }
    const uint32_t rendered_ms = ll_get_time_ms();
    ll_disp_put_area(fast_pixels, 0, 0, LCD_PIX_W - 1u, LCD_PIX_H - 1u);
    display_barrier();
    const uint32_t done_ms = ll_get_time_ms();
    const uint32_t render_ms = rendered_ms - start_ms;
    const uint32_t present_ms = done_ms - rendered_ms;
    if (fast_total_frames++ == 0u)
        printf("DOOM_FAST_FIRST render_ms=%lu present_ms=%lu x=%ld y=%ld\n",
               (unsigned long)render_ms, (unsigned long)present_ms,
               (long)(x >> 16), (long)(y >> 16));
    if (fast_frames == 0u) fast_batch_start_ms = start_ms;
    fast_render_ms += render_ms;
    fast_present_ms += present_ms;
    if (render_ms > fast_max_render_ms) fast_max_render_ms = render_ms;
    if (present_ms > fast_max_present_ms) fast_max_present_ms = present_ms;
    if (++fast_frames == 32u) {
        printf("DOOM_FAST frames=32 elapsed_ms=%lu render_ms=%lu present_ms=%lu max_interval_ms=%lu max_render_ms=%lu max_present_ms=%lu hp=%d kills=%d blue=%d map=%d x=%ld y=%ld angle=%lu\n",
               (unsigned long)(done_ms - fast_batch_start_ms),
               (unsigned long)fast_render_ms,
               (unsigned long)fast_present_ms,
               (unsigned long)fast_max_interval_ms,
               (unsigned long)fast_max_render_ms,
               (unsigned long)fast_max_present_ms,
               health, kills, has_blue_key, map_mode,
               (long)(x >> 16), (long)(y >> 16),
               (unsigned long)(angle >> 24));
        fast_frames = 0u;
        fast_render_ms = fast_present_ms = fast_max_interval_ms = 0u;
        fast_max_render_ms = fast_max_present_ms = 0u;
    }
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
    if (logic_ms > phase_logic_max_ms) phase_logic_max_ms = logic_ms;
    if (draw_ms > phase_draw_max_ms) phase_draw_max_ms = draw_ms;
    if (++phase_samples == 16u) {
        printf("DOOM_PHASE frames=%u logic_ms=%lu draw_ms=%lu logic_max_ms=%lu draw_max_ms=%lu\n",
               phase_samples, (unsigned long)phase_logic_ms,
               (unsigned long)phase_draw_ms,
               (unsigned long)phase_logic_max_ms,
               (unsigned long)phase_draw_max_ms);
        phase_logic_ms = phase_draw_ms = 0;
        phase_logic_max_ms = phase_draw_max_ms = 0;
        phase_samples = 0;
    }
}

void SkyOS_DoomProfileTic(unsigned elapsed_ms) {
    tic_sum_ms += elapsed_ms;
    if (elapsed_ms > tic_max_ms) tic_max_ms = elapsed_ms;
    if (++tic_samples == 35u) {
        printf("DOOM_TIC tics=35 sum_ms=%lu max_ms=%lu\n",
               (unsigned long)tic_sum_ms, (unsigned long)tic_max_ms);
        tic_sum_ms = tic_max_ms = 0u;
        tic_samples = 0u;
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
    exit_requested = 0;
    previous_key = DOOM_NO_KEY;
    previous_code = 0;
    frames = 0;
    gray_palette_source = NULL;
    gray_palette_ready = 0;
    last_frame_ms = 0;
    perf_wall_ms = perf_convert_ms = perf_display_ms = 0;
    perf_samples = 0;
    phase_logic_ms = phase_draw_ms = 0;
    phase_logic_max_ms = phase_draw_max_ms = 0;
    phase_samples = 0;
    tic_sum_ms = tic_max_ms = 0;
    tic_samples = 0;
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
    case KEY_ENTER: return SkyOS_DoomFastMode ? 0 : KEYD_START;
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
    const uint16_t key = raw >> 16 ? (uint16_t)raw : DOOM_NO_KEY;
    if (key == KEY_F6 || key == KEY_ON) {
        // Return from the engine loop at a frame boundary. Jumping out of
        // I_StartTic skips callers that may still be updating engine state.
        exit_requested = 1;
        return;
    }
    if (key == previous_key) return;
    post_key(ev_keyup, previous_code);
    previous_key = key;
    previous_code = map_key(key);
    post_key(ev_keydown, previous_code);
}

int SkyOS_DoomExitRequested(void) { return exit_requested; }

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
    fast_pixels = ui_pixels;
    fast_last_frame_ms = 0u;
    fast_frames = 0u;
    fast_total_frames = 0u;
    fast_render_ms = fast_present_ms = fast_max_interval_ms = 0u;
    fast_max_render_ms = fast_max_present_ms = 0u;
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
    printf("DOOM_MODE flat_walls=%d fast=%d\n",
           SkyOS_DoomFlatWalls, SkyOS_DoomFastMode);
    // Remove the suspended UI from the unused LCD margins before the first
    // game frame.
    memset(ui_pixels, 0, LCD_PIX_W * LCD_PIX_H);
    ll_disp_put_area(ui_pixels, 0, 0, LCD_PIX_W - 1u, LCD_PIX_H - 1u);
    display_barrier();
    while (ll_vm_check_key() >> 16) vTaskDelay(pdMS_TO_TICKS(20));
    printf("DOOM_START allocated=%lu\n", (unsigned long)getHeapAllocateSize());

    const int outcome = setjmp(app_exit);
    if (!outcome) {
        const uint32_t boot_start_ms = ll_get_time_ms();
        I_PreInitGraphics();
        const uint32_t preinit_ms = ll_get_time_ms();
        // Keep swap enabled after exit: dirty zone pages may remain cached
        // and must be writable to the FTL when they are later evicted.
        ll_mem_swap_enable(true);
        const uint32_t swap_ms = ll_get_time_ms();
        Z_Init();
        const uint32_t zone_ms = ll_get_time_ms();
        InitGlobals();
        if (SkyOS_DoomFastMode)
            printf("DOOM_BOOT preinit_ms=%lu swap_ms=%lu zone_ms=%lu globals_ms=%lu\n",
                   (unsigned long)(preinit_ms - boot_start_ms),
                   (unsigned long)(swap_ms - preinit_ms),
                   (unsigned long)(zone_ms - swap_ms),
                   (unsigned long)(ll_get_time_ms() - zone_ms));
        D_DoomMain();
    }
    printf("DOOM_EXIT outcome=%d frames=%u allocated=%lu critical=%lu\n",
           outcome, frames, (unsigned long)getHeapAllocateSize(),
           (unsigned long)ulCriticalNesting);
    mainzone = NULL; // The SkyOS Doom zone is a fixed VM RAM region, not malloc storage.
    _g = NULL;
    display_barrier();
done:;
    fast_pixels = NULL;
    // Do not deliver the same F6 press to the resumed UI. A held F6
    // switches the UI to Settings while Doom is still handing off.
    unsigned release_wait = 0;
    while ((ll_vm_check_key() >> 16) && release_wait++ < 50u)
        vTaskDelay(pdMS_TO_TICKS(20));
    printf("DOOM_CLEANUP key_released=%u critical=%lu resuming_ui\n",
           (ll_vm_check_key() >> 16) == 0u,
           (unsigned long)ulCriticalNesting);
    SystemUIResume();
    printf("DOOM_CLEANUP ui_resumed critical=%lu\n",
           (unsigned long)ulCriticalNesting);
    // Early free (about one second after UI resume) froze the system in two
    // hardware runs; one of them kept this task alive, so task deletion alone
    // cannot explain it. One staged 12/24/36-second run stayed responsive.
    // Keep that tested ordering until the UI redraw and allocator interaction
    // is verified; the delay itself is not a proof of buffer lifetime.
    for (unsigned seconds = 1; seconds <= 36u; ++seconds) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (seconds == 12u) {
            if (lcd_pixels) free(lcd_pixels);
            lcd_pixels = NULL;
            printf("DOOM_CLEANUP lcd_freed allocated=%lu\n",
                   (unsigned long)getHeapAllocateSize());
        }
        if (seconds == 24u) {
            if (game_pixels) check_game_buffer();
            if (game_buffer_allocation) free(game_buffer_allocation);
            game_buffer_allocation = NULL;
            game_pixels = NULL;
            printf("DOOM_CLEANUP game_freed allocated=%lu\n",
                   (unsigned long)getHeapAllocateSize());
        }
        printf("DOOM_CLEANUP_WAIT seconds=%u key=%08lx critical=%lu\n",
               seconds, (unsigned long)ll_vm_check_key(),
               (unsigned long)ulCriticalNesting);
    }
    app_running = 0;
    SkyOS_DoomFastRunning = 0;
    printf("DOOM_CLEANUP task_delete\n");
    vTaskDelete(NULL);
}

static void doom_start(int mode) {
    if (app_running) return;
    app_running = 1;
    SkyOS_DoomFlatWalls = mode == 1;
    SkyOS_DoomFastMode = mode == 2;
    SkyOS_DoomFastRunning = SkyOS_DoomFastMode;
    if (xTaskCreate(doom_task, "Doom", 4096, NULL,
                    configMAX_PRIORITIES - 3, NULL) != pdPASS) {
        app_running = 0;
        SkyOS_DoomFastRunning = 0;
        printf("DOOM_ERROR task allocation\n");
    }
}

void DoomPort_Start(void) { doom_start(0); }
void DoomPort_StartFlat(void) { doom_start(1); }
void DoomPort_StartFast(void) { doom_start(2); }
