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
    float yaw, pitch, roll;
} RayCamera;
enum { RAY_PORCELAIN=0u, RAY_CHROME=1u, RAY_GLASS=2u };
typedef struct {
    RayVec3 center;
    float radius, diffuse, reflectivity, specular;
    unsigned material;
    float ior, transmission;
} RaySphere;
typedef struct {
    uint32_t primary_rays, reflection_rays, shadow_rays;
    uint32_t sphere_tests, plane_tests, hits;
    uint32_t camera_us, intersect_us, shadow_us, shade_us, reflection_us;
    uint32_t preview_triangles, preview_pixels;
    uint32_t refraction_rays, glass_exits, tir_events, floor_reflection_rays;
    uint32_t refraction_us;
} RayStats;
/* Timings are inclusive: shade_us includes shadow_us; reflection_us includes
 * the secondary intersection/shade/shadow work. refraction_us includes the
 * internal exit and transmitted terminal ray. Never sum them as total.
 * refraction_rays counts each emitted inside/outside segment; floor reflections
 * are also included in reflection_rays. Secondary rays never branch. */
extern const RaySphere Ray_Spheres[RAY_SPHERE_COUNT];
extern const RayVec3 Ray_LightDirection;
void Ray_SetClock(uint32_t (*clock_us)(void));
uint32_t Ray_Clock(void);
void RayCamera_Init(RayCamera *camera);
int RayCamera_Move(RayCamera *camera, float distance, float turn);
/* Vertical movement saturates at heights 0.20..8.0. A swept camera radius
 * of 0.18 protects against sphere penetration; invalid inputs change nothing. */
int RayCamera_Elevate(RayCamera *camera, float delta);
/* Angles are radians. Pitch saturates at +/-80 degrees; yaw and roll wrap
 * into [-pi,pi]. Invalid input returns zero without changing the camera. */
int RayCamera_Orient(RayCamera *camera, float yaw_delta, float pitch_delta,
                     float roll_delta);
uint8_t Ray_TracePixel(const RayCamera *camera, unsigned x, unsigned y,
                      unsigned contrast, RayStats *stats);
void RayPreview_Render(uint8_t *pixels, const RayCamera *camera,
                       unsigned contrast, RayStats *stats);
uint8_t Ray_MapGray(float luminance, unsigned contrast);
/* Unit incident and normal, with the normal opposing the incident direction.
 * eta is the incident/transmitted IOR ratio. TIR or invalid input returns zero
 * and leaves out unchanged; success writes a finite unit transmitted ray. */
int Ray_RefractDirection(RayVec3 incident, RayVec3 normal, float eta,
                        RayVec3 *out);
/* Pure shading functions shared with the polygon preview; neither traces rays. */
float Ray_EnvironmentLuminance(RayVec3 direction);
float Ray_SurfaceDiffuse(const RaySphere *sphere, RayVec3 point);
#endif
