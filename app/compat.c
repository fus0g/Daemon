/* SPDX-License-Identifier: GPL-3.0-or-later */
/* glibc 2.43 exports sqrtf/acosf under a new symbol version, which would make the binary refuse to start on any older glibc.
 * The linker redirects those two calls here (-Wl,--wrap) and we forward to the double versions, which have had the same
 * symbol version since glibc 2.2.5. */
#include <math.h>
float __wrap_sqrtf(float x) { return (float)sqrt((double)x); }
float __wrap_acosf(float x) { return (float)acos((double)x); }
