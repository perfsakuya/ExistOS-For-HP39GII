#include "RayCore.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

/* This scene and camera are shared with the polygon preview. */
const RaySphere Ray_Spheres[RAY_SPHERE_COUNT] = {
    {{-1.70f, 0.90f, 1.10f}, 0.90f, 0.10f, 0.04f, 0.70f,
     RAY_GLASS, 1.50f, 0.94f},
    {{ 0.15f, 1.08f, 2.10f}, 1.08f, 0.42f, 0.86f, 0.55f,
     RAY_CHROME, 1.00f, 0.00f},
    {{ 1.90f, 0.78f, 0.60f}, 0.78f, 0.88f, 0.00f, 0.45f,
     RAY_PORCELAIN, 1.00f, 0.00f}
};

/* Unit vector from a surface towards the directional light. */
const RayVec3 Ray_LightDirection = {
    -0.43193421f, 0.86386843f, -0.25916053f
};

/* Two fixed samples of a broad directional studio light. Sampling is
   deterministic and requires no per-frame noise or accumulation buffer. */
static const RayVec3 ray_light_samples[2] = {
    {-0.514023072f, 0.834780497f, -0.197286095f},
    {-0.339638703f, 0.883919597f, -0.321452480f}
};

static const float sphere_inverse_radius[RAY_SPHERE_COUNT] = {
    1.11111111f, 0.92592593f, 1.28205128f
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

int Ray_RefractDirection(RayVec3 incident, RayVec3 normal, float eta,
                        RayVec3 *out)
{
    float incident_length, normal_length, cosine, eta_squared, k;
    RayVec3 transmitted;
    if (out == NULL || !finite_vector(incident) || !finite_vector(normal) ||
        !finite_float(eta) || eta <= 0.0f)
        return 0;
    incident_length = vec_dot(incident, incident);
    normal_length = vec_dot(normal, normal);
    if (!finite_float(incident_length) || !finite_float(normal_length) ||
        fabsf(incident_length - 1.0f) > 0.001f ||
        fabsf(normal_length - 1.0f) > 0.001f)
        return 0;
    cosine = -vec_dot(incident, normal);
    if (cosine < -0.00001f) return 0;
    cosine = clamp_unit(cosine);
    eta_squared = eta * eta;
    if (!finite_float(eta_squared)) return 0;
    k = 1.0f - eta_squared * (1.0f - cosine * cosine);
    /* Do not manufacture a transmitted direction for total reflection. */
    if (!finite_float(k) || k < 0.0f) return 0;
    transmitted = vec_add(vec_scale(incident, eta),
                  vec_scale(normal, eta * cosine - sqrtf(k)));
    if (!vec_normalize(&transmitted)) return 0;
    *out = transmitted;
    return 1;
}

static float fresnel_schlick(float cosine, float normal_reflectance)
{
    float edge = 1.0f - clamp_unit(cosine);
    float edge_squared = edge * edge;
    return normal_reflectance + (1.0f - normal_reflectance) *
           edge_squared * edge_squared * edge;
}

float Ray_EnvironmentLuminance(RayVec3 direction)
{
    float luminance;
    if (!finite_vector(direction)) return 0.0f;
    luminance = 0.16f + 0.38f * clamp_unit(direction.y * 0.75f + 0.50f);
    /* Analytic softboxes on the environment behind the default camera:
       they become broad bright strips in curved chrome/glass reflections. */
    if (direction.z < -0.20f && direction.y > 0.08f &&
        direction.y < 0.82f && fabsf(direction.x + 0.32f) < 0.10f)
        luminance = 1.05f;
    if (direction.z < -0.12f && direction.y > 0.36f &&
        direction.y < 0.85f && fabsf(direction.x - 0.38f) < 0.15f)
        luminance = 0.92f;
    if (direction.z < -0.30f && direction.y > 0.62f &&
        direction.y < 0.75f && fabsf(direction.x) < 0.80f)
        luminance = 1.00f;
    return luminance;
}

float Ray_SurfaceDiffuse(const RaySphere *sphere, RayVec3 point)
{
    float local_height;
    unsigned stripe;
    if (sphere == NULL || !finite_vector(point) ||
        !finite_float(sphere->diffuse) || !finite_float(sphere->radius) ||
        sphere->radius <= 0.0f || !finite_vector(sphere->center))
        return 0.0f;
    if (sphere->material != RAY_PORCELAIN) return sphere->diffuse;
    local_height = (point.y - sphere->center.y) / sphere->radius;
    stripe = (unsigned)(clamp_unit(local_height * 0.5f + 0.5f) * 8.0f);
    return sphere->diffuse * ((stripe & 1u) ? 0.42f : 1.0f);
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

static float shadow_visibility(RayVec3 origin, RayVec3 light, RayStats *stats)
{
    uint32_t start = timer_start(stats);
    float visibility = 1.0f;
    unsigned i;
    if (stats != NULL) ++stats->shadow_rays;
    for (i = 0; i < RAY_SPHERE_COUNT; ++i) {
        if (sphere_distance(origin, light, i, stats) < FLT_MAX) {
            if (Ray_Spheres[i].material == RAY_GLASS) {
                /* A cheap transmitted shadow, without tracing caustics. */
                visibility *= 0.55f;
            } else {
                timer_finish(stats != NULL ? &stats->shadow_us : NULL, start);
                return 0.0f; /* Opaque any-hit traversal still stops early. */
            }
        }
    }
    if (plane_distance(origin, light, stats) < FLT_MAX) {
        timer_finish(stats != NULL ? &stats->shadow_us : NULL, start);
        return 0.0f;
    }
    timer_finish(stats != NULL ? &stats->shadow_us : NULL, start);
    return visibility;
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

static float shade_hit(const RayHit *hit, RayVec3 direction, RayStats *stats)
{
    uint32_t start = timer_start(stats);
    float material, specular;
    float luminance;
    unsigned sample;
    if (hit->object >= 0) {
        material = Ray_SurfaceDiffuse(&Ray_Spheres[hit->object], hit->point);
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
    luminance = material * 0.16f;
    for (sample = 0u; sample < 2u; ++sample) {
        RayVec3 light = ray_light_samples[sample];
        float n_dot_l = clamp_unit(vec_dot(hit->normal, light));
        if (n_dot_l > 0.0f) {
            RayVec3 shadow_origin = vec_add(
                hit->point, vec_scale(hit->normal, RAY_ORIGIN_OFFSET));
            float visibility = shadow_visibility(shadow_origin, light, stats);
            if (visibility > 0.0f) {
                luminance += material * 0.38f * n_dot_l * visibility;
                if (specular > 0.0f) {
                    RayVec3 reflected_light = vec_subtract(
                        vec_scale(hit->normal, 2.0f * n_dot_l), light);
                    float highlight = clamp_unit(
                        -vec_dot(reflected_light, direction));
                    /* Fixed exponent 16: four multiplies, no general powf. */
                    highlight *= highlight;
                    highlight *= highlight;
                    highlight *= highlight;
                    highlight *= highlight;
                    luminance += 0.5f * specular * highlight * visibility;
                }
            }
        }
    }
    /* shade_us includes its shadow_us interval. Categories are inclusive
       profiling views and must not be added to obtain wall time. */
    timer_finish(stats != NULL ? &stats->shade_us : NULL, start);
    return luminance;
}

/* A terminal ray performs one scene query and direct shading. It never
   reflects or refracts again, regardless of the material it encounters. */
static float trace_terminal(RayVec3 origin, RayVec3 direction, RayStats *stats)
{
    RayHit hit;
    if (nearest_hit(origin, direction, &hit, stats))
        return shade_hit(&hit, direction, stats);
    return Ray_EnvironmentLuminance(direction);
}

static float trace_reflection(const RayHit *hit, RayVec3 direction,
                              int floor_reflection, RayStats *stats)
{
    uint32_t start = timer_start(stats);
    RayVec3 reflected = vec_subtract(direction,
                        vec_scale(hit->normal,
                                  2.0f * vec_dot(direction, hit->normal)));
    float luminance = Ray_EnvironmentLuminance(direction);
    if (vec_normalize(&reflected)) {
        float side = vec_dot(reflected, hit->normal) >= 0.0f ? 1.0f : -1.0f;
        RayVec3 origin = vec_add(hit->point,
                        vec_scale(hit->normal, side * RAY_ORIGIN_OFFSET));
        if (stats != NULL) {
            ++stats->reflection_rays;
            if (floor_reflection) ++stats->floor_reflection_rays;
        }
        luminance = trace_terminal(origin, reflected, stats);
    }
    /* Inclusive: terminal intersection, shading and its shadow samples. */
    timer_finish(stats != NULL ? &stats->reflection_us : NULL, start);
    return luminance;
}

static int transmit_glass(const RayHit *hit, RayVec3 direction,
                          float *luminance, RayStats *stats)
{
    const RaySphere *sphere = &Ray_Spheres[hit->object];
    uint32_t start = timer_start(stats);
    RayVec3 outgoing, exit_point, exit_normal;
    float chord = 0.0f;
    int success = 0;
    if (vec_dot(direction, hit->normal) <= 0.0f) {
        RayVec3 inside;
        if (!Ray_RefractDirection(direction, hit->normal,
                                  1.0f / sphere->ior, &inside))
            goto finish;
        if (stats != NULL) ++stats->refraction_rays;

        /* Starting on a sphere and travelling inward, the other root is
           exactly -2 dot(P-C,D). This traces the interior of this sphere
           directly, without querying the whole scene or allocating a path. */
        chord = -2.0f * vec_dot(vec_subtract(hit->point, sphere->center), inside);
        if (stats != NULL) ++stats->sphere_tests;
        if (!finite_float(chord) || chord <= RAY_MIN_T) goto finish;
        exit_point = vec_add(hit->point, vec_scale(inside, chord));
        if (!finite_vector(exit_point)) goto finish;
        exit_normal = vec_scale(vec_subtract(exit_point, sphere->center),
                                sphere_inverse_radius[hit->object]);
        if (stats != NULL) ++stats->glass_exits;
        if (!Ray_RefractDirection(inside, vec_scale(exit_normal, -1.0f),
                                  sphere->ior, &outgoing)) {
            if (stats != NULL) ++stats->tir_events;
            goto finish;
        }
    } else {
        /* The public camera API may be used with an origin inside glass.
           Its first surface is already the exit, so apply glass-to-air
           Snell directly. TIR produces no invented transmitted direction. */
        exit_point = hit->point;
        exit_normal = hit->normal;
        if (stats != NULL) ++stats->glass_exits;
        if (!Ray_RefractDirection(direction, vec_scale(exit_normal, -1.0f),
                                  sphere->ior, &outgoing)) {
            if (stats != NULL) ++stats->tir_events;
            goto finish;
        }
    }
    if (stats != NULL) ++stats->refraction_rays;
    exit_point = vec_add(exit_point,
                        vec_scale(exit_normal, RAY_ORIGIN_OFFSET));
    /* Mild, bounded attenuation with path length; no expensive expf and
       no stored floating-point frame. The secondary ray terminates below. */
    *luminance = trace_terminal(exit_point, outgoing, stats) *
                 sphere->transmission / (1.0f + 0.065f * chord);
    success = 1;
finish:
    /* Inclusive: entry/exit math and transmitted terminal shading. The
       separate exterior reflection branch belongs to reflection_us. */
    timer_finish(stats != NULL ? &stats->refraction_us : NULL, start);
    return success;
}

static float floor_reflection_weight(const RayHit *hit, RayVec3 direction)
{
    float side_fade = clamp_unit((3.6f - fabsf(hit->point.x)) / 0.9f);
    float front_fade = clamp_unit((hit->point.z + 1.5f) / 1.0f);
    float back_fade = clamp_unit((4.0f - hit->point.z) / 1.0f);
    float fresnel = fresnel_schlick(fabsf(direction.y), 0.12f);
    /* Only the near studio patch reflects. Distant checkerboard rays
       remain direct-shaded, bounding the secondary-ray workload. */
    return 0.18f * side_fade * front_fade * back_fade *
           (0.65f + 0.35f * fresnel);
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
        luminance = Ray_EnvironmentLuminance(direction);
    } else {
        luminance = shade_hit(&hit, direction, stats);
        if (hit.object >= 0) {
            const RaySphere *sphere = &Ray_Spheres[hit.object];
            float fresnel = fresnel_schlick(
                fabsf(vec_dot(direction, hit.normal)), sphere->reflectivity);
            if (sphere->material == RAY_GLASS) {
                float reflected = trace_reflection(&hit, direction, 0, stats);
                float transmitted = 0.0f;
                if (!transmit_glass(&hit, direction, &transmitted, stats))
                    fresnel = 1.0f;
                /* Two independent primary branches; every secondary hit
                   terminates at direct shading. On TIR only the reflected
                   branch contributes, with no fake refracted direction. */
                luminance = reflected * fresnel + transmitted * (1.0f - fresnel)
                            + 0.015f * luminance;
            } else if (sphere->material == RAY_CHROME) {
                float reflected = trace_reflection(&hit, direction, 0, stats);
                luminance = luminance * (1.0f - fresnel) + reflected * fresnel;
            }
        } else {
            float weight = floor_reflection_weight(&hit, direction);
            if (weight > 0.001f) {
                float reflected = trace_reflection(&hit, direction, 1, stats);
                luminance = luminance * (1.0f - weight) + reflected * weight;
            }
        }
    }
    return Ray_MapGray(luminance, contrast);
}
