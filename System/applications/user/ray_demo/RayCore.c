#include "RayCore.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

/* This scene and camera are shared with the polygon preview. */
const RaySphere Ray_Spheres[RAY_SPHERE_COUNT] = {
    {{-1.45f, 0.75f, 1.25f}, 0.75f, 0.78f, 0.00f, 0.18f},
    {{ 0.15f, 0.90f, 2.10f}, 0.90f, 0.55f, 0.62f, 0.55f},
    {{ 1.65f, 0.65f, 0.80f}, 0.65f, 0.50f, 0.00f, 0.30f}
};

/* Unit vector from a surface towards the directional light. */
const RayVec3 Ray_LightDirection = {
    -0.43193421f, 0.86386843f, -0.25916053f
};

static const float sphere_inverse_radius[RAY_SPHERE_COUNT] = {
    1.33333333f, 1.11111111f, 1.53846154f
};

#define RAY_PI              3.14159265358979323846f
#define RAY_TWO_PI          6.28318530717958647692f
#define RAY_MIN_T           0.001f
#define RAY_ORIGIN_OFFSET   0.002f
#define RAY_CAMERA_RADIUS   0.18f
#define RAY_CAMERA_MIN_X   -6.0f
#define RAY_CAMERA_MAX_X    6.0f
#define RAY_CAMERA_MIN_Z   -7.0f
#define RAY_CAMERA_MAX_Z    7.0f
#define RAY_PITCH_SIN      -0.10f
#define RAY_PITCH_COS       0.9949874371f
#define RAY_SCREEN_SCALE   (0.70f / 128.0f)

typedef struct {
    RayVec3 point;
    RayVec3 normal;
    int object; /* -1 for the plane; 0..2 for a sphere. */
} RayHit;

static uint32_t (*ray_clock_us)(void);

static int finite_float(float value)
{
    return value == value && value <= FLT_MAX && value >= -FLT_MAX;
}

static int finite_vector(RayVec3 vector)
{
    return finite_float(vector.x) && finite_float(vector.y) &&
           finite_float(vector.z);
}

static float clamp_unit(float value)
{
    /* A NaN maps to black; infinities are clamped like other extremes. */
    if (!(value > 0.0f)) return 0.0f;
    if (value >= 1.0f) return 1.0f;
    return value;
}

static RayVec3 vec_add(RayVec3 a, RayVec3 b)
{
    RayVec3 result = {a.x + b.x, a.y + b.y, a.z + b.z};
    return result;
}

static RayVec3 vec_subtract(RayVec3 a, RayVec3 b)
{
    RayVec3 result = {a.x - b.x, a.y - b.y, a.z - b.z};
    return result;
}

static RayVec3 vec_scale(RayVec3 a, float scale)
{
    RayVec3 result = {a.x * scale, a.y * scale, a.z * scale};
    return result;
}

static float vec_dot(RayVec3 a, RayVec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static int vec_normalize(RayVec3 *vector)
{
    float length_squared = vec_dot(*vector, *vector);
    float inverse_length;
    if (!finite_float(length_squared) || length_squared < 1.0e-12f)
        return 0;
    inverse_length = 1.0f / sqrtf(length_squared);
    *vector = vec_scale(*vector, inverse_length);
    return 1;
}

void Ray_SetClock(uint32_t (*clock_us)(void))
{
    ray_clock_us = clock_us;
}

uint32_t Ray_Clock(void)
{
    return ray_clock_us != NULL ? ray_clock_us() : 0u;
}

static uint32_t timer_start(const RayStats *stats)
{
    return stats != NULL ? Ray_Clock() : 0u;
}

static void timer_finish(uint32_t *counter, uint32_t start)
{
    if (counter != NULL && ray_clock_us != NULL)
        *counter += Ray_Clock() - start; /* Unsigned subtraction handles wrap. */
}

static void camera_basis(RayCamera *camera)
{
    float sine = sinf(camera->yaw);
    float cosine = cosf(camera->yaw);
    camera->forward.x = sine * RAY_PITCH_COS;
    camera->forward.y = RAY_PITCH_SIN;
    camera->forward.z = cosine * RAY_PITCH_COS;
    camera->right.x = cosine;
    camera->right.y = 0.0f;
    camera->right.z = -sine;
    camera->up.x = -sine * RAY_PITCH_SIN;
    camera->up.y = RAY_PITCH_COS;
    camera->up.z = -cosine * RAY_PITCH_SIN;
}

void RayCamera_Init(RayCamera *camera)
{
    if (camera == NULL) return;
    camera->position.x = 0.0f;
    camera->position.y = 1.45f;
    camera->position.z = -5.5f;
    camera->yaw = 0.0f;
    camera_basis(camera);
}

static float wrap_yaw(float yaw)
{
    yaw = fmodf(yaw, RAY_TWO_PI);
    if (yaw > RAY_PI) yaw -= RAY_TWO_PI;
    if (yaw < -RAY_PI) yaw += RAY_TWO_PI;
    return yaw;
}

static int position_in_bounds(RayVec3 position)
{
    return finite_vector(position) &&
           position.x >= RAY_CAMERA_MIN_X &&
           position.x <= RAY_CAMERA_MAX_X &&
           position.z >= RAY_CAMERA_MIN_Z &&
           position.z <= RAY_CAMERA_MAX_Z &&
           position.y > RAY_ORIGIN_OFFSET && position.y <= 8.0f;
}

static int clear_movement(RayVec3 from, RayVec3 to)
{
    float dx = to.x - from.x;
    float dz = to.z - from.z;
    float length_squared = dx * dx + dz * dz;
    unsigned i;
    for (i = 0; i < RAY_SPHERE_COUNT; ++i) {
        float x = from.x - Ray_Spheres[i].center.x;
        float z = from.z - Ray_Spheres[i].center.z;
        float fraction = 0.0f;
        float radius = Ray_Spheres[i].radius + RAY_CAMERA_RADIUS;
        if (length_squared > 0.0f)
            fraction = clamp_unit(-(x * dx + z * dz) / length_squared);
        x += fraction * dx;
        z += fraction * dz;
        /* Conservative horizontal clearance also protects the camera near
           a sphere's top and prevents a large step passing through it. */
        if (x * x + z * z <= radius * radius) return 0;
    }
    return 1;
}

int RayCamera_Move(RayCamera *camera, float distance, float turn)
{
    RayVec3 candidate;
    float yaw;
    int changed = 0;
    if (camera == NULL || !finite_float(distance) || !finite_float(turn) ||
        !finite_float(camera->yaw) || !position_in_bounds(camera->position))
        return 0;

    /* Reduce each operand before adding, so even a very large finite turn
       cannot overflow the yaw sum. Turning is allowed beside an obstacle. */
    yaw = wrap_yaw(wrap_yaw(camera->yaw) + wrap_yaw(turn));
    if (yaw != camera->yaw) changed = 1;
    camera->yaw = yaw;
    camera_basis(camera);

    if (distance != 0.0f) {
        candidate = camera->position;
        candidate.x += sinf(yaw) * distance;
        candidate.z += cosf(yaw) * distance;
        if (position_in_bounds(candidate) &&
            clear_movement(camera->position, candidate) &&
            (candidate.x != camera->position.x ||
             candidate.z != camera->position.z)) {
            camera->position = candidate;
            changed = 1;
        }
    }
    return changed;
}

uint8_t Ray_MapGray(float luminance, unsigned contrast)
{
    static const float gains[3] = {1.0f, 1.25f, 1.60f};
    if (contrast > 2u) contrast = 2u;
    luminance = clamp_unit(luminance);
    luminance = clamp_unit(0.5f + (luminance - 0.5f) * gains[contrast]);
    return (uint8_t)(luminance * 255.0f + 0.5f);
}

/* The direction is unit length. Use the nearer positive root, or the exit
   root if the origin is inside a sphere. Each actual test is counted once. */
static float sphere_distance(RayVec3 origin, RayVec3 direction,
                             unsigned index, RayStats *stats)
{
    RayVec3 offset = vec_subtract(origin, Ray_Spheres[index].center);
    float b = vec_dot(offset, direction);
    float c = vec_dot(offset, offset) -
              Ray_Spheres[index].radius * Ray_Spheres[index].radius;
    float discriminant = b * b - c;
    float root, distance;
    if (stats != NULL) ++stats->sphere_tests;
    if (discriminant < 0.0f) return FLT_MAX;
    root = sqrtf(discriminant);
    distance = -b - root;
    if (distance <= RAY_MIN_T) distance = -b + root;
    return distance > RAY_MIN_T ? distance : FLT_MAX;
}

static float plane_distance(RayVec3 origin, RayVec3 direction,
                            RayStats *stats)
{
    float distance;
    if (stats != NULL) ++stats->plane_tests;
    if (direction.y == 0.0f) return FLT_MAX;
    distance = -origin.y / direction.y;
    return finite_float(distance) && distance > RAY_MIN_T ?
           distance : FLT_MAX;
}

static int nearest_hit(RayVec3 origin, RayVec3 direction, RayHit *hit,
                       RayStats *stats)
{
    uint32_t start = timer_start(stats);
    float nearest = FLT_MAX;
    int object = -2;
    unsigned i;
    for (i = 0; i < RAY_SPHERE_COUNT; ++i) {
        float distance = sphere_distance(origin, direction, i, stats);
        if (distance < nearest) {
            nearest = distance;
            object = (int)i;
        }
    }
    {
        float distance = plane_distance(origin, direction, stats);
        if (distance < nearest) {
            nearest = distance;
            object = -1;
        }
    }
    if (object != -2) {
        hit->point = vec_add(origin, vec_scale(direction, nearest));
        if (!finite_vector(hit->point)) {
            object = -2;
        } else {
            hit->object = object;
            if (object >= 0) {
                hit->normal = vec_scale(
                    vec_subtract(hit->point, Ray_Spheres[object].center),
                    sphere_inverse_radius[object]);
            } else {
                RayVec3 up = {0.0f, 1.0f, 0.0f};
                hit->normal = up;
            }
            if (stats != NULL) ++stats->hits;
        }
    }
    timer_finish(stats != NULL ? &stats->intersect_us : NULL, start);
    return object != -2;
}

static int shadowed(RayVec3 origin, RayStats *stats)
{
    uint32_t start = timer_start(stats);
    unsigned i;
    if (stats != NULL) ++stats->shadow_rays;
    for (i = 0; i < RAY_SPHERE_COUNT; ++i) {
        if (sphere_distance(origin, Ray_LightDirection, i, stats) < FLT_MAX) {
            timer_finish(stats != NULL ? &stats->shadow_us : NULL, start);
            return 1; /* Any-hit traversal stops on its first blocker. */
        }
    }
    if (plane_distance(origin, Ray_LightDirection, stats) < FLT_MAX) {
        timer_finish(stats != NULL ? &stats->shadow_us : NULL, start);
        return 1;
    }
    timer_finish(stats != NULL ? &stats->shadow_us : NULL, start);
    return 0;
}

static unsigned checker_parity(float x, float z)
{
    /* All ordinary views use the integer path. The infinite plane may be
       hit very far away by a near-horizontal ray; avoid out-of-range casts
       there. Float precision eventually limits the visible checker size. */
    if (x >= -1048576.0f && x <= 1048576.0f &&
        z >= -1048576.0f && z <= 1048576.0f) {
        int ix = (int)x;
        int iz = (int)z;
        if ((float)ix > x) --ix;
        if ((float)iz > z) --iz;
        return (unsigned)(ix + iz) & 1u;
    }
    /* Reduce the two cells independently to prevent the sum overflowing. */
    {
        float parity = fmodf(fmodf(floorf(x), 2.0f) +
                             fmodf(floorf(z), 2.0f), 2.0f);
        return parity == 0.0f ? 0u : 1u;
    }
}

static float sky_luminance(RayVec3 direction)
{
    return 0.22f + 0.34f * clamp_unit(direction.y * 0.75f + 0.50f);
}

static float shade_hit(const RayHit *hit, RayVec3 direction, RayStats *stats)
{
    uint32_t start = timer_start(stats);
    float material, specular;
    float n_dot_l = clamp_unit(vec_dot(hit->normal, Ray_LightDirection));
    float luminance;
    if (hit->object >= 0) {
        material = Ray_Spheres[hit->object].diffuse;
        specular = Ray_Spheres[hit->object].specular;
    } else {
        /* At grazing angles one screen pixel spans many checker cells. A
         * cheap directional fade to their average avoids horizon moire and
         * the expensive huge-coordinate parity path in ordinary views.
         * This is a display filter, not full ray differentials or AA. */
        const float checker_weight = clamp_unit((fabsf(direction.y) - 0.03f) * (1.0f / 0.09f));
        material = 0.48f;
        if (checker_weight > 0.0f) {
            const float cell = checker_parity(hit->point.x, hit->point.z) ? 0.24f : 0.72f;
            material += (cell - 0.48f) * checker_weight;
        }
        specular = 0.0f;
    }
    luminance = material * 0.18f;
    if (n_dot_l > 0.0f) {
        RayVec3 shadow_origin = vec_add(
            hit->point, vec_scale(hit->normal, RAY_ORIGIN_OFFSET));
        if (!shadowed(shadow_origin, stats)) {
            luminance += material * 0.74f * n_dot_l;
            if (specular > 0.0f) {
                RayVec3 reflected_light = vec_subtract(
                    vec_scale(hit->normal, 2.0f * n_dot_l),
                    Ray_LightDirection);
                float highlight = clamp_unit(
                    -vec_dot(reflected_light, direction));
                /* Fixed exponent 16: four multiplies, no general powf. */
                highlight *= highlight;
                highlight *= highlight;
                highlight *= highlight;
                highlight *= highlight;
                luminance += specular * highlight;
            }
        }
    }
    /* shade_us includes its shadow_us interval. Categories are inclusive
       profiling views and must not be added to obtain wall time. */
    timer_finish(stats != NULL ? &stats->shade_us : NULL, start);
    return luminance;
}

uint8_t Ray_TracePixel(const RayCamera *camera, unsigned x, unsigned y,
                      unsigned contrast, RayStats *stats)
{
    RayVec3 direction;
    RayHit hit;
    uint32_t start;
    float sx, sy, luminance;
    if (camera == NULL || x >= RAY_WIDTH || y >= RAY_HEIGHT ||
        !position_in_bounds(camera->position) ||
        !finite_vector(camera->forward) || !finite_vector(camera->right) ||
        !finite_vector(camera->up))
        return 0u;

    start = timer_start(stats);
    sx = ((float)x + 0.5f - (float)RAY_WIDTH * 0.5f) * RAY_SCREEN_SCALE;
    sy = ((float)RAY_HEIGHT * 0.5f - (float)y - 0.5f) * RAY_SCREEN_SCALE;
    direction = vec_add(camera->forward,
                vec_add(vec_scale(camera->right, sx),
                        vec_scale(camera->up, sy)));
    if (!vec_normalize(&direction)) {
        timer_finish(stats != NULL ? &stats->camera_us : NULL, start);
        return 0u;
    }
    timer_finish(stats != NULL ? &stats->camera_us : NULL, start);
    /* A pixel emits one primary ray; secondary tracing never calls this
       entry point and therefore cannot increase primary_rays again. */
    if (stats != NULL) ++stats->primary_rays;

    if (!nearest_hit(camera->position, direction, &hit, stats)) {
        luminance = sky_luminance(direction);
    } else {
        luminance = shade_hit(&hit, direction, stats);
        if (hit.object == 1) {
            RayVec3 reflected_direction = vec_subtract(
                direction,
                vec_scale(hit.normal, 2.0f * vec_dot(direction, hit.normal)));
            RayVec3 reflected_origin = vec_add(
                hit.point, vec_scale(hit.normal, RAY_ORIGIN_OFFSET));
            RayHit reflected_hit;
            float reflected_luminance;
            start = timer_start(stats);
            if (vec_normalize(&reflected_direction)) {
                if (stats != NULL) ++stats->reflection_rays;
                if (nearest_hit(reflected_origin, reflected_direction,
                                &reflected_hit, stats)) {
                    /* Exactly one bounce: shade the secondary hit directly
                       even if it happens to be the mirror sphere again. */
                    reflected_luminance = shade_hit(
                        &reflected_hit, reflected_direction, stats);
                } else {
                    reflected_luminance = sky_luminance(reflected_direction);
                }
                luminance = luminance * (1.0f - Ray_Spheres[1].reflectivity) +
                            reflected_luminance * Ray_Spheres[1].reflectivity;
            }
            /* reflection_us includes this secondary ray's intersect_us,
               shade_us and shadow_us. It is intentionally an inclusive
               timer; summing timing categories would count them twice. */
            timer_finish(stats != NULL ? &stats->reflection_us : NULL, start);
        }
    }
    return Ray_MapGray(luminance, contrast);
}
