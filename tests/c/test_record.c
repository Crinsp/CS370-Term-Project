#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "record.h"
#include "crc32.h"

int main(void)
{
    /* Layout must stay exactly 32 bytes and match the documented offsets,
     * since store's recovery scan reads this as a fixed-size raw record. */
    assert(sizeof(struct cs370_record) == CS370_RECORD_SIZE);
    assert(offsetof(struct cs370_record, crc32) == 28);

    struct cs370_record rec = {
        .mono_ns = 123456789ull,
        .seq     = 42,
        .wall_s  = 1700000000,
        .lux     = 88.5f,
        .rh      = 625,   /* 62.5% RH */
        .temp_c  = 231,   /* 23.1 C */
        .source  = CS370_SOURCE_DHT,
        .flags   = 0,
    };
    memset(rec.reserved, 0, sizeof(rec.reserved));

    rec.crc32 = crc32_of(&rec, offsetof(struct cs370_record, crc32));
    assert(crc32_of(&rec, offsetof(struct cs370_record, crc32)) == rec.crc32);

    /* Flipping any payload byte must change the CRC. */
    struct cs370_record corrupt = rec;
    corrupt.seq += 1;
    assert(crc32_of(&corrupt, offsetof(struct cs370_record, crc32)) != rec.crc32);

    return 0;
}
