[TOP]<!#ifdef XLION_LDR_OUTPUT
#define XLION_NO_SHADOW_MAP
#endif
#include "Cache/dependencies/xLIONRender/src/shaders/mb_standard_pbr.frag"
#include "Cache/dependencies/xLIONRender/src/shaders/mb_tone_mapper_lion.frag"
#include "Cache/dependencies/xLIONRender/src/shaders/mb_lineartogamma.frag"
layout (location = 0) out vec4 outFragColor;
!> [BOT]<!#ifdef XMAT_MASKED
if ($input[6]_prop[3] < XMAT_ALPHA_CUTOFF) discard;
#ifndef XLION_LDR_OUTPUT
if (UBOLight.LightParams.w > 0.5) { outFragColor = vec4(0.0); XLION_GPRE_OUT(); return; }
#endif
#endif
vec3 $var = PBRLighting($input[3], $input[0], $input[4]_prop[2], 0.04, $input[1]_prop[0], $input[2]_prop[1], $input[5]);
#ifdef XLION_LDR_OUTPUT
$var = linearToSrgb(ToneMapper_lion($var));
#endif
#if defined(XMAT_MASKED) || defined(XLION_TILE_PIXEL)
outFragColor = vec4($var, 1.0);
#else
outFragColor = vec4($var, $input[6]_prop[3]);
#endif
!>