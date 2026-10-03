#ifndef SKYOS_RAY_SESSION_H
#define SKYOS_RAY_SESSION_H
#include "RayCore.h"
enum { RAY_UP=1u, RAY_DOWN=2u, RAY_LEFT=4u, RAY_RIGHT=8u,
       RAY_RESET=16u, RAY_CONTRAST=32u };
enum { RAY_PREVIEW=0u, RAY_WAIT=1u, RAY_TRACE=2u, RAY_DONE=3u };
enum { RAY_CHANGED=1u, RAY_CANCELLED=2u, RAY_STARTED=4u };
typedef struct {
    RayCamera camera;
    RayStats stats;
    uint32_t generation, trace_generation, last_motion_ms, last_step_ms;
    uint32_t traced_samples, cancellations;
    unsigned phase, contrast, pass, cursor, previous_buttons, preview_dirty;
} RaySession;
void RaySession_Init(RaySession *session, uint32_t now_ms);
/* Input updates never trace rays. Held movement restarts the idle deadline. */
unsigned RaySession_Update(RaySession *session, uint32_t now_ms, unsigned buttons);
/* Produces bounded batches after the idle transition; nested samples are reused. */
unsigned RaySession_TraceBatch(RaySession *session, uint8_t *pixels,
                              unsigned max_samples, uint32_t budget_us);
#endif
