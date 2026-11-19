// Shared by the Wear nodes: the pixel's world position and normal, value noise.
// Position and normal are macros: In and UBOLight are declared later, by the output node's include, and read where the macro is used (main).
#ifndef XMAT_WEAR_COMMON
#define XMAT_WEAR_COMMON
#if defined(XLION_LDR_OUTPUT)
#define XMAT_WORLD_POS (In.wSpacePosition.xyz)
#else
#define XMAT_WORLD_POS (In.wSpacePosition.xyz + UBOLight.CameraPos.xyz)
#endif
#define XMAT_WORLD_N normalize(In.Normal.xyz)
float xmat_hash13(vec3 p) { p = fract(p * 0.1031); p += dot(p, p.zyx + 31.32); return fract((p.x + p.y) * p.z); }
float xmat_vnoise(vec3 p)
{
    const vec3 i = floor(p); vec3 f = fract(p); f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(xmat_hash13(i),                   xmat_hash13(i + vec3(1.0, 0.0, 0.0)), f.x),
                   mix(xmat_hash13(i + vec3(0.0, 1.0, 0.0)), xmat_hash13(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),
               mix(mix(xmat_hash13(i + vec3(0.0, 0.0, 1.0)), xmat_hash13(i + vec3(1.0, 0.0, 1.0)), f.x),
                   mix(xmat_hash13(i + vec3(0.0, 1.0, 1.0)), xmat_hash13(i + vec3(1.0, 1.0, 1.0)), f.x), f.y), f.z);
}
float xmat_fbm3(vec3 p) { return 0.5 * xmat_vnoise(p) + 0.3 * xmat_vnoise(p * 2.03 + 17.1) + 0.2 * xmat_vnoise(p * 4.11 + 41.7); }
vec3  xmat_seed(float s) { return vec3(s * 127.1, s * 311.7, s * 74.7); }
#endif
