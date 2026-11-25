/* Wall clock and randomness weak defaults (platform.h): Unix time as an
 * offset against the monotonic platform_now_ms, unknown until set. A reset of
 * the monotonic clock (platform_time_reset) is the platform's to follow with a
 * new set; the default does not notice it. */
#include "platform.h"

static int64_t wall_offset;     /* unix ms - platform_now_ms at the time of the set */
static int wall_known;

__attribute__((weak)) int64_t platform_unix_time_ms(void) {
    return wall_known ? wall_offset + (int64_t)platform_now_ms() : 0;
}

__attribute__((weak)) int platform_set_unix_time_ms(int64_t ms) {
    if (ms > 0 && ms < GOGO_BUILD_UNIX_MS) return 0;
    wall_known = ms > 0;
    wall_offset = wall_known ? ms - (int64_t)platform_now_ms() : 0;
    return 1;
}

__attribute__((weak)) uint32_t platform_random(void) {
    static uint32_t x = 0x9E3779B9u;
    x ^= (uint32_t)platform_now_ms() * 2654435761u + (uint32_t)platform_unix_time_ms();
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;       /* xorshift32 */
    return x;
}
