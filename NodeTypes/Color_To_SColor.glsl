vec4 $var = vec4(mix($input[0].rgb * 12.92, 1.055 * pow($input[0].rgb, vec3(1.0 / 2.4)) - 0.055, greaterThan($input[0].rgb, vec3(0.0031308))), $input[0].a);
