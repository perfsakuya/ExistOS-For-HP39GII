#ifndef SKYOS_RAY_POST_H
#define SKYOS_RAY_POST_H

#include "RayCore.h"

#define RAY_POST_SEARCH_RADIUS 4u
#define RAY_POST_ROW_RADIUS (RAY_POST_SEARCH_RADIUS + 1u)
#define RAY_POST_CACHE_ROWS (2u * RAY_POST_ROW_RADIUS + 1u)
#define RAY_POST_CACHE_BYTES (RAY_POST_CACHE_ROWS * RAY_WIDTH)

/* Keep this state static or in persistent session storage, not on a task stack.
 * Exactly 2816 bytes hold 11 original rows. On the 32-bit ARM ABI sizeof is 2848.
 * Searches reach +/-4 pixels; a bilinear fetch can additionally read row +1,
 * so original rows within +/-5 of the output row remain available.
 * row is the next framebuffer row, 0..127; done is set at row == RAY_HEIGHT.
 * pixels_processed includes unchanged frame borders and reaches RAY_PIXELS.
 * aa_us includes initialization copies plus active batch work; batch_max_us
 * measures active batches only. Both exclude caller yields and trace time.
 * edge_pixels counts non-flat/non-axis-coherent edge candidates; search_steps
 * counts endpoint sample pairs, at most 8 per candidate. */
typedef struct {
    uint8_t rows[RAY_POST_CACHE_ROWS][RAY_WIDTH];
    uint32_t pixels_processed, pixels_changed, aa_us, batch_max_us;
    uint32_t edge_pixels, search_steps;
    uint16_t row;
    uint8_t done, active;
    uint8_t *framebuffer;
} RayPostState;

/* Begin on a completed 256x127 grayscale trace. The caller owns the on/off
 * switch and must keep this frame stable until completion or cancellation.
 * Reinitialization starts from the current buffer; NULL cancels without reads.
 * Reinitializing a partially filtered frame cannot restore its old raw pixels. */
void RayPost_Init(RayPostState *state, uint8_t *pixels);

/* In-place, deterministic across batch sizes: each pixel uses original rows.
 * Returns rows completed this call. max_rows == 0 makes no progress.
 * budget_us == 0 disables the clock limit; otherwise check after each row,
 * making at least one row of progress. The row cap also works without a clock.
 * A NULL/wrong buffer or inactive/completed state makes no progress.
 * The outermost frame borders stay byte-exact. This is a bounded grayscale CPU
 * FXAA adaptation: integer luminance, weighted second-order edge direction,
 * strongest-gradient pair, +/-4 endpoint search, Q8 subpixel resampling, and
 * capped subpixel blending. It is not a verbatim GPU shader implementation. */
unsigned RayPost_Batch(RayPostState *state, uint8_t *pixels,
                       unsigned max_rows, uint32_t budget_us);

#endif
