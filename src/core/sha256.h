#ifndef INPUT_STRATEGY_SHA256_H
#define INPUT_STRATEGY_SHA256_H
#include <stdint.h>
#include <stddef.h>
#define SHA256_HEXLEN 64
void sha256_hex(const void *data, size_t len, char out_hex[65]);
#endif
