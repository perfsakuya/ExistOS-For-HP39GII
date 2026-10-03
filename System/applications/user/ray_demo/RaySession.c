#include "RaySession.h"
#include <string.h>

static void invalidate(RaySession *s, uint32_t now, unsigned *events) {
    if (s->phase == RAY_TRACE) {
        ++s->cancellations;
        *events |= RAY_CANCELLED;
    }
    ++s->generation;
    s->phase = RAY_WAIT;
    s->last_motion_ms = now;
    s->traced_samples = 0u;
    s->pass = s->cursor = 0u;
    s->preview_dirty = 1u;
    *events |= RAY_CHANGED;
}

void RaySession_Init(RaySession *s, uint32_t now_ms) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    RayCamera_Init(&s->camera);
    s->generation = 1u;
    s->contrast = 1u;
    s->phase = RAY_WAIT;
    s->last_motion_ms = s->last_step_ms = now_ms;
    s->preview_dirty = 1u;
}

unsigned RaySession_Update(RaySession *s, uint32_t now, unsigned buttons) {
    if (!s) return 0u;
    unsigned events = 0u;
    const unsigned was_moving = s->previous_buttons & (RAY_UP | RAY_DOWN | RAY_LEFT | RAY_RIGHT);
    const unsigned presses = buttons & ~s->previous_buttons;
    s->previous_buttons = buttons;
    if (presses & RAY_RESET) {
        RayCamera_Init(&s->camera);
        invalidate(s, now, &events);
    }
    if (presses & RAY_CONTRAST) {
        s->contrast = (s->contrast + 1u) % 3u;
        invalidate(s, now, &events);
    }
    if (buttons & (RAY_UP | RAY_DOWN | RAY_LEFT | RAY_RIGHT)) {
        /* A held key postpones tracing even when movement is blocked. */
        if (s->phase == RAY_TRACE || s->phase == RAY_DONE)
            invalidate(s, now, &events);
        s->phase = RAY_PREVIEW;
        s->last_motion_ms = now;
        uint32_t dt = now - s->last_step_ms;
        if (dt >= 40u) {
            if (dt > 100u) dt = 100u;
            const float distance = ((buttons & RAY_UP ? 1.0f : 0.0f) -
                                    (buttons & RAY_DOWN ? 1.0f : 0.0f)) * (float)dt * 0.002f;
            const float turn = ((buttons & RAY_RIGHT ? 1.0f : 0.0f) -
                                (buttons & RAY_LEFT ? 1.0f : 0.0f)) * (float)dt * 0.001f;
            if (RayCamera_Move(&s->camera, distance, turn)) {
                invalidate(s, now, &events);
                s->phase = RAY_PREVIEW;
            }
            s->last_step_ms = now;
        }
    } else {
        s->last_step_ms = now;
        /* Start the quiet period at the observed key release, never earlier. */
        if (was_moving) s->last_motion_ms = now;
        if (s->phase == RAY_PREVIEW) s->phase = RAY_WAIT;
        if (s->phase == RAY_WAIT && (uint32_t)(now - s->last_motion_ms) >= 1000u) {
            s->phase = RAY_TRACE;
            s->trace_generation = s->generation;
            s->pass = s->cursor = s->traced_samples = 0u;
            memset(&s->stats, 0, sizeof(s->stats));
            events |= RAY_STARTED;
        }
    }
    return events;
}

unsigned RaySession_TraceBatch(RaySession *s, uint8_t *pixels,
                              unsigned max_samples, uint32_t budget_us) {
    if (!s || !pixels || !max_samples || s->phase != RAY_TRACE ||
        s->generation != s->trace_generation) return 0u;
    const uint32_t began = Ray_Clock();
    unsigned completed = 0u;
    while (completed < max_samples && s->pass < 3u) {
        const unsigned step = 4u >> s->pass;
        const unsigned width = (RAY_WIDTH + step - 1u) / step;
        const unsigned height = (RAY_HEIGHT + step - 1u) / step;
        if (s->cursor >= width * height) {
            ++s->pass;
            s->cursor = 0u;
            continue;
        }
        const unsigned x = (s->cursor % width) * step;
        const unsigned y = (s->cursor / width) * step;
        ++s->cursor;
        if (s->pass && !(x % (step * 2u)) && !(y % (step * 2u)))
            continue;
        const uint8_t gray = Ray_TracePixel(&s->camera, x, y, s->contrast, &s->stats);
        for (unsigned yy = y; yy < y + step && yy < RAY_HEIGHT; ++yy)
            for (unsigned xx = x; xx < x + step && xx < RAY_WIDTH; ++xx)
                pixels[yy * RAY_WIDTH + xx] = gray;
        ++completed;
        ++s->traced_samples;
        /* Clock checks stay outside the geometry/pixel fill loops. */
        if (budget_us && (completed & 7u) == 0u &&
            (uint32_t)(Ray_Clock() - began) >= budget_us) break;
    }
    if (s->pass == 3u) s->phase = RAY_DONE;
    return completed;
}
