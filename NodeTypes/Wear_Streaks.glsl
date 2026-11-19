[TOP]<!#include "Cache/Plugins/xmaterial.plugin/NodeTypes/Wear_Common.glsl"
!>
float $var = smoothstep(0.55, 0.8, xmat_fbm3(vec3($input[0].x * $input[2]_prop[0], $input[0].y * $input[2]_prop[0] / $input[3]_prop[1], $input[0].z * $input[2]_prop[0]) + xmat_seed($input[4]_prop[2]))) * (1.0 - abs($input[1].y));
