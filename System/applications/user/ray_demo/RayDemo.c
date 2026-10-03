/* Small native ray demo. Geometry and controller also run on the host. */
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "SystemUI.h"
#include "keyboard_gii39.h"
#include "sys_llapi.h"
#include "RaySession.h"
#include "RayPost.h"

volatile int SkyOS_RayRunning;
static RaySession session;
static RayPostState post;
static uint8_t barrier_pixel;
typedef struct {
    uint32_t began_ms, trace_us, preview_us, lcd_us, batch_max_us;
    unsigned batch_samples;
} RayPerf;

static void present(uint8_t *pixels) {
    barrier_pixel = pixels[0];
    ll_disp_put_area(pixels, 0, 0, RAY_WIDTH - 1u, RAY_HEIGHT - 1u);
    /* The four-entry display queue borrows the buffer. Five stable single
     * pixel submissions make the full-frame DMA finish before we reuse it. */
    for (unsigned i = 0; i < 5u; ++i)
        ll_disp_put_area(&barrier_pixel, 0, 0, 0, 0);
}

static unsigned buttons_for(uint16_t key) {
    switch (key) {
        case KEY_UP: return RAY_UP;
        case KEY_DOWN: return RAY_DOWN;
        case KEY_LEFT: return RAY_LEFT;
        case KEY_RIGHT: return RAY_RIGHT;
        case KEY_F1: return RAY_RESET;
        case KEY_F2: return RAY_AA_TOGGLE;
        case KEY_F3: return RAY_CONTRAST;
        case KEY_2: return RAY_PITCH_DOWN;
        case KEY_8: return RAY_PITCH_UP;
        case KEY_4: return RAY_ROLL_LEFT;
        case KEY_6: return RAY_ROLL_RIGHT;
        case KEY_7: return RAY_DESCEND;
        case KEY_9: return RAY_ASCEND;
        default: return 0u;
    }
}

/* A preview-only badge is overwritten by the complete traced image. No HUD
 * enters the AA source image, and no saved framebuffer or font heap is needed. */
static void preview_badge(uint8_t *pixels) {
    static const uint8_t glyphs[5][5] = {
        {2, 5, 7, 5, 5}, /* A */
        {7, 5, 5, 5, 7}, /* O */
        {5, 7, 7, 7, 5}, /* N */
        {7, 4, 6, 4, 4}, /* F */
        {0, 0, 0, 0, 0}, /* space */
    };
    const unsigned letters[6] = {0u, 0u, 4u, 1u,
                               session.aa_enabled ? 2u : 3u,
                               session.aa_enabled ? 4u : 3u};
    for (unsigned y = 2u; y < 16u; ++y)
        for (unsigned x = 2u; x < 52u; ++x)
            pixels[y * RAY_WIDTH + x] = 240u;
    for (unsigned c = 0; c < 6u; ++c)
        for (unsigned row = 0; row < 5u; ++row)
            for (unsigned col = 0; col < 3u; ++col)
                if (glyphs[letters[c]][row] & (4u >> col))
                    for (unsigned dy = 0; dy < 2u; ++dy)
                        for (unsigned dx = 0; dx < 2u; ++dx)
                            pixels[(4u + row * 2u + dy) * RAY_WIDTH +
                                   4u + c * 8u + col * 2u + dx] = 0u;
}

static void log_event(const char *event) {
    printf("RAY_EVENT event=%s generation=%lu phase=%u samples=%lu pass=%u contrast=%u "
           "x_q8=%ld y_q8=%ld z_q8=%ld cancellations=%lu ms=%lu idle_base_ms=%lu aa_enabled=%u "
           "yaw_mrad=%ld pitch_mrad=%ld roll_mrad=%ld\n", event,
           (unsigned long)session.generation, session.phase,
           (unsigned long)session.traced_samples, session.pass,
           session.contrast + 1u, (long)(session.camera.position.x * 256.0f),
           (long)(session.camera.position.y * 256.0f),
           (long)(session.camera.position.z * 256.0f), (unsigned long)session.cancellations,
           (unsigned long)ll_get_time_ms(), (unsigned long)session.last_motion_ms, session.aa_enabled,
           (long)(session.camera.yaw * 1000.0f),
           (long)(session.camera.pitch * 1000.0f),
           (long)(session.camera.roll * 1000.0f));
}

static void log_perf(const RayPerf *p) {
    const RayStats *r = &session.stats;
    printf("RAY_PERF generation=%lu phase=%u pass=%u samples=%lu batch_samples=%u elapsed_ms=%lu "
           "trace_us=%lu preview_us=%lu lcd_us=%lu batch_max_us=%lu primary=%lu reflection=%lu shadow=%lu "
           "sphere_tests=%lu plane_tests=%lu camera_us=%lu intersect_us=%lu shadow_us=%lu shade_us=%lu "
           "reflection_us=%lu refraction=%lu refraction_us=%lu glass_exits=%lu tir_events=%lu floor_reflection=%lu "
           "stack_words=-1 preview_triangles=%lu preview_pixels=%lu hits=%lu "
           "aa_enabled=%u aa_us=%lu aa_rows=%u aa_pixels=%lu aa_changed=%lu aa_batch_max_us=%lu aa_edges=%lu aa_search_steps=%lu "
           "x_q8=%ld y_q8=%ld z_q8=%ld yaw_mrad=%ld pitch_mrad=%ld roll_mrad=%ld\n",
           (unsigned long)session.generation, session.phase, session.pass,
           (unsigned long)session.traced_samples, p->batch_samples,
           (unsigned long)(ll_get_time_ms() - p->began_ms),
           (unsigned long)p->trace_us, (unsigned long)p->preview_us,
           (unsigned long)p->lcd_us, (unsigned long)p->batch_max_us,
           (unsigned long)r->primary_rays, (unsigned long)r->reflection_rays,
           (unsigned long)r->shadow_rays, (unsigned long)r->sphere_tests,
           (unsigned long)r->plane_tests, (unsigned long)r->camera_us,
           (unsigned long)r->intersect_us, (unsigned long)r->shadow_us,
           (unsigned long)r->shade_us, (unsigned long)r->reflection_us,
           (unsigned long)r->refraction_rays, (unsigned long)r->refraction_us,
           (unsigned long)r->glass_exits, (unsigned long)r->tir_events,
           (unsigned long)r->floor_reflection_rays,
           (unsigned long)r->preview_triangles, (unsigned long)r->preview_pixels,
           (unsigned long)r->hits, session.aa_enabled, (unsigned long)post.aa_us,
           (unsigned)post.row, (unsigned long)post.pixels_processed, (unsigned long)post.pixels_changed,
           (unsigned long)post.batch_max_us,
           (unsigned long)post.edge_pixels, (unsigned long)post.search_steps,
           (long)(session.camera.position.x * 256.0f),
           (long)(session.camera.position.y * 256.0f),
           (long)(session.camera.position.z * 256.0f),
           (long)(session.camera.yaw * 1000.0f),
           (long)(session.camera.pitch * 1000.0f),
           (long)(session.camera.roll * 1000.0f));
}

static void log_memory(void) {
    extern uint32_t TotalAllocatableSize;
    extern uint32_t getHeapAllocateSize(void);
    extern size_t getOnChipHeapAllocated(void), getSwapMemHeapAllocated(void);
    uint32_t free_bytes, total_bytes;
    ll_mem_phy_info(&free_bytes, &total_bytes);
    printf("RAY_MEM a=%lu/%lu z=%lu/%lu s=%lu w=%lu\n",
           (unsigned long)getHeapAllocateSize(), (unsigned long)TotalAllocatableSize,
           (unsigned long)(total_bytes - free_bytes), (unsigned long)total_bytes,
           (unsigned long)getOnChipHeapAllocated(), (unsigned long)getSwapMemHeapAllocated());
}

static void ray_task(void *unused) {
    (void)unused;
    printf("RAY_BOOT phase=task_start width=%u height=%u pixels=%u session_bytes=%u "
           "framebuffer_bytes=%u renderer_heap_bytes=0 task_stack_bytes=8192 idle_ms=1000 batch_budget_us=6000 "
           "cpu_mhz=%d detail_timing=1 reflection_timing_nested=1 refraction_timing_nested=1 scene=studio-v4 "
           "aa_default=0 aa_method=fxaa-gray-bounded aa_scratch_bytes=%u post_state_bytes=%u\n",
           RAY_WIDTH, RAY_HEIGHT, RAY_PIXELS, (unsigned)sizeof(session), RAY_PIXELS,
           ll_get_cur_freq(), (unsigned)RAY_POST_CACHE_BYTES, (unsigned)sizeof(post));
    uint8_t *pixels = SystemUIBorrowFrameBuffer();
    if (!pixels) {
        printf("RAY_BOOT phase=framebuffer_error\n");
        SystemUIResume();
        SkyOS_RayRunning = 0;
        vTaskDelete(NULL);
        return;
    }
    /* Also drain any UI draw queued before its task was suspended. */
    const uint32_t drain_began = ll_get_time_us();
    present(pixels);
    printf("RAY_BOOT phase=framebuffer_ready display_drain_us=%lu\n",
           (unsigned long)(ll_get_time_us() - drain_began));
    Ray_SetClock(ll_get_time_us);
    RaySession_Init(&session, ll_get_time_ms());
    memset(&post, 0, sizeof(post));
    RayPerf perf = {0};
    perf.began_ms = ll_get_time_ms();
    uint32_t last_log_ms = perf.began_ms, last_mem_ms = perf.began_ms;
    uint32_t last_preview_ms = perf.began_ms - 60u, last_present_ms = perf.began_ms;
    unsigned previous_buttons = 0u, previous_phase = session.phase;
    log_event("ready");
    log_memory();

    for (;;) {
        const uint32_t now = ll_get_time_ms();
        const uint32_t key_state = ll_vm_check_key();
        const uint16_t key = key_state >> 16 ? (uint16_t)key_state : UINT16_MAX;
        if (key == KEY_F6 || key == KEY_ON) break;
        const unsigned buttons = buttons_for(key);
        const unsigned pressed = buttons & ~previous_buttons;
        /* Preserve an interrupted generation's counters before invalidating it. */
        if ((session.phase == RAY_TRACE || session.phase == RAY_POSTAA) &&
            ((buttons & RAY_MOTION_MASK) ||
             (pressed & (RAY_RESET | RAY_CONTRAST | RAY_AA_TOGGLE)))) log_perf(&perf);
        const unsigned events = RaySession_Update(&session, now, buttons);
        if (events & RAY_CHANGED) {
            if (events & RAY_CANCELLED) log_event("cancel");
            memset(&session.stats, 0, sizeof(session.stats));
            memset(&perf, 0, sizeof(perf));
            memset(&post, 0, sizeof(post));
            perf.began_ms = now;
        }
        if (pressed & RAY_RESET) log_event("reset");
        if (pressed & RAY_CONTRAST) log_event("contrast");
        if (pressed & RAY_AA_TOGGLE) log_event("aa_toggle");
        if (events & RAY_STARTED) {
            memset(&perf, 0, sizeof(perf));
            perf.began_ms = now;
            log_event("start");
        }
        if (session.phase != previous_phase && session.phase == RAY_WAIT)
            log_event("wait");
        previous_buttons = buttons;
        previous_phase = session.phase;

        int needs_present = 0, just_completed = 0;
        if (session.preview_dirty && (uint32_t)(now - last_preview_ms) >= 60u) {
            const uint32_t began = ll_get_time_us();
            RayPreview_Render(pixels, &session.camera, session.contrast, &session.stats);
            preview_badge(pixels);
            perf.preview_us += ll_get_time_us() - began;
            session.preview_dirty = 0u;
            last_preview_ms = now;
            needs_present = 1;
        }
        if (session.phase == RAY_TRACE) {
            const unsigned pass = session.pass;
            const uint32_t began = ll_get_time_us();
            const unsigned count = RaySession_TraceBatch(&session, pixels, 512u, 6000u);
            const uint32_t elapsed = ll_get_time_us() - began;
            perf.trace_us += elapsed;
            if (elapsed > perf.batch_max_us) perf.batch_max_us = elapsed;
            perf.batch_samples = count;
            if ((uint32_t)(now - last_present_ms) >= 100u || session.pass != pass || session.phase == RAY_DONE)
                needs_present = 1;
            if (session.phase == RAY_POSTAA) {
                RayPost_Init(&post, pixels);
                log_event("aa_start");
                needs_present = 1;
            } else if (session.phase == RAY_DONE) {
                just_completed = 1;
            }
        }
        if (session.phase == RAY_POSTAA) {
            /* Integer post processing casts no rays. Poll inputs and yield
             * between at most eight rows; the borrowed frame is fully drained. */
            RayPost_Batch(&post, pixels, 8u, 6000u);
            if ((uint32_t)(now - last_present_ms) >= 100u || post.done)
                needs_present = 1;
            if (post.done) {
                session.phase = RAY_DONE;
                just_completed = 1;
                log_event("aa_done");
            }
        }
        if (needs_present) {
            const uint32_t began = ll_get_time_us();
            present(pixels);
            perf.lcd_us += ll_get_time_us() - began;
            last_present_ms = ll_get_time_ms();
        }
        if (just_completed) {
            log_event("done");
            log_perf(&perf);
        }
        if ((uint32_t)(now - last_log_ms) >= 1000u) {
            log_perf(&perf);
            last_log_ms = now;
        }
        if ((uint32_t)(now - last_mem_ms) >= 5000u) {
            log_memory();
            last_mem_ms = now;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    log_perf(&perf);
    printf("RAY_EXIT phase=key generation=%lu samples=%lu ms=%lu\n",
           (unsigned long)session.generation, (unsigned long)session.traced_samples,
           (unsigned long)ll_get_time_ms());
    const uint32_t release_began = ll_get_time_ms();
    for (unsigned i = 0; (ll_vm_check_key() >> 16) && i < 50u; ++i)
        vTaskDelay(pdMS_TO_TICKS(20));
    /* No dynamic buffers to free. Finish borrowed-frame writes before UI reuse. */
    present(pixels);
    printf("RAY_EXIT phase=key_release wait_ms=%lu\n",
           (unsigned long)(ll_get_time_ms() - release_began));
    SystemUIResume();
    printf("RAY_EXIT phase=ui_resume_done ms=%lu\n", (unsigned long)ll_get_time_ms());
    Ray_SetClock(NULL);
    SkyOS_RayRunning = 0;
    printf("RAY_EXIT phase=task_delete ms=%lu\n", (unsigned long)ll_get_time_ms());
    vTaskDelete(NULL);
}

void RayDemo_Start(void) {
    if (SkyOS_RayRunning) return;
    SkyOS_RayRunning = 1;
    if (xTaskCreate(ray_task, "RayDemo", 2048u, NULL, configMAX_PRIORITIES - 3, NULL) != pdPASS) {
        SkyOS_RayRunning = 0;
        printf("RAY_BOOT phase=task_allocation_error\n");
    }
}
