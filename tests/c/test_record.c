#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "record.h"
#include "crc32.h"

static int failures;

#define CHECK(cond)                                                   \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                               \
        }                                                             \
    } while (0)

/* Packs the same field values into a flat buffer at the offsets the header
 * documents (0, 8, 12, 16, 20, 22, 24, 25, 26, 28), independent of the
 * struct definition. Comparing this against the struct's own bytes catches
 * both a wrong offset and an unwanted compiler-inserted padding byte. */
static void pack_expected(uint8_t out[CS370_RECORD_SIZE], uint64_t mono_ns,
                           uint32_t seq, uint32_t wall_s, float lux,
                           uint16_t rh, int16_t temp_c, uint8_t source,
                           uint8_t flags)
{
    memset(out, 0, CS370_RECORD_SIZE);
    memcpy(out + 0, &mono_ns, sizeof(mono_ns));
    memcpy(out + 8, &seq, sizeof(seq));
    memcpy(out + 12, &wall_s, sizeof(wall_s));
    memcpy(out + 16, &lux, sizeof(lux));
    memcpy(out + 20, &rh, sizeof(rh));
    memcpy(out + 22, &temp_c, sizeof(temp_c));
    out[24] = source;
    out[25] = flags;
    /* out[26..27] stay zero: reserved */
    uint32_t crc = crc32_of(out, 28);
    memcpy(out + 28, &crc, sizeof(crc));
}

int main(void)
{
    CHECK(sizeof(struct cs370_record) == CS370_RECORD_SIZE);
    CHECK(offsetof(struct cs370_record, crc32) == 28);

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

    uint8_t expected[CS370_RECORD_SIZE];
    pack_expected(expected, rec.mono_ns, rec.seq, rec.wall_s, rec.lux, rec.rh,
                  rec.temp_c, rec.source, rec.flags);

    /* Independently-packed bytes must match the struct's own bytes exactly:
     * same field offsets, no padding, and a CRC that is reproducible from
     * the documented layout rather than just self-consistent. */
    CHECK(memcmp(&rec, expected, CS370_RECORD_SIZE) == 0);

    /* Flipping any single payload field must change the CRC. */
    struct cs370_record flipped = rec;

    flipped = rec;
    flipped.seq += 1;
    CHECK(crc32_of(&flipped, 28) != rec.crc32);

    flipped = rec;
    flipped.temp_c += 1;
    CHECK(crc32_of(&flipped, 28) != rec.crc32);

    flipped = rec;
    flipped.rh += 1;
    CHECK(crc32_of(&flipped, 28) != rec.crc32);

    flipped = rec;
    flipped.source = CS370_SOURCE_LIGHT;
    CHECK(crc32_of(&flipped, 28) != rec.crc32);

    flipped = rec;
    flipped.flags |= CS370_FLAG_SUSPECT;
    CHECK(crc32_of(&flipped, 28) != rec.crc32);

    if (failures == 0)
        puts("test_record: ok");
    return failures ? 1 : 0;
}
