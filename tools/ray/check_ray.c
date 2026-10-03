/* Host-only checks of the production Ray core, preview and session APIs.
 * Compile RayCore.c with -DRay_TracePixel=Ray_TestRealTracePixel so this
 * independent observer can prove which pixels the production session samples.
 * No host elapsed time is an estimate of ARM execution time. */
#include "../../System/applications/user/ray_demo/RayCore.h"
#include "../../System/applications/user/ray_demo/RaySession.h"
#include "../../System/applications/user/ray_demo/RayPost.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUARD 32u
#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "CHECK_FAIL line=%u condition=%s\n", (unsigned)__LINE__, #expr); \
    return 1; } } while (0)
static uint8_t guarded[RAY_PIXELS + 2u * GUARD];
static uint8_t direct[RAY_PIXELS], again[RAY_PIXELS];
static uint8_t post_whole[RAY_PIXELS];
static uint8_t visits[RAY_PIXELS];
static unsigned observe, observed, duplicate, out_of_bounds;
static unsigned pass_samples[3];
static uint32_t fake_us;

uint8_t Ray_TestRealTracePixel(const RayCamera *, unsigned, unsigned, unsigned, RayStats *);
uint8_t Ray_TracePixel(const RayCamera *camera, unsigned x, unsigned y,
                      unsigned contrast, RayStats *stats) {
    if (observe) {
        if (x >= RAY_WIDTH || y >= RAY_HEIGHT) ++out_of_bounds;
        else {
            const unsigned i = y * RAY_WIDTH + x;
            if (visits[i]++) ++duplicate;
            ++observed;
            ++pass_samples[(!(x % 4u) && !(y % 4u)) ? 0u :
                           (!(x % 2u) && !(y % 2u)) ? 1u : 2u];
        }
    }
    return Ray_TestRealTracePixel(camera, x, y, contrast, stats);
}

static uint32_t advancing_clock(void) { fake_us += 13u; return fake_us; }
static void reset_observer(void) {
    memset(visits, 0, sizeof(visits));
    memset(pass_samples, 0, sizeof(pass_samples));
    observed = duplicate = out_of_bounds = 0u;
}
static uint8_t *new_frame(void) {
    memset(guarded, 0xa5, sizeof(guarded));
    return guarded + GUARD;
}
static int guards_valid(void) {
    for (unsigned i = 0; i < GUARD; ++i)
        if (guarded[i] != 0xa5 || guarded[GUARD + RAY_PIXELS + i] != 0xa5) return 0;
    return 1;
}
static unsigned differences(const uint8_t *a, const uint8_t *b) {
    unsigned result = 0;
    for (unsigned i = 0; i < RAY_PIXELS; ++i) result += a[i] != b[i];
    return result;
}
static int save_pgm(const char *directory, const char *name, const uint8_t *pixels) {
    char path[1024];
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= (int)sizeof(path)) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    fprintf(file, "P5\n%u %u\n255\n", RAY_WIDTH, RAY_HEIGHT);
    const int wrote = fwrite(pixels, 1u, RAY_PIXELS, file) == RAY_PIXELS;
    return fclose(file) == 0 && wrote;
}
static void trace_direct(const RayCamera *camera, unsigned contrast,
                         uint8_t *pixels, RayStats *stats) {
    memset(stats, 0, sizeof(*stats));
    for (unsigned y = 0; y < RAY_HEIGHT; ++y)
        for (unsigned x = 0; x < RAY_WIDTH; ++x)
            pixels[y * RAY_WIDTH + x] = Ray_TracePixel(camera, x, y, contrast, stats);
}
static int no_trace_stats(const RayStats *stats) {
    return !stats->primary_rays && !stats->reflection_rays && !stats->shadow_rays &&
           !stats->refraction_rays && !stats->glass_exits && !stats->tir_events &&
           !stats->floor_reflection_rays && !stats->refraction_us &&
           !stats->sphere_tests && !stats->plane_tests && !stats->hits;
}

static int unit_finite(RayVec3 direction) {
    return isfinite(direction.x) && isfinite(direction.y) && isfinite(direction.z) &&
           fabsf(direction.x * direction.x + direction.y * direction.y +
                 direction.z * direction.z - 1.0f) < 0.0001f;
}

static float check_dot(RayVec3 a, RayVec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static int camera_basis_valid(const RayCamera *camera) {
    const RayVec3 cross = {camera->right.y * camera->up.z - camera->right.z * camera->up.y,
                          camera->right.z * camera->up.x - camera->right.x * camera->up.z,
                          camera->right.x * camera->up.y - camera->right.y * camera->up.x};
    return unit_finite(camera->forward) && unit_finite(camera->right) && unit_finite(camera->up) &&
           fabsf(check_dot(camera->forward, camera->right)) < 0.0001f &&
           fabsf(check_dot(camera->forward, camera->up)) < 0.0001f &&
           fabsf(check_dot(camera->right, camera->up)) < 0.0001f &&
           check_dot(cross, camera->forward) > 0.9999f;
}

static int check_orientation(const char *directory) {
    RayCamera camera, original, tilted, horizontal;
    RayStats stats;
    RayCamera_Init(&camera);
    REQUIRE(camera_basis_valid(&camera));
    REQUIRE(fabsf(camera.pitch + 0.1001674212f) < 0.00001f && camera.roll == 0.0f);
    original = camera;
    REQUIRE(!RayCamera_Orient(&camera, 0.0f, 0.0f, 0.0f));
    REQUIRE(!RayCamera_Orient(NULL, 0.0f, 0.0f, 0.0f));
    REQUIRE(!RayCamera_Orient(&camera, NAN, 0.0f, 0.0f));
    REQUIRE(!RayCamera_Orient(&camera, 0.0f, INFINITY, 0.0f));
    REQUIRE(!RayCamera_Orient(&camera, 0.0f, 0.0f, -INFINITY));
    REQUIRE(memcmp(&camera, &original, sizeof(camera)) == 0);
    camera.pitch = NAN;
    tilted = camera;
    REQUIRE(!RayCamera_Orient(&camera, 0.1f, 0.1f, 0.1f));
    REQUIRE(memcmp(&camera, &tilted, sizeof(camera)) == 0);
    camera = original;
    REQUIRE(RayCamera_Orient(&camera, 1.0e30f, 1.0e30f, -1.0e30f));
    REQUIRE(camera_basis_valid(&camera));
    REQUIRE(camera.pitch <= 1.396264f && camera.pitch >= -1.396264f);
    REQUIRE(camera.yaw >= -3.141593f && camera.yaw <= 3.141593f);
    REQUIRE(camera.roll >= -3.141593f && camera.roll <= 3.141593f);
    REQUIRE(!RayCamera_Orient(&camera, 0.0f, 100.0f, 0.0f));
    REQUIRE(RayCamera_Orient(&camera, -1.0e30f, -1.0e30f, 1.0e30f));
    REQUIRE(camera_basis_valid(&camera) && camera.pitch >= -1.396264f);
    for (unsigned i = 0; i < 400u; ++i) {
        RayCamera_Orient(&camera, 0.73f, (i & 1u) ? -0.13f : 0.19f, 0.37f);
        REQUIRE(camera_basis_valid(&camera));
        REQUIRE(camera.pitch >= -1.396264f && camera.pitch <= 1.396264f);
    }
    horizontal = tilted = original;
    REQUIRE(RayCamera_Orient(&horizontal, 0.3f, 0.0f, 0.0f));
    REQUIRE(RayCamera_Orient(&tilted, 0.3f, 0.7f, 1.1f));
    REQUIRE(RayCamera_Move(&horizontal, 0.2f, 0.03f));
    REQUIRE(RayCamera_Move(&tilted, 0.2f, 0.03f));
    REQUIRE(fabsf(horizontal.position.x - tilted.position.x) < 0.00001f);
    REQUIRE(fabsf(horizontal.position.z - tilted.position.z) < 0.00001f);
    REQUIRE(horizontal.position.y == original.position.y && tilted.position.y == original.position.y);
    REQUIRE(camera_basis_valid(&horizontal) && camera_basis_valid(&tilted));

    trace_direct(&original, 1u, direct, &stats);
    RayPreview_Render(again, &original, 1u, NULL);
    const float angles[][3] = {{0.0f, 0.25f, 0.0f}, {0.0f, 0.0f, 0.30f},
                              {0.0f, 0.12f, 0.28f}, {0.0f, 10.0f, 1.5f}, {0.0f, -10.0f, -1.5f}};
    const char *preview_names[] = {"preview-pitch.pgm", "preview-roll.pgm", "preview-tilt.pgm"};
    for (unsigned i = 0; i < sizeof(angles) / sizeof(angles[0]); ++i) {
        camera = original;
        REQUIRE(RayCamera_Orient(&camera, angles[i][0], angles[i][1], angles[i][2]));
        REQUIRE(camera_basis_valid(&camera));
        uint8_t *pixels = new_frame();
        memset(&stats, 0, sizeof(stats));
        RayPreview_Render(pixels, &camera, 1u, &stats);
        REQUIRE(guards_valid() && no_trace_stats(&stats));
        REQUIRE(differences(pixels, again) > 1000u);
        if (i < 3u) REQUIRE(save_pgm(directory, preview_names[i], pixels));
        trace_direct(&camera, 1u, pixels, &stats);
        REQUIRE(guards_valid() && stats.primary_rays == RAY_PIXELS);
        REQUIRE(differences(pixels, direct) > 1000u);
        if (i == 2u) REQUIRE(save_pgm(directory, "trace-tilt.pgm", pixels));
    }
    puts("CHECK_PASS camera_orthonormal_extreme_angles_atomic_invalid_horizontal_motion_pose_images_guards");
    return 0;
}

static int check_refraction(void) {
    const RayVec3 normal = {0.0f, 1.0f, 0.0f};
    const RayVec3 incident = {0.0f, -1.0f, 0.0f};
    const RayVec3 sentinel = {7.0f, 8.0f, 9.0f};
    RayVec3 result, reciprocal;
    REQUIRE(Ray_RefractDirection(incident, normal, 1.0f / 1.5f, &result));
    REQUIRE(unit_finite(result));
    REQUIRE(fabsf(result.x) < 0.00001f && fabsf(result.y + 1.0f) < 0.00001f &&
            fabsf(result.z) < 0.00001f);
    /* sin(theta_t) = eta * sin(theta_i), with theta_i = 30 degrees. */
    const RayVec3 angled = {0.5f, -0.866025404f, 0.0f};
    REQUIRE(Ray_RefractDirection(angled, normal, 1.0f / 1.5f, &result));
    REQUIRE(unit_finite(result));
    REQUIRE(fabsf(result.x - 1.0f / 3.0f) < 0.00001f);
    REQUIRE(fabsf(result.y + sqrtf(8.0f / 9.0f)) < 0.00001f && fabsf(result.z) < 0.00001f);
    REQUIRE(Ray_RefractDirection(result, normal, 1.5f, &reciprocal));
    REQUIRE(unit_finite(reciprocal));
    REQUIRE(fabsf(reciprocal.x - angled.x) < 0.00001f &&
            fabsf(reciprocal.y - angled.y) < 0.00001f);
    /* Glass-to-air critical sine is 1/1.5: bracket its two sides. */
    const RayVec3 below_critical = {0.665f, -sqrtf(1.0f - 0.665f * 0.665f), 0.0f};
    const RayVec3 above_critical = {0.668f, -sqrtf(1.0f - 0.668f * 0.668f), 0.0f};
    REQUIRE(Ray_RefractDirection(below_critical, normal, 1.5f, &result));
    REQUIRE(unit_finite(result));
    result = sentinel;
    REQUIRE(!Ray_RefractDirection(above_critical, normal, 1.5f, &result));
    REQUIRE(memcmp(&result, &sentinel, sizeof(result)) == 0);
    const RayVec3 invalid_vectors[] = {{0.0f, 0.0f, 0.0f}, {NAN, -1.0f, 0.0f},
                                      {0.0f, -INFINITY, 0.0f}};
    for (unsigned i = 0; i < sizeof(invalid_vectors) / sizeof(invalid_vectors[0]); ++i) {
        result = sentinel;
        REQUIRE(!Ray_RefractDirection(invalid_vectors[i], normal, 1.0f / 1.5f, &result));
        REQUIRE(memcmp(&result, &sentinel, sizeof(result)) == 0);
        REQUIRE(!Ray_RefractDirection(incident, invalid_vectors[i], 1.0f / 1.5f, &result));
        REQUIRE(memcmp(&result, &sentinel, sizeof(result)) == 0);
    }
    const float invalid_ratios[] = {0.0f, -1.0f, NAN, INFINITY};
    for (unsigned i = 0; i < sizeof(invalid_ratios) / sizeof(invalid_ratios[0]); ++i) {
        result = sentinel;
        REQUIRE(!Ray_RefractDirection(incident, normal, invalid_ratios[i], &result));
        REQUIRE(memcmp(&result, &sentinel, sizeof(result)) == 0);
    }
    REQUIRE(!Ray_RefractDirection(incident, normal, 1.0f / 1.5f, NULL));
    puts("CHECK_PASS refraction_normal_snell_reciprocity_critical_angle_tir_invalid_inputs");
    return 0;
}

static int check_core(const char *directory) {
    RayCamera camera, moved;
    RayStats stats, unused = {0};
    RayCamera_Init(&camera);
    REQUIRE(camera.position.x == 0.0f && camera.position.z == -5.5f);
    REQUIRE(fabsf(camera.position.y - 1.45f) < 0.0001f);
    REQUIRE(camera.yaw == 0.0f);
    for (unsigned c = 0; c < 3u; ++c) {
        trace_direct(&camera, c, direct, &stats);
        REQUIRE(stats.primary_rays == RAY_PIXELS);
        REQUIRE(stats.reflection_rays > 0u && stats.shadow_rays > 0u);
        REQUIRE(stats.refraction_rays > 0u && stats.glass_exits > 0u && stats.floor_reflection_rays > 0u);
        REQUIRE(stats.sphere_tests > RAY_PIXELS && stats.plane_tests >= RAY_PIXELS);
        trace_direct(&camera, c, again, &unused);
        REQUIRE(memcmp(direct, again, RAY_PIXELS) == 0);
        if (c) REQUIRE(differences(direct, guarded + GUARD) > 100u);
        memcpy(guarded + GUARD, direct, RAY_PIXELS);
        char name[32];
        snprintf(name, sizeof(name), "trace-c%u.pgm", c + 1u);
        REQUIRE(save_pgm(directory, name, direct));
    }
    trace_direct(&camera, 1u, direct, &stats);
    moved = camera;
    REQUIRE(RayCamera_Move(&moved, 0.3f, 0.15f));
    trace_direct(&moved, 1u, again, &unused);
    REQUIRE(differences(direct, again) > 1000u);
    memset(&stats, 0, sizeof(stats));
    REQUIRE(Ray_TracePixel(NULL, 0u, 0u, 0u, &stats) == 0u);
    REQUIRE(Ray_TracePixel(&camera, RAY_WIDTH, 0u, 0u, &stats) == 0u);
    REQUIRE(Ray_TracePixel(&camera, 0u, RAY_HEIGHT, 0u, &stats) == 0u);
    REQUIRE(no_trace_stats(&stats));
    REQUIRE(Ray_MapGray(NAN, 0u) == 0u && Ray_MapGray(INFINITY, 0u) == 255u);
    REQUIRE(Ray_MapGray(-INFINITY, 0u) == 0u);
    REQUIRE(Ray_MapGray(0.4f, 99u) == Ray_MapGray(0.4f, 2u));
    for (unsigned c = 0; c < 3u; ++c) {
        unsigned previous = 0u;
        for (unsigned i = 0; i <= 256u; ++i) {
            const unsigned value = Ray_MapGray((float)i / 256.0f, c);
            REQUIRE(value >= previous);
            previous = value;
        }
    }
    puts("CHECK_PASS core_determinism_view_contrast_api");
    return 0;
}

static int check_preview(const char *directory) {
    RayCamera camera, moved;
    RayStats stats;
    RayCamera_Init(&camera);
    for (unsigned c = 0; c < 3u; ++c) {
        uint8_t *pixels = new_frame();
        memset(&stats, 0, sizeof(stats));
        reset_observer(); observe = 1u;
        RayPreview_Render(pixels, &camera, c, &stats);
        observe = 0u;
        REQUIRE(guards_valid() && no_trace_stats(&stats) && observed == 0u);
        REQUIRE(stats.preview_triangles > 0u && stats.preview_pixels > 0u);
        RayPreview_Render(again, &camera, c, NULL);
        REQUIRE(memcmp(pixels, again, RAY_PIXELS) == 0);
        if (c == 1u) {
            REQUIRE(save_pgm(directory, "preview.pgm", pixels));
            memcpy(direct, pixels, RAY_PIXELS);
        }
    }
    moved = camera;
    REQUIRE(RayCamera_Move(&moved, 0.2f, 0.15f));
    RayPreview_Render(again, &moved, 1u, NULL);
    REQUIRE(differences(direct, again) > 100u);
    puts("CHECK_PASS preview_frame_guards_determinism_view_zero_rt");
    return 0;
}

static int check_movement(void) {
    RayCamera camera, original;
    RayCamera_Init(&camera);
    original = camera;
    REQUIRE(!RayCamera_Move(&camera, 100.0f, 0.0f));
    REQUIRE(memcmp(&camera, &original, sizeof(camera)) == 0);
    REQUIRE(!RayCamera_Move(&camera, NAN, 0.0f));
    REQUIRE(!RayCamera_Move(&camera, 0.0f, INFINITY));
    REQUIRE(memcmp(&camera, &original, sizeof(camera)) == 0);
    for (unsigned i = 0; i < 1000u; ++i) {
        RayCamera_Move(&camera, 0.05f, 0.137f);
        REQUIRE(camera.position.x >= -6.0f && camera.position.x <= 6.0f);
        REQUIRE(camera.position.z >= -7.0f && camera.position.z <= 7.0f);
        REQUIRE(camera.yaw >= -3.141593f && camera.yaw <= 3.141593f);
        REQUIRE(fabsf(camera.position.y - 1.45f) < 0.0001f);
    }
    for (unsigned i = 0; i < RAY_SPHERE_COUNT; ++i) {
        RayCamera_Init(&camera);
        camera.position.x = Ray_Spheres[i].center.x;
        camera.position.z = Ray_Spheres[i].center.z - Ray_Spheres[i].radius - 0.4f;
        original = camera;
        REQUIRE(!RayCamera_Move(&camera, 2.0f * Ray_Spheres[i].radius + 0.8f, 0.0f));
        REQUIRE(memcmp(&camera, &original, sizeof(camera)) == 0);
    }
    puts("CHECK_PASS movement_world_bounds_segment_collision_finite_input");
    return 0;
}

static int check_deadline_and_controls(void) {
    RaySession session;
    uint8_t *pixels = new_frame();
    RaySession_Init(&session, 0u);
    const RayCamera initial = session.camera;
    reset_observer(); observe = 1u;
    RaySession_Update(&session, 39u, RAY_UP);
    REQUIRE(memcmp(&initial, &session.camera, sizeof(initial)) == 0);
    RaySession_Update(&session, 40u, RAY_UP);
    REQUIRE(session.camera.position.z > initial.position.z);
    RaySession_Update(&session, 50u, RAY_UP);
    REQUIRE(RaySession_TraceBatch(&session, pixels, 100u, 0u) == 0u);
    RaySession_Update(&session, 65u, 0u);
    REQUIRE(session.last_motion_ms == 65u);
    REQUIRE(!(RaySession_Update(&session, 1064u, 0u) & RAY_STARTED));
    REQUIRE(session.phase == RAY_WAIT && session.traced_samples == 0u);
    REQUIRE(RaySession_Update(&session, 1065u, 0u) & RAY_STARTED);
    REQUIRE(observed == 0u && no_trace_stats(&session.stats));
    observe = 0u;
    RaySession_Init(&session, UINT32_MAX - 500u);
    REQUIRE(!(RaySession_Update(&session, (uint32_t)(UINT32_MAX - 500u + 999u), 0u) & RAY_STARTED));
    REQUIRE(RaySession_Update(&session, (uint32_t)(UINT32_MAX - 500u + 1000u), 0u) & RAY_STARTED);
    RaySession_Init(&session, 0u);
    const unsigned first_contrast = session.contrast;
    const uint32_t first_generation = session.generation;
    for (unsigned i = 1u; i <= 3u; ++i) {
        REQUIRE(RaySession_Update(&session, i * 20u, RAY_CONTRAST) & RAY_CHANGED);
        REQUIRE(session.contrast == (first_contrast + i) % 3u);
        const uint32_t generation = session.generation;
        REQUIRE(!(RaySession_Update(&session, i * 20u + 1u, RAY_CONTRAST) & RAY_CHANGED));
        REQUIRE(session.generation == generation);
        RaySession_Update(&session, i * 20u + 2u, 0u);
    }
    REQUIRE(session.contrast == first_contrast && session.generation == first_generation + 3u);
    RaySession_Update(&session, 120u, RAY_UP | RAY_RIGHT);
    REQUIRE(RaySession_Update(&session, 121u, RAY_RESET) & RAY_CHANGED);
    REQUIRE(memcmp(&initial, &session.camera, sizeof(initial)) == 0);
    REQUIRE(no_trace_stats(&session.stats) && guards_valid());
    puts("CHECK_PASS no_trace_during_movement_999ms_1000ms_clock_wrap_controls");
    return 0;
}

static int check_cancellation_and_budget(void) {
    RaySession session;
    uint8_t *pixels = new_frame();
    RaySession_Init(&session, 0u);
    RaySession_Update(&session, 1000u, 0u);
    REQUIRE(RaySession_TraceBatch(&session, pixels, 0u, 0u) == 0u);
    REQUIRE(RaySession_TraceBatch(&session, pixels, 19u, 0u) == 19u);
    const uint32_t generation = session.generation;
    REQUIRE(RaySession_Update(&session, 1040u, RAY_RIGHT) & RAY_CANCELLED);
    REQUIRE(session.generation > generation && session.cancellations == 1u);
    REQUIRE(session.traced_samples == 0u && session.phase == RAY_PREVIEW);
    REQUIRE(RaySession_TraceBatch(&session, pixels, 100u, 0u) == 0u);
    RaySession_Update(&session, 1053u, 0u);
    REQUIRE(session.last_motion_ms == 1053u);
    REQUIRE(!(RaySession_Update(&session, 2052u, 0u) & RAY_STARTED));
    REQUIRE(RaySession_Update(&session, 2053u, 0u) & RAY_STARTED);
    ++session.generation;
    REQUIRE(RaySession_TraceBatch(&session, pixels, 11u, 0u) == 0u);
    session.generation = session.trace_generation;
    REQUIRE(RaySession_TraceBatch(NULL, pixels, 11u, 0u) == 0u);
    REQUIRE(RaySession_TraceBatch(&session, NULL, 11u, 0u) == 0u);
    REQUIRE(RaySession_Update(NULL, 0u, 0u) == 0u);
    RaySession_Init(NULL, 0u);
    Ray_SetClock(NULL);
    REQUIRE(Ray_Clock() == 0u);
    REQUIRE(RaySession_TraceBatch(&session, pixels, 11u, 1u) == 11u);
    fake_us = UINT32_MAX - 64u;
    Ray_SetClock(advancing_clock);
    REQUIRE(RaySession_TraceBatch(&session, pixels, 99u, 32u) == 8u);
    Ray_SetClock(NULL);
    REQUIRE(guards_valid());
    puts("CHECK_PASS cancellation_generation_gate_bounded_batch_budget_clock_wrap_api");
    return 0;
}

static int check_pose_keys_and_aa_control(void) {
    const unsigned pose_buttons[] = {RAY_PITCH_DOWN, RAY_PITCH_UP, RAY_ROLL_LEFT, RAY_ROLL_RIGHT};
    RaySession session;
    uint8_t *pixels = new_frame();
    for (unsigned i = 0; i < sizeof(pose_buttons) / sizeof(pose_buttons[0]); ++i) {
        RaySession_Init(&session, 0u);
        const RayCamera original = session.camera;
        reset_observer(); observe = 1u;
        REQUIRE(pose_buttons[i] & RAY_MOTION_MASK);
        REQUIRE(!session.aa_enabled);
        RaySession_Update(&session, 39u, pose_buttons[i]);
        REQUIRE(memcmp(&session.camera, &original, sizeof(original)) == 0);
        REQUIRE(RaySession_Update(&session, 40u, pose_buttons[i]) & RAY_CHANGED);
        REQUIRE(session.phase == RAY_PREVIEW && camera_basis_valid(&session.camera));
        REQUIRE(i < 2u ? session.camera.pitch != original.pitch : session.camera.roll != original.roll);
        RaySession_Update(&session, 55u, pose_buttons[i]);
        REQUIRE(RaySession_TraceBatch(&session, pixels, 128u, 0u) == 0u);
        RaySession_Update(&session, 70u, 0u);
        REQUIRE(session.last_motion_ms == 70u);
        REQUIRE(!(RaySession_Update(&session, 1069u, 0u) & RAY_STARTED));
        REQUIRE(RaySession_Update(&session, 1070u, 0u) & RAY_STARTED);
        REQUIRE(observed == 0u && no_trace_stats(&session.stats));
        observe = 0u;
    }
    /* Held pitch at its limit still postpones the quiet deadline. */
    RaySession_Init(&session, 0u);
    REQUIRE(RayCamera_Orient(&session.camera, 0.0f, 100.0f, 0.0f));
    RaySession_Update(&session, 40u, RAY_PITCH_UP);
    RaySession_Update(&session, 2040u, RAY_PITCH_UP);
    REQUIRE(session.phase == RAY_PREVIEW && session.last_motion_ms == 2040u);
    RaySession_Update(&session, 2050u, 0u);
    REQUIRE(!(RaySession_Update(&session, 3049u, 0u) & RAY_STARTED));
    REQUIRE(RaySession_Update(&session, 3050u, 0u) & RAY_STARTED);

    RaySession_Init(&session, 0u);
    REQUIRE(RaySession_Update(&session, 20u, RAY_AA_TOGGLE) & RAY_CHANGED);
    REQUIRE(session.aa_enabled == 1u);
    const uint32_t toggled_generation = session.generation;
    REQUIRE(!(RaySession_Update(&session, 21u, RAY_AA_TOGGLE) & RAY_CHANGED));
    REQUIRE(session.aa_enabled == 1u && session.generation == toggled_generation);
    RaySession_Update(&session, 22u, 0u);
    REQUIRE(RaySession_Update(&session, 30u, RAY_AA_TOGGLE) & RAY_CHANGED);
    REQUIRE(session.aa_enabled == 0u);
    RaySession_Update(&session, 31u, 0u);
    REQUIRE(RaySession_Update(&session, 40u, RAY_AA_TOGGLE) & RAY_CHANGED);
    REQUIRE(session.aa_enabled == 1u);
    RaySession_Update(&session, 41u, 0u);
    REQUIRE(RaySession_Update(&session, 50u, RAY_RESET) & RAY_CHANGED);
    REQUIRE(session.aa_enabled == 1u);
    RaySession_Update(&session, 51u, 0u);
    REQUIRE(RaySession_Update(&session, 1050u, 0u) & RAY_STARTED);
    unsigned batches = 0u;
    while (session.phase == RAY_TRACE) {
        REQUIRE(++batches < 100u);
        REQUIRE(RaySession_TraceBatch(&session, pixels, 512u, 0u) <= 512u);
    }
    REQUIRE(session.phase == RAY_POSTAA && session.traced_samples == RAY_PIXELS);
    REQUIRE(session.stats.primary_rays == RAY_PIXELS);
    RayStats direct_stats;
    trace_direct(&session.camera, session.contrast, again, &direct_stats);
    REQUIRE(memcmp(pixels, again, RAY_PIXELS) == 0);
    const RayStats before_post = session.stats;
    RayPostState pending_post;
    RayPost_Init(&pending_post, pixels);
    reset_observer(); observe = 1u;
    REQUIRE(RayPost_Batch(&pending_post, pixels, 8u, 0u) == 8u);
    REQUIRE(pending_post.row == 8u && !pending_post.done);
    REQUIRE(memcmp(&before_post, &session.stats, sizeof(before_post)) == 0);
    const RaySession post_session = session;
    REQUIRE(RaySession_TraceBatch(&session, pixels, 512u, 0u) == 0u);
    const unsigned cancellation_buttons[] = {RAY_PITCH_DOWN, RAY_ROLL_RIGHT, RAY_RESET,
                                             RAY_CONTRAST, RAY_AA_TOGGLE};
    for (unsigned i = 0; i < sizeof(cancellation_buttons) / sizeof(cancellation_buttons[0]); ++i) {
        session = post_session;
        REQUIRE(RaySession_Update(&session, 1090u, cancellation_buttons[i]) & RAY_CANCELLED);
        REQUIRE(session.generation > post_session.generation && session.traced_samples == 0u);
        REQUIRE(session.phase == ((cancellation_buttons[i] & RAY_MOTION_MASK) ? RAY_PREVIEW : RAY_WAIT));
        REQUIRE(RaySession_TraceBatch(&session, pixels, 512u, 0u) == 0u);
    }
    RayPost_Init(&pending_post, NULL);
    REQUIRE(RayPost_Batch(&pending_post, pixels, RAY_HEIGHT, 0u) == 0u);
    observe = 0u;
    REQUIRE(observed == 0u && guards_valid());
    puts("CHECK_PASS pose_keys_limit_hold_1000ms_aa_edge_reset_preserves_aa_postaa_cancellation_zero_new_rays");
    return 0;
}

static int finish_post(RayPostState *post, uint8_t *pixels, unsigned rows) {
    unsigned batches = 0u;
    while (!post->done) {
        const unsigned count = RayPost_Batch(post, pixels, rows, 0u);
        REQUIRE(count > 0u && count <= rows && ++batches <= RAY_HEIGHT);
        REQUIRE(post->row <= RAY_HEIGHT && post->pixels_processed == (uint32_t)post->row * RAY_WIDTH);
        REQUIRE(guards_valid());
    }
    REQUIRE(post->row == RAY_HEIGHT && !post->active && post->pixels_processed == RAY_PIXELS);
    REQUIRE(RayPost_Batch(post, pixels, rows, 0u) == 0u);
    return 0;
}

/* Independent pixel-area reference for white half-plane x < 64 + y/2.
 * Integrate its horizontal coverage through each pixel with 32 vertical samples. */
static unsigned diagonal_coverage(unsigned x, unsigned y) {
    float covered = 0.0f;
    for (unsigned sample = 0u; sample < 32u; ++sample) {
        float width = 64.0f + 0.5f * ((float)y + ((float)sample + 0.5f) / 32.0f) - (float)x;
        if (width < 0.0f) width = 0.0f;
        if (width > 1.0f) width = 1.0f;
        covered += width;
    }
    return (unsigned)(covered * (255.0f / 32.0f) + 0.5f);
}

static uint64_t diagonal_error(const uint8_t *pixels) {
    uint64_t error = 0u;
    for (unsigned y = 0u; y < RAY_HEIGHT; ++y)
        for (unsigned x = 0u; x < RAY_WIDTH; ++x) {
            const int difference = (int)pixels[y * RAY_WIDTH + x] - (int)diagonal_coverage(x, y);
            error += (uint64_t)(difference * difference);
        }
    return error;
}

static int check_post(const char *directory) {
    RayPostState post;
    RayCamera camera;
    RayStats stats;
    /* Flat frames, axial steps and one-pixel axial lines stay byte-exact. */
    for (unsigned pattern = 0u; pattern < 5u; ++pattern) {
        uint8_t *pixels = new_frame();
        for (unsigned y = 0u; y < RAY_HEIGHT; ++y)
            for (unsigned x = 0u; x < RAY_WIDTH; ++x) {
                unsigned value = 37u;
                if (pattern == 1u) value = x < RAY_WIDTH / 2u ? 37u : 213u;
                if (pattern == 2u) value = y < RAY_HEIGHT / 2u ? 37u : 213u;
                if (pattern == 3u) value = x == RAY_WIDTH / 2u ? 255u : 0u;
                if (pattern == 4u) value = y == RAY_HEIGHT / 2u ? 255u : 0u;
                pixels[y * RAY_WIDTH + x] = (uint8_t)value;
            }
        memcpy(direct, pixels, RAY_PIXELS);
        RayPost_Init(&post, pixels);
        REQUIRE(!finish_post(&post, pixels, 7u));
        REQUIRE(guards_valid() && memcmp(pixels, direct, RAY_PIXELS) == 0);
        REQUIRE(post.pixels_changed == 0u);
    }
    for (unsigned y = 0u; y < RAY_HEIGHT; ++y)
        for (unsigned x = 0u; x < RAY_WIDTH; ++x)
            direct[y * RAY_WIDTH + x] = (float)x + 0.5f < 64.0f + 0.5f * ((float)y + 0.5f) ? 255u : 0u;
    REQUIRE(save_pgm(directory, "aa-diagonal-before.pgm", direct));
    uint8_t *pixels = new_frame();
    memcpy(pixels, direct, RAY_PIXELS);
    RayPost_Init(&post, pixels);
    const RayPostState before_zero = post;
    REQUIRE(RayPost_Batch(&post, pixels, 0u, 0u) == 0u);
    REQUIRE(memcmp(&post, &before_zero, sizeof(post)) == 0);
    REQUIRE(RayPost_Batch(&post, again, RAY_HEIGHT, 0u) == 0u);
    REQUIRE(RayPost_Batch(NULL, pixels, RAY_HEIGHT, 0u) == 0u);
    REQUIRE(RayPost_Batch(&post, NULL, RAY_HEIGHT, 0u) == 0u);
    reset_observer(); observe = 1u;
    REQUIRE(RayPost_Batch(&post, pixels, RAY_HEIGHT, 0u) == RAY_HEIGHT);
    REQUIRE(post.done && !post.active && post.pixels_processed == RAY_PIXELS);
    REQUIRE(post.pixels_changed == differences(pixels, direct) && post.pixels_changed > 0u);
    const uint64_t error_before = diagonal_error(direct), error_after = diagonal_error(pixels);
    REQUIRE(error_after < error_before);
    REQUIRE(guards_valid());
    memcpy(post_whole, pixels, RAY_PIXELS);
    REQUIRE(save_pgm(directory, "aa-diagonal-after.pgm", pixels));
    const unsigned row_caps[] = {1u, 3u, 7u, 31u};
    for (unsigned i = 0u; i < sizeof(row_caps) / sizeof(row_caps[0]); ++i) {
        pixels = new_frame();
        memcpy(pixels, direct, RAY_PIXELS);
        RayPost_Init(&post, pixels);
        REQUIRE(!finish_post(&post, pixels, row_caps[i]));
        REQUIRE(memcmp(pixels, post_whole, RAY_PIXELS) == 0);
    }
    pixels = new_frame();
    memcpy(pixels, direct, RAY_PIXELS);
    RayPost_Init(&post, pixels);
    REQUIRE(RayPost_Batch(&post, pixels, 4u, 1u) == 4u);  /* Null clock respects row cap. */
    RayPost_Init(&post, NULL);
    REQUIRE(post.done && !post.active && post.pixels_processed == 0u);
    REQUIRE(RayPost_Batch(&post, pixels, RAY_HEIGHT, 0u) == 0u);
    RayPost_Init(&post, pixels);
    fake_us = UINT32_MAX - 6u;
    Ray_SetClock(advancing_clock);
    REQUIRE(RayPost_Batch(&post, pixels, RAY_HEIGHT, 1u) == 1u);
    Ray_SetClock(NULL);
    observe = 0u;
    REQUIRE(observed == 0u && guards_valid());
    printf("CHECK_PASS post_flat_axial_steps_lines_exact_diagonal_SSE_before=%llu_after=%llu_batch_equal_guards_budget_wrap_zero_rays\n",
           (unsigned long long)error_before, (unsigned long long)error_after);

    /* Real scene AA uses no new ray samples, for the level and oblique cameras. */
    for (unsigned pose = 0u; pose < 2u; ++pose) {
        RayCamera_Init(&camera);
        if (pose) REQUIRE(RayCamera_Orient(&camera, 0.0f, 0.12f, 0.28f));
        trace_direct(&camera, 1u, direct, &stats);
        const RayStats original_stats = stats;
        pixels = new_frame();
        memcpy(pixels, direct, RAY_PIXELS);
        RayPost_Init(&post, pixels);
        reset_observer(); observe = 1u;
        REQUIRE(!finish_post(&post, pixels, 8u));
        observe = 0u;
        REQUIRE(observed == 0u && memcmp(&stats, &original_stats, sizeof(stats)) == 0);
        REQUIRE(post.pixels_changed == differences(pixels, direct) && post.pixels_changed > 0u);
        REQUIRE(save_pgm(directory, pose ? "trace-tilt-aa.pgm" : "trace-aa.pgm", pixels));
        printf("CHECK_PASS post_scene_%s changed=%lu processed=%lu no_new_rays=1\n",
               pose ? "tilt" : "level", (unsigned long)post.pixels_changed,
               (unsigned long)post.pixels_processed);
    }
    return 0;
}

static int check_progressive(void) {
    RaySession session;
    RayStats stats;
    for (unsigned c = 0u; c < 3u; ++c) {
        uint8_t *pixels = new_frame();
        RaySession_Init(&session, 0u);
        session.contrast = c;
        REQUIRE(RaySession_Update(&session, 1000u, 0u) & RAY_STARTED);
        trace_direct(&session.camera, c, direct, &stats);
        reset_observer(); observe = 1u;
        unsigned batches = 0u;
        while (session.phase == RAY_TRACE) {
            const unsigned done = RaySession_TraceBatch(&session, pixels, 113u, 0u);
            REQUIRE(done <= 113u && ++batches < 400u);
            REQUIRE(guards_valid());
        }
        observe = 0u;
        REQUIRE(session.phase == RAY_DONE && session.pass == 3u);
        REQUIRE(session.traced_samples == RAY_PIXELS && observed == RAY_PIXELS);
        REQUIRE(session.stats.primary_rays == RAY_PIXELS && duplicate == 0u && out_of_bounds == 0u);
        REQUIRE(pass_samples[0] == 2048u && pass_samples[1] == 6144u && pass_samples[2] == 24320u);
        for (unsigned i = 0; i < RAY_PIXELS; ++i) REQUIRE(visits[i] == 1u);
        REQUIRE(memcmp(pixels, direct, RAY_PIXELS) == 0);
        REQUIRE(RaySession_TraceBatch(&session, pixels, 100u, 0u) == 0u);
        printf("CHECK_PASS progressive_c%u unique=%u passes=%u/%u/%u batches=%u direct_equal=1\n",
               c + 1u, observed, pass_samples[0], pass_samples[1], pass_samples[2], batches);
        REQUIRE(RaySession_Update(&session, 1040u, RAY_LEFT) & RAY_CHANGED);
        REQUIRE(session.phase == RAY_PREVIEW && session.traced_samples == 0u);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: check_ray output_directory\n"); return 2; }
    Ray_SetClock(NULL);
    if (check_refraction() || check_core(argv[1]) || check_preview(argv[1]) || check_movement() ||
        check_orientation(argv[1]) || check_deadline_and_controls() || check_cancellation_and_budget() ||
        check_pose_keys_and_aa_control() || check_progressive() || check_post(argv[1])) return 1;
    puts("CHECK_PASS all_host_ray_checks; physical_exit_and_ARM_timing_require_hardware");
    return 0;
}
