vec3 $var = $input[0].rgb * 2.0 - 1.0;
$var = normalize(vec3($var.xy * $input[1]_prop[0], $var.z));
