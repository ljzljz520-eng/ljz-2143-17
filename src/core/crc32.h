#ifndef INPUT_STRATEGY_CRC32_H
#define INPUT_STRATEGY_CRC32_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
uint32_t crc32_ieee(const void *data, size_t len);
#endif
