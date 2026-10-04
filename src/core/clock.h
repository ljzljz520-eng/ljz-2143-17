#ifndef INPUT_STRATEGY_CLOCK_H
#define INPUT_STRATEGY_CLOCK_H
#include <stdint.h>
/* 单调时钟毫秒（用于连发/全屏超时）。平台可替换实现以便测试。 */
uint32_t clock_monotonic_ms(void);
#endif
