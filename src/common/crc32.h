#ifndef CS370_CRC32_H
#define CS370_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320). Used by the store's records. */
uint32_t crc32_update(uint32_t crc, const void *buf, size_t len);

static inline uint32_t crc32_of(const void *buf, size_t len)
{
    return crc32_update(0, buf, len);
}

#endif
