[TOP]<!#ifdef XLION_TILE_PIXEL
struct { vec4 wSpacePosition; vec4 ShadowPosition; vec4 VertColor; vec4 Tangent; vec3 Normal; vec2 UV; } In;
vec2 InUV1 = vec2(0.0);
#else
layout(location = 0) in struct { vec4 wSpacePosition; vec4 ShadowPosition; vec4 VertColor; vec4 Tangent; vec3 Normal; vec2 UV; } In;
layout(location = 6) in vec2 InUV1;
#endif
#define XMAT_VARYING_IN
!>