[TOP]<!layout (location = 0) out  vec4 outFragColor;
!> [BOT]<!#ifdef XMAT_MASKED
if ($input[1]_prop[0] < XMAT_ALPHA_CUTOFF) discard;
#endif
outFragColor = vec4($input[0],$input[1]_prop[0]);
!>