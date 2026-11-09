vec4 $var = vec4(mix($input[0].rgb / 12.92, pow(($input[0].rgb + 0.055) / 1.055, vec3(2.4)), greaterThan($input[0].rgb, vec3(0.04045))), $input[0].a);
