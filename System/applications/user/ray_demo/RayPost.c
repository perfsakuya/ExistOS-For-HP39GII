#include "RayPost.h"

#include <stddef.h>
#include <string.h>

/* Design references (concepts only, no imported implementation):
 * NVIDIA FXAA whitepaper: local luminance contrast rejection and capped blend.
 * https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/FXAA_WhitePaper.pdf
 * Intel CMAA2: classify edge candidates while limiting change to sharp detail.
 * https://www.intel.com/content/www/us/en/developer/articles/technical/conservative-morphological-anti-aliasing-20.html
 *
 * This grayscale, integer-only approximation intentionally preserves horizontal
 * and vertical steps/lines. A 3x3 neighborhood can soften a right-angle corner,
 * but it never spreads filtering along a straight axis-aligned edge. */

_Static_assert(RAY_WIDTH == 256u && RAY_HEIGHT == 127u,
               "RayPost requires the fixed ray framebuffer dimensions");
_Static_assert(sizeof(((RayPostState *)0)->rows) == 768u,
               "RayPost original-row cache must be exactly 768 bytes");

static unsigned post_min(unsigned a, unsigned b)
{
    return a < b ? a : b;
}

static unsigned post_max(unsigned a, unsigned b)
{
    return a > b ? a : b;
}

static unsigned post_magnitude(int value)
{
    return (unsigned)(value < 0 ? -value : value);
}

static uint32_t post_add_time(uint32_t total, uint32_t elapsed)
{
    return elapsed > UINT32_MAX - total ? UINT32_MAX : total + elapsed;
}

static uint8_t post_pixel(const uint8_t *above, const uint8_t *current,
                          const uint8_t *below, unsigned x)
{
    unsigned c = current[x], n = above[x], s = below[x];
    unsigned w = current[x - 1u], e = current[x + 1u];
    unsigned nw = above[x - 1u], ne = above[x + 1u];
    unsigned sw = below[x - 1u], se = below[x + 1u];
    unsigned low = post_min(c, post_min(n, s));
    unsigned high = post_max(c, post_max(n, s));
    unsigned range, threshold, gx, gy, small_gradient, large_gradient;
    int gradient_x, gradient_y;

    low = post_min(low, post_min(w, e));
    high = post_max(high, post_max(w, e));
    low = post_min(low, post_min(post_min(nw, ne), post_min(sw, se)));
    high = post_max(high, post_max(post_max(nw, ne), post_max(sw, se)));
    range = high - low;
    threshold = post_max(12u, (high + 7u) >> 3u);
    if (range < threshold) return (uint8_t)c;

    /* Sobel values are bounded by +/-1020, so all signed arithmetic is safe.
     * Both axes must carry a meaningful edge, rejecting straight brick lines.
     * The ratio cap also avoids classifying a nearly axial edge as diagonal. */
    gradient_x = (int)ne + 2 * (int)e + (int)se
               - (int)nw - 2 * (int)w - (int)sw;
    gradient_y = (int)sw + 2 * (int)s + (int)se
               - (int)nw - 2 * (int)n - (int)ne;
    gx = post_magnitude(gradient_x);
    gy = post_magnitude(gradient_y);
    small_gradient = post_min(gx, gy);
    large_gradient = post_max(gx, gy);
    if (small_gradient < range || small_gradient * 4u < large_gradient)
        return (uint8_t)c;

    /* Preserve 75% of the center and mix only 25% of its four-neighbor mean.
     * Flat ramps are preserved by the symmetric kernel; no floats or division. */
    return (uint8_t)((12u * c + n + s + w + e + 8u) >> 4u);
}

void RayPost_Init(RayPostState *state, uint8_t *pixels)
{
    uint32_t start;
    if (state == NULL) return;
    start = Ray_Clock();
    state->pixels_processed = 0u;
    state->pixels_changed = 0u;
    state->aa_us = 0u;
    state->batch_max_us = 0u;
    state->row = pixels != NULL ? 0u : (uint16_t)RAY_HEIGHT;
    state->done = pixels == NULL;
    state->active = pixels != NULL;
    state->framebuffer = pixels;
    if (pixels != NULL)
        memcpy(state->rows, pixels, RAY_POST_CACHE_BYTES);
    state->aa_us = Ray_Clock() - start;
}

unsigned RayPost_Batch(RayPostState *state, uint8_t *pixels,
                       unsigned max_rows, uint32_t budget_us)
{
    uint32_t start, elapsed = 0u;
    unsigned completed = 0u;

    if (state == NULL || pixels == NULL || max_rows == 0u
        || state->done || !state->active || pixels != state->framebuffer
        || state->row >= RAY_HEIGHT) return 0u;
    start = Ray_Clock();
    while (completed < max_rows && state->row < RAY_HEIGHT) {
        unsigned row = state->row;
        if (row > 0u && row + 1u < RAY_HEIGHT) {
            const uint8_t *above = state->rows[(row - 1u) % 3u];
            const uint8_t *current = state->rows[row % 3u];
            const uint8_t *below = state->rows[(row + 1u) % 3u];
            uint8_t *output = pixels + row * RAY_WIDTH;
            unsigned x;
            for (x = 1u; x + 1u < RAY_WIDTH; ++x) {
                uint8_t filtered = post_pixel(above, current, below, x);
                if (filtered != current[x]) ++state->pixels_changed;
                output[x] = filtered;
            }
        }
        state->pixels_processed += RAY_WIDTH;
        state->row = (uint16_t)(row + 1u);
        ++completed;

        /* Capture the next untouched source row before replacing this slot.
         * No later batch reads an output row as original input. */
        if (state->row >= 2u && (unsigned)state->row + 1u < RAY_HEIGHT) {
            unsigned next = (unsigned)state->row + 1u;
            memcpy(state->rows[next % 3u], pixels + next * RAY_WIDTH, RAY_WIDTH);
        }
        if (state->row == RAY_HEIGHT) {
            state->done = 1u;
            state->active = 0u;
        }
        elapsed = Ray_Clock() - start;
        if (budget_us != 0u && elapsed >= budget_us) break;
    }
    state->aa_us = post_add_time(state->aa_us, elapsed);
    if (elapsed > state->batch_max_us) state->batch_max_us = elapsed;
    return completed;
}
