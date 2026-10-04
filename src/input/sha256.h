#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32
#define SHA256_HEX_SIZE 65

typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} Sha256Context;

void sha256_init(Sha256Context *ctx);
void sha256_update(Sha256Context *ctx, const void *input, size_t len);
void sha256_final(Sha256Context *ctx, uint8_t digest[SHA256_DIGEST_SIZE]);
void sha256_bytes(const void *input, size_t len, uint8_t digest[SHA256_DIGEST_SIZE]);
void sha256_hex(const void *input, size_t len, char out[SHA256_HEX_SIZE]);

#endif
