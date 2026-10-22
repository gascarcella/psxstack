/* SHA-256 (FIPS 180-4), our own implementation: the launcher's check of a downloaded release against the digest
 * GitHub's release API gives each asset ("sha256:<hex>"; docs/LAUNCHER.md "Applying one"). */
#ifndef PORT_SHA256_H
#define PORT_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct PortSha256 {
    uint32_t h[8];
    uint64_t length; /* bytes so far */
    uint8_t block[64];
    size_t used; /* bytes in block */
} PortSha256;

void port_sha256_init(PortSha256 *c);
void port_sha256_update(PortSha256 *c, const void *data, size_t n);
void port_sha256_final(PortSha256 *c, uint8_t digest[32]);
/* init, update, final over one buffer */
void port_sha256(const void *data, size_t n, uint8_t digest[32]);
/* digest -> 64 lowercase hex digits and a NUL */
void port_sha256_hex(const uint8_t digest[32], char hex[65]);

#endif /* PORT_SHA256_H */
