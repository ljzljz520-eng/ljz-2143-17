#define _POSIX_C_SOURCE 200809L
#include "clock.h"

#include <time.h>

uint32_t clock_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}
