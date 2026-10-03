#include "RayCore.h"

#include <math.h>
#include <stddef.h>

/* This renderer contains only polygon projection and scan conversion. */
#define PREVIEW_NEAR 0.08f
#define PREVIEW_FOCAL (128.0f / 0.70f)
#define PREVIEW_HALF_WIDTH 128.0f
#define PREVIEW_HALF_HEIGHT 63.5f
#define PREVIEW_X_SLOPE 0.70f
#define PREVIEW_Y_SLOPE (63.5f / PREVIEW_FOCAL)
#define PREVIEW_CLIP_VERTICES 10u
#define PREVIEW_FLOOR_RADIUS 12
#define PREVIEW_FLOOR_EXTENT 1024.0f
#define PREVIEW_POSITION_LIMIT 1000000.0f
#define PREVIEW_VIEW_LIMIT 1000000000.0f
#define PREVIEW_LONGITUDES 12u
#define PREVIEW_LATITUDES 6u

typedef struct { float x, y, z; } PreviewVertex;
typedef struct { float x, y; } PreviewPoint;
typedef struct {
    uint8_t *pixels;
    const RayCamera *camera;
    RayStats *stats;
    unsigned contrast;
} PreviewContext;

static const float preview_ring_y[PREVIEW_LATITUDES + 1u] = {
    -1.0f, -0.86602540f, -0.5f, 0.0f, 0.5f, 0.86602540f, 1.0f
};
static const float preview_ring_radius[PREVIEW_LATITUDES + 1u] = {
    0.0f, 0.5f, 0.86602540f, 1.0f, 0.86602540f, 0.5f, 0.0f
};
static const float preview_longitude_cos[PREVIEW_LONGITUDES] = {
    1.0f, 0.86602540f, 0.5f, 0.0f, -0.5f, -0.86602540f,
    -1.0f, -0.86602540f, -0.5f, 0.0f, 0.5f, 0.86602540f
};
static const float preview_longitude_sin[PREVIEW_LONGITUDES] = {
    0.0f, 0.5f, 0.86602540f, 1.0f, 0.86602540f, 0.5f,
    0.0f, -0.5f, -0.86602540f, -1.0f, -0.86602540f, -0.5f
};

static float preview_clamp(float value, float low, float high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static float preview_dot(RayVec3 a, RayVec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static RayVec3 preview_subtract(RayVec3 a, RayVec3 b)
{
    RayVec3 result = {a.x - b.x, a.y - b.y, a.z - b.z};
    return result;
}

static int preview_finite_vector(RayVec3 value, float limit)
{
    return isfinite(value.x) && isfinite(value.y) && isfinite(value.z)
        && fabsf(value.x) <= limit && fabsf(value.y) <= limit
        && fabsf(value.z) <= limit;
}

static int preview_valid_camera(const RayCamera *camera)
{
    return preview_finite_vector(camera->position, PREVIEW_POSITION_LIMIT)
        && preview_finite_vector(camera->forward, 4.0f)
        && preview_finite_vector(camera->right, 4.0f)
        && preview_finite_vector(camera->up, 4.0f);
}

static int preview_to_view(const RayCamera *camera, RayVec3 point,
                           PreviewVertex *view)
{
    RayVec3 offset = preview_subtract(point, camera->position);
    view->x = preview_dot(offset, camera->right);
    view->y = preview_dot(offset, camera->up);
    view->z = preview_dot(offset, camera->forward);
    return isfinite(view->x) && isfinite(view->y) && isfinite(view->z)
        && fabsf(view->x) <= PREVIEW_VIEW_LIMIT
        && fabsf(view->y) <= PREVIEW_VIEW_LIMIT
        && fabsf(view->z) <= PREVIEW_VIEW_LIMIT;
}

static float preview_clip_distance(PreviewVertex vertex, unsigned plane)
{
    switch (plane) {
    case 0u: return vertex.z - PREVIEW_NEAR;
    case 1u: return vertex.x + vertex.z * PREVIEW_X_SLOPE;
    case 2u: return vertex.z * PREVIEW_X_SLOPE - vertex.x;
    case 3u: return vertex.y + vertex.z * PREVIEW_Y_SLOPE;
    default: return vertex.z * PREVIEW_Y_SLOPE - vertex.y;
    }
}

static unsigned preview_clip_polygon(const PreviewVertex *input,
                                     unsigned count,
                                     PreviewVertex *output, unsigned plane)
{
    unsigned i, output_count = 0u;
    PreviewVertex previous = input[count - 1u];
    float previous_distance = preview_clip_distance(previous, plane);
    int previous_inside = previous_distance >= 0.0f;

    for (i = 0u; i < count; ++i) {
        PreviewVertex current = input[i];
        float distance = preview_clip_distance(current, plane);
        int inside = distance >= 0.0f;
        if (inside != previous_inside) {
            float fraction = previous_distance / (previous_distance - distance);
            PreviewVertex intersection;
            fraction = preview_clamp(fraction, 0.0f, 1.0f);
            intersection.x = previous.x + (current.x - previous.x) * fraction;
            intersection.y = previous.y + (current.y - previous.y) * fraction;
            intersection.z = previous.z + (current.z - previous.z) * fraction;
            if (output_count >= PREVIEW_CLIP_VERTICES) return 0u;
            output[output_count++] = intersection;
        }
        if (inside) {
            if (output_count >= PREVIEW_CLIP_VERTICES) return 0u;
            output[output_count++] = current;
        }
        previous = current;
        previous_distance = distance;
        previous_inside = inside;
    }
    return output_count;
}

static void preview_raster_triangle(PreviewContext *context,
                                    PreviewPoint a, PreviewPoint b,
                                    PreviewPoint c, uint8_t gray)
{
    PreviewPoint vertices[3] = {a, b, c};
    float min_y = fminf(a.y, fminf(b.y, c.y));
    float max_y = fmaxf(a.y, fmaxf(b.y, c.y));
    float area = (b.x - a.x) * (c.y - a.y)
               - (b.y - a.y) * (c.x - a.x);
    int row, first_row, last_row;
    int wrote_pixels = 0;

    if (fabsf(area) < 0.00001f) return;
    /* All vertices were frustum clipped; these casts stay within [-1, 127]. */
    first_row = (int)ceilf(min_y - 0.5f);
    last_row = (int)ceilf(max_y - 0.5f) - 1;
    if (first_row < 0) first_row = 0;
    if (last_row >= (int)RAY_HEIGHT) last_row = (int)RAY_HEIGHT - 1;

    for (row = first_row; row <= last_row; ++row) {
        float scan_y = (float)row + 0.5f;
        float min_x = (float)RAY_WIDTH, max_x = 0.0f;
        unsigned edge, intersections = 0u;
        int first_x, last_x, x;
        for (edge = 0u; edge < 3u; ++edge) {
            PreviewPoint p = vertices[edge];
            PreviewPoint q = vertices[(edge + 1u) % 3u];
            /* Half-open edges share their boundary without filling twice. */
            if ((scan_y >= p.y && scan_y < q.y)
                || (scan_y >= q.y && scan_y < p.y)) {
                float fraction = (scan_y - p.y) / (q.y - p.y);
                float edge_x = p.x + (q.x - p.x) * fraction;
                edge_x = preview_clamp(edge_x, 0.0f, (float)RAY_WIDTH);
                if (edge_x < min_x) min_x = edge_x;
                if (edge_x > max_x) max_x = edge_x;
                ++intersections;
            }
        }
        if (intersections < 2u) continue;
        first_x = (int)ceilf(min_x - 0.5f);
        last_x = (int)ceilf(max_x - 0.5f) - 1;
        if (first_x < 0) first_x = 0;
        if (last_x >= (int)RAY_WIDTH) last_x = (int)RAY_WIDTH - 1;
        if (first_x > last_x) continue;
        for (x = first_x; x <= last_x; ++x)
            context->pixels[(unsigned)row * RAY_WIDTH + (unsigned)x] = gray;
        if (context->stats != NULL)
            context->stats->preview_pixels += (uint32_t)(last_x - first_x + 1);
        wrote_pixels = 1;
    }
    if (wrote_pixels && context->stats != NULL)
        ++context->stats->preview_triangles;
}

static void preview_world_triangle(PreviewContext *context, RayVec3 a,
                                   RayVec3 b, RayVec3 c, uint8_t gray)
{
    PreviewVertex buffers[2][PREVIEW_CLIP_VERTICES];
    PreviewPoint projected[PREVIEW_CLIP_VERTICES];
    unsigned count = 3u, source = 0u, plane, i;

    if (!preview_to_view(context->camera, a, &buffers[0][0])
        || !preview_to_view(context->camera, b, &buffers[0][1])
        || !preview_to_view(context->camera, c, &buffers[0][2])) return;
    /* Camera-space clipping also bounds projection before any float-to-int cast. */
    for (plane = 0u; plane < 5u; ++plane) {
        count = preview_clip_polygon(buffers[source], count,
                                     buffers[1u - source], plane);
        if (count < 3u) return;
        source = 1u - source;
    }
    for (i = 0u; i < count; ++i) {
        PreviewVertex vertex = buffers[source][i];
        float inverse_z, screen_x, screen_y;
        if (!isfinite(vertex.z) || vertex.z < PREVIEW_NEAR * 0.99f) return;
        inverse_z = PREVIEW_FOCAL / vertex.z;
        screen_x = PREVIEW_HALF_WIDTH + vertex.x * inverse_z;
        screen_y = PREVIEW_HALF_HEIGHT - vertex.y * inverse_z;
        if (!isfinite(screen_x) || !isfinite(screen_y)) return;
        projected[i].x = preview_clamp(screen_x, 0.0f, (float)RAY_WIDTH);
        projected[i].y = preview_clamp(screen_y, 0.0f, (float)RAY_HEIGHT);
    }
    for (i = 1u; i + 1u < count; ++i)
        preview_raster_triangle(context, projected[0], projected[i],
                                projected[i + 1u], gray);
}

static void preview_floor(PreviewContext *context)
{
    const RayCamera *camera = context->camera;
    float light = 0.18f + 0.74f * fmaxf(Ray_LightDirection.y, 0.0f);
    uint8_t gray[2] = {Ray_MapGray(0.24f * light, context->contrast),
                       Ray_MapGray(0.72f * light, context->contrast)};
    RayVec3 a, b, c, d;
    int base_x = (int)floorf(camera->position.x);
    int base_z = (int)floorf(camera->position.z);
    int x, z;

    a = (RayVec3){camera->position.x - PREVIEW_FLOOR_EXTENT, 0.0f,
                 camera->position.z - PREVIEW_FLOOR_EXTENT};
    b = (RayVec3){camera->position.x + PREVIEW_FLOOR_EXTENT, 0.0f,
                 camera->position.z - PREVIEW_FLOOR_EXTENT};
    c = (RayVec3){camera->position.x + PREVIEW_FLOOR_EXTENT, 0.0f,
                 camera->position.z + PREVIEW_FLOOR_EXTENT};
    d = (RayVec3){camera->position.x - PREVIEW_FLOOR_EXTENT, 0.0f,
                 camera->position.z + PREVIEW_FLOOR_EXTENT};
    preview_world_triangle(context, a, b, c,
                           Ray_MapGray(0.48f * light, context->contrast));
    preview_world_triangle(context, a, c, d,
                           Ray_MapGray(0.48f * light, context->contrast));

    /* A finite near grid keeps movement cost bounded; the base plane continues it. */
    for (z = base_z - PREVIEW_FLOOR_RADIUS;
         z < base_z + PREVIEW_FLOOR_RADIUS; ++z) {
        for (x = base_x - PREVIEW_FLOOR_RADIUS;
             x < base_x + PREVIEW_FLOOR_RADIUS; ++x) {
            uint8_t cell_gray = gray[((x + z) & 1) == 0];
            a = (RayVec3){(float)x, 0.0f, (float)z};
            b = (RayVec3){(float)x + 1.0f, 0.0f, (float)z};
            c = (RayVec3){(float)x + 1.0f, 0.0f, (float)z + 1.0f};
            d = (RayVec3){(float)x, 0.0f, (float)z + 1.0f};
            preview_world_triangle(context, a, b, c, cell_gray);
            preview_world_triangle(context, a, c, d, cell_gray);
        }
    }
}

static RayVec3 preview_sphere_vertex(const RaySphere *sphere,
                                     unsigned ring, unsigned longitude)
{
    float radial = sphere->radius * preview_ring_radius[ring];
    RayVec3 point = {
        sphere->center.x + radial * preview_longitude_cos[longitude],
        sphere->center.y + sphere->radius * preview_ring_y[ring],
        sphere->center.z + radial * preview_longitude_sin[longitude]
    };
    return point;
}

static void preview_sphere_triangle(PreviewContext *context,
                                    const RaySphere *sphere,
                                    RayVec3 a, RayVec3 b, RayVec3 c,
                                    int inside_sphere)
{
    RayVec3 ab = preview_subtract(b, a), ac = preview_subtract(c, a);
    RayVec3 normal = {ab.y * ac.z - ab.z * ac.y,
                     ab.z * ac.x - ab.x * ac.z,
                     ab.x * ac.y - ab.y * ac.x};
    RayVec3 center = {(a.x + b.x + c.x) / 3.0f,
                      (a.y + b.y + c.y) / 3.0f,
                      (a.z + b.z + c.z) / 3.0f};
    RayVec3 toward_camera = preview_subtract(context->camera->position, center);
    float facing = preview_dot(normal, toward_camera);
    float normal_length, view_length, n_dot_l, specular = 0.0f, luminance;
    RayVec3 reflected;

    /* The mesh is convex, so its visible faces need no per-pixel depth buffer. */
    if ((!inside_sphere && facing <= 0.0f)
        || (inside_sphere && facing >= 0.0f)) return;
    normal_length = sqrtf(preview_dot(normal, normal));
    if (normal_length <= 0.00001f) return;
    if (inside_sphere) normal_length = -normal_length;
    normal.x /= normal_length;
    normal.y /= normal_length;
    normal.z /= normal_length;
    n_dot_l = preview_clamp(preview_dot(normal, Ray_LightDirection), 0.0f, 1.0f);
    view_length = sqrtf(preview_dot(toward_camera, toward_camera));
    if (n_dot_l > 0.0f && view_length > 0.00001f) {
        float highlight;
        reflected = (RayVec3){2.0f * n_dot_l * normal.x - Ray_LightDirection.x,
                              2.0f * n_dot_l * normal.y - Ray_LightDirection.y,
                              2.0f * n_dot_l * normal.z - Ray_LightDirection.z};
        highlight = preview_clamp(preview_dot(reflected, toward_camera)
                                   / view_length, 0.0f, 1.0f);
        highlight *= highlight;
        highlight *= highlight;
        highlight *= highlight;
        highlight *= highlight;
        specular = sphere->specular * highlight;
    }
    luminance = sphere->diffuse * (0.18f + 0.74f * n_dot_l);
    /* Bright gray stands in for the mirror material; no reflection is traced. */
    luminance = luminance * (1.0f - sphere->reflectivity)
              + sphere->reflectivity * (0.40f + 0.40f * n_dot_l) + specular;
    preview_world_triangle(context, a, b, c,
                           Ray_MapGray(luminance, context->contrast));
}

static void preview_sphere(PreviewContext *context, const RaySphere *sphere)
{
    RayVec3 offset = preview_subtract(context->camera->position, sphere->center);
    int inside_sphere = preview_dot(offset, offset) < sphere->radius * sphere->radius;
    unsigned ring, longitude;
    for (ring = 0u; ring < PREVIEW_LATITUDES; ++ring) {
        for (longitude = 0u; longitude < PREVIEW_LONGITUDES; ++longitude) {
            unsigned next = (longitude + 1u) % PREVIEW_LONGITUDES;
            RayVec3 a = preview_sphere_vertex(sphere, ring, longitude);
            RayVec3 b = preview_sphere_vertex(sphere, ring, next);
            RayVec3 c = preview_sphere_vertex(sphere, ring + 1u, next);
            RayVec3 d = preview_sphere_vertex(sphere, ring + 1u, longitude);
            if (ring + 1u < PREVIEW_LATITUDES)
                preview_sphere_triangle(context, sphere, a, d, c, inside_sphere);
            if (ring > 0u)
                preview_sphere_triangle(context, sphere, a, c, b, inside_sphere);
        }
    }
}

void RayPreview_Render(uint8_t *pixels, const RayCamera *camera,
                       unsigned contrast, RayStats *stats)
{
    PreviewContext context = {pixels, camera, stats, contrast};
    unsigned order[RAY_SPHERE_COUNT], i, j, y, x;
    float depth[RAY_SPHERE_COUNT];
    int valid_camera;

    if (stats != NULL) {
        stats->preview_triangles = 0u;
        stats->preview_pixels = 0u;
    }
    if (pixels == NULL || camera == NULL) return;
    valid_camera = preview_valid_camera(camera);
    for (y = 0u; y < RAY_HEIGHT; ++y) {
        float elevation = 0.0f;
        uint8_t gray;
        if (valid_camera) {
            float sy = (PREVIEW_HALF_HEIGHT - (float)y - 0.5f) / PREVIEW_FOCAL;
            elevation = (camera->forward.y + sy * camera->up.y)
                      / sqrtf(1.0f + sy * sy);
        }
        gray = Ray_MapGray(0.22f + 0.34f
                           * preview_clamp(elevation * 0.75f + 0.50f, 0.0f, 1.0f),
                           contrast);
        for (x = 0u; x < RAY_WIDTH; ++x) pixels[y * RAY_WIDTH + x] = gray;
    }
    if (stats != NULL) stats->preview_pixels = RAY_PIXELS;
    if (!valid_camera) return;

    preview_floor(&context);
    for (i = 0u; i < RAY_SPHERE_COUNT; ++i) {
        order[i] = i;
        depth[i] = preview_dot(preview_subtract(Ray_Spheres[i].center,
                                               camera->position), camera->forward);
    }
    /* Paint whole spheres from far to near after the ground. */
    for (i = 1u; i < RAY_SPHERE_COUNT; ++i) {
        unsigned current = order[i];
        j = i;
        while (j > 0u && depth[order[j - 1u]] < depth[current]) {
            order[j] = order[j - 1u];
            --j;
        }
        order[j] = current;
    }
    for (i = 0u; i < RAY_SPHERE_COUNT; ++i)
        preview_sphere(&context, &Ray_Spheres[order[i]]);
}
