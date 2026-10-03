#include "RayPost.h"

#include <stddef.h>
#include <string.h>

/* Bounded grayscale CPU FXAA adaptation, based on Timothy Lottes' NVIDIA
 * FXAA whitepaper: local contrast, weighted second-order edge direction,
 * strongest-gradient pair, two-ended edge search, perpendicular subpixel
 * resampling, and a trimmed/capped subpixel lowpass blend.
 * https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/FXAA_WhitePaper.pdf
 *
 * This is independently written CPU code, not the GPU shader implementation.
 * Search radius is four pixels with no acceleration; all sampling uses original
 * cached rows. Q8 coordinates/luminance avoid floats. Subpixel trim is 1/4 with
 * cap 1/4; thin features get smaller caps, and coherent axis edges stay exact. */

_Static_assert(RAY_WIDTH == 256u && RAY_HEIGHT == 127u,
               "RayPost requires the fixed ray framebuffer dimensions");
_Static_assert(RAY_POST_SEARCH_RADIUS == 4u && RAY_POST_ROW_RADIUS == 5u,
               "RayPost search and sampling bounds must stay fixed");
_Static_assert(sizeof(((RayPostState *)0)->rows) == 2816u,
               "RayPost original-row cache must be exactly 2816 bytes");

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

/* Q8 bilinear sampling with clamped coordinates. At integer y the generic
 * sampler still loads y+1, so a search at output y+4 requires cached y+5. */
static unsigned post_sample_q8(const RayPostState *state, int x_q8, int y_q8)
{
    unsigned x0, x1, y0, y1, fx, fy, upper, lower;
    const uint8_t *row0, *row1;
    if (x_q8 < 0) x_q8 = 0;
    if (y_q8 < 0) y_q8 = 0;
    if (x_q8 > (int)((RAY_WIDTH - 1u) * 256u))
        x_q8 = (int)((RAY_WIDTH - 1u) * 256u);
    if (y_q8 > (int)((RAY_HEIGHT - 1u) * 256u))
        y_q8 = (int)((RAY_HEIGHT - 1u) * 256u);
    x0 = (unsigned)x_q8 >> 8u;
    y0 = (unsigned)y_q8 >> 8u;
    x1 = post_min(x0 + 1u, RAY_WIDTH - 1u);
    y1 = post_min(y0 + 1u, RAY_HEIGHT - 1u);
    fx = (unsigned)x_q8 & 255u;
    fy = (unsigned)y_q8 & 255u;
    row0 = state->rows[y0 % RAY_POST_CACHE_ROWS];
    row1 = state->rows[y1 % RAY_POST_CACHE_ROWS];
    upper = (256u - fx) * row0[x0] + fx * row0[x1];
    lower = (256u - fx) * row1[x0] + fx * row1[x1];
    /* Largest numerator is 255*65536+128, safely inside uint32_t. */
    return ((256u - fy) * upper + fy * lower + 128u) >> 8u;
}

static uint8_t post_pixel(RayPostState *state, unsigned x, unsigned y)
{
    const uint8_t *above = state->rows[(y - 1u) % RAY_POST_CACHE_ROWS];
    const uint8_t *current = state->rows[y % RAY_POST_CACHE_ROWS];
    const uint8_t *below = state->rows[(y + 1u) % RAY_POST_CACHE_ROWS];
    int c = current[x], n = above[x], s = below[x];
    int w = current[x - 1u], e = current[x + 1u];
    int nw, ne, sw, se, negative_delta, positive_delta, direction;
    int horizontal, thin, base_x, base_y, tangent_x, tangent_y;
    int normal_x, normal_y, pair_luma_q8, center_delta_q8;
    int end_n_delta = 0, end_p_delta = 0, found_n = 0, found_p = 0;
    unsigned low = post_min((unsigned)c, post_min((unsigned)n, (unsigned)s));
    unsigned high = post_max((unsigned)c, post_max((unsigned)n, (unsigned)s));
    unsigned range, threshold, edge_horizontal, edge_vertical, gradient;
    unsigned distance_n = RAY_POST_SEARCH_RADIUS;
    unsigned distance_p = RAY_POST_SEARCH_RADIUS;
    unsigned step, offset_q8 = 0u, blend_q8, ratio_q8;
    unsigned sampled_q8, lowpass_q8, near_distance, span_length;

    /* FXAA's local contrast uses center and four axial neighbors. */
    low = post_min(low, post_min((unsigned)w, (unsigned)e));
    high = post_max(high, post_max((unsigned)w, (unsigned)e));
    range = high - low;
    threshold = post_max(12u, (high + 7u) >> 3u);
    if (range < threshold) return (uint8_t)c;

    nw = above[x - 1u];
    ne = above[x + 1u];
    sw = below[x - 1u];
    se = below[x + 1u];
    /* Preserve already coherent axis-aligned steps and single-pixel lines.
     * Their center/adjacent traces do not vary along the edge at all. */
    if ((n == c && s == c && nw == w && sw == w && ne == e && se == e)
        || (w == c && e == c && nw == n && ne == n && sw == s && se == s))
        return (uint8_t)c;

    /* Whitepaper high-pass direction tests, scaled by four: second differences
     * on three rows/columns, with the middle trace weighted twice. This also
     * detects a one-pixel line that a first-derivative/Sobel-only test misses. */
    edge_vertical = post_magnitude(nw - 2 * n + ne)
                  + 2u * post_magnitude(w - 2 * c + e)
                  + post_magnitude(sw - 2 * s + se);
    edge_horizontal = post_magnitude(nw - 2 * w + sw)
                    + 2u * post_magnitude(n - 2 * c + s)
                    + post_magnitude(ne - 2 * e + se);
    if (edge_horizontal == 0u && edge_vertical == 0u) return (uint8_t)c;
    horizontal = edge_horizontal >= edge_vertical;
    negative_delta = (horizontal ? n : w) - c;
    positive_delta = (horizontal ? s : e) - c;
    gradient = post_max(post_magnitude(negative_delta), post_magnitude(positive_delta));
    if (gradient == 0u) return (uint8_t)c;
    direction = post_magnitude(negative_delta) >= post_magnitude(positive_delta) ? -1 : 1;
    normal_x = horizontal ? 0 : direction;
    normal_y = horizontal ? direction : 0;
    tangent_x = horizontal ? 256 : 0;
    tangent_y = horizontal ? 0 : 256;
    base_x = (int)x * 256 + normal_x * 128;
    base_y = (int)y * 256 + normal_y * 128;
    pair_luma_q8 = (c + (direction < 0 ? (horizontal ? n : w)
                                      : (horizontal ? s : e))) * 128;
    center_delta_q8 = c * 256 - pair_luma_q8;
    ++state->edge_pixels;

    /* Search the half-pixel strongest pair in both tangent directions. Endpoint
     * contrast threshold is one quarter of the selected gradient. At most four
     * samples per direction are taken; unresolved sides stay at distance four. */
    for (step = 1u; step <= RAY_POST_SEARCH_RADIUS; ++step) {
        if (!found_n) {
            end_n_delta = (int)post_sample_q8(state,
                base_x - (int)step * tangent_x, base_y - (int)step * tangent_y)
                - pair_luma_q8;
            distance_n = step;
            found_n = post_magnitude(end_n_delta) >= gradient * 64u;
            ++state->search_steps;
        }
        if (!found_p) {
            end_p_delta = (int)post_sample_q8(state,
                base_x + (int)step * tangent_x, base_y + (int)step * tangent_y)
                - pair_luma_q8;
            distance_p = step;
            found_p = post_magnitude(end_p_delta) >= gradient * 64u;
            ++state->search_steps;
        }
        if (found_n && found_p) break;
    }
    near_distance = post_min(distance_n, distance_p);
    span_length = distance_n + distance_p;
    /* A same-sign endpoint would shift the pixel in the wrong contrast sense.
     * Require an actual endpoint, not just exhaustion of the radius cap. */
    if (distance_n < distance_p) {
        if (found_n && ((end_n_delta < 0) != (center_delta_q8 < 0)))
            offset_q8 = 128u - (near_distance * 256u) / span_length;
    } else {
        if (found_p && ((end_p_delta < 0) != (center_delta_q8 < 0)))
            offset_q8 = 128u - (near_distance * 256u) / span_length;
    }

    /* Whitepaper subpixel ratio: axial mean vs center, normalized by contrast.
     * Trim 1/4, scale by 4/3, then cap at 1/4 to retain small grayscale detail. */
    ratio_q8 = (post_magnitude(n + s + w + e - 4 * c) * 64u) / range;
    blend_q8 = ratio_q8 > 64u ? post_min(64u, ((ratio_q8 - 64u) * 4u) / 3u) : 0u;
    thin = ((((n - c) < 0 && (s - c) < 0) || ((n - c) > 0 && (s - c) > 0))
             && post_min(post_magnitude(n - c), post_magnitude(s - c)) * 2u >= range)
        || ((((w - c) < 0 && (e - c) < 0) || ((w - c) > 0 && (e - c) > 0))
             && post_min(post_magnitude(w - c), post_magnitude(e - c)) * 2u >= range);
    if (thin) {
        offset_q8 = post_min(offset_q8, 32u);
        blend_q8 = post_min(blend_q8, 16u);
    }
    if (offset_q8 == 0u && blend_q8 == 0u) return (uint8_t)c;

    /* Resample perpendicular to the edge from the original pixel center. */
    sampled_q8 = post_sample_q8(state, (int)x * 256 + normal_x * (int)offset_q8,
                                      (int)y * 256 + normal_y * (int)offset_q8);
    lowpass_q8 = ((unsigned)(c + n + s + w + e + nw + ne + sw + se) * 256u + 4u) / 9u;
    return (uint8_t)(((256u - blend_q8) * sampled_q8
                     + blend_q8 * lowpass_q8 + 32768u) >> 16u);
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
    state->edge_pixels = 0u;
    state->search_steps = 0u;
    state->row = pixels != NULL ? 0u : (uint16_t)RAY_HEIGHT;
    state->done = pixels == NULL;
    state->active = pixels != NULL;
    state->framebuffer = pixels;
    if (pixels != NULL)
        memcpy(state->rows, pixels, (RAY_POST_ROW_RADIUS + 1u) * RAY_WIDTH);
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
            const uint8_t *current = state->rows[row % RAY_POST_CACHE_ROWS];
            uint8_t *output = pixels + row * RAY_WIDTH;
            unsigned x;
            for (x = 1u; x + 1u < RAY_WIDTH; ++x) {
                uint8_t filtered = post_pixel(state, x, row);
                if (filtered != current[x]) ++state->pixels_changed;
                output[x] = filtered;
            }
        }
        state->pixels_processed += RAY_WIDTH;
        state->row = (uint16_t)(row + 1u);
        ++completed;

        /* New window [row-5,row+5]: row+5 is untouched and replaces row-6.
         * The window update happens even when the batch stops here. */
        if ((unsigned)state->row + RAY_POST_ROW_RADIUS < RAY_HEIGHT) {
            unsigned next = (unsigned)state->row + RAY_POST_ROW_RADIUS;
            memcpy(state->rows[next % RAY_POST_CACHE_ROWS],
                   pixels + next * RAY_WIDTH, RAY_WIDTH);
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
