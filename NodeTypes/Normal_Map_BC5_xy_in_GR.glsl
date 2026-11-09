vec3 $var = vec3($input[0].gr * 2.0 - 1.0, 0.0);
$var.z = sqrt(max(1.0 - dot($var.xy, $var.xy), 0.0));
$var = normalize(vec3($var.xy * $input[1]_prop[0], $var.z));
