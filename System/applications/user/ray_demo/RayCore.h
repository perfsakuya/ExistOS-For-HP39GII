#ifndef SKYOS_RAY_CORE_H
#define SKYOS_RAY_CORE_H
#include <stdint.h>
#define RAY_WIDTH 256u
#define RAY_HEIGHT 127u
#define RAY_PIXELS (RAY_WIDTH * RAY_HEIGHT)
#define RAY_SPHERE_COUNT 3u
typedef struct { float x, y, z; } RayVec3;
typedef struct {
    RayVec3 position, forward, right, up;
    float yaw;
} RayCamera;
typedef struct {
    RayVec3 center;
    float radius, diffuse, reflectivity, specular;
} RaySphere;
typedef struct {
    uint32_t primary_rays, reflection_rays, shadow_rays;
    uint32_t sphere_tests, plane_tests, hits;
    uint32_t camera_us, intersect_us, shadow_us, shade_us, reflection_us;
    uint32_t preview_triangles, preview_pixels;
} RayStats;
/* Timings are inclusive: shade_us includes shadow_us; reflection_us includes
 * the secondary intersection/shade/shadow work. Never sum them as total. */
extern const RaySphere Ray_Spheres[RAY_SPHERE_COUNT];
extern const RayVec3 Ray_LightDirection;
void Ray_SetClock(uint32_t (*clock_us)(void));
uint32_t Ray_Clock(void);
void RayCamera_Init(RayCamera *camera);
int RayCamera_Move(RayCamera *camera, float distance, float turn);
uint8_t Ray_TracePixel(const RayCamera *camera, unsigned x, unsigned y,
                      unsigned contrast, RayStats *stats);
void RayPreview_Render(uint8_t *pixels, const RayCamera *camera,
                       unsigned contrast, RayStats *stats);
uint8_t Ray_MapGray(float luminance, unsigned contrast);
#endif
