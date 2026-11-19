[TOP]<!#include "Cache/Plugins/xmaterial.plugin/NodeTypes/Wear_Common.glsl"
!>
float $var = xmat_fbm3($input[0] * $input[1]_prop[0] + xmat_seed($input[2]_prop[1]));
