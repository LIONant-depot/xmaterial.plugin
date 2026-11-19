[TOP]<!#include "Cache/Plugins/xmaterial.plugin/NodeTypes/Wear_Common.glsl"
!>
vec3 $var_n = XMAT_WORLD_N; vec3 $var_p = XMAT_WORLD_POS;
vec3 $var_dpx = dFdx($var_p), $var_dpy = dFdy($var_p);
float $var = clamp(0.5 * (dot(dFdx($var_n), $var_dpx) / max(dot($var_dpx, $var_dpx), 1e-10) + dot(dFdy($var_n), $var_dpy) / max(dot($var_dpy, $var_dpy), 1e-10)) * $input[0]_prop[0], -1.0, 1.0);
