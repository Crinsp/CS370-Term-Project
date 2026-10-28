#include <math.h>
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

#define CRC_OFF offsetof(struct cs370_record, crc32)

static void put_le(uint8_t *p, uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* Packs the same field values into a flat buffer at the offsets the header
 * documents (0, 8, 12, 16, 20, 22, 24, 25, 26, 28), byte by byte in
 * little-endian order, independent of the struct definition. Comparing this
 * against the struct's own bytes catches a wrong offset, an unwanted
 * compiler-inserted padding byte, and a host that is not little-endian. */
static void pack_expected(uint8_t out[CS370_RECORD_SIZE], uint64_t mono_ns,
                           uint32_t seq, uint32_t wall_s, float lux,
                           uint16_t rh, int16_t temp_c, uint8_t source,
                           uint8_t flags)
{
    uint32_t lux_bits;
    memcpy(&lux_bits, &lux, sizeof(lux_bits));

    memset(out, 0, CS370_RECORD_SIZE);
    put_le(out + 0, mono_ns, 8);
    put_le(out + 8, seq, 4);
    put_le(out + 12, wall_s, 4);
    put_le(out + 16, lux_bits, 4);
    put_le(out + 20, rh, 2);
    put_le(out + 22, (uint16_t)temp_c, 2);
    out[24] = source;
    out[25] = flags;
    /* out[26..27] stay zero: reserved */
    put_le(out + 28, crc32_of(out, 28), 4);
}

static struct cs370_record make(uint8_t source, uint8_t flags, float lux,
                                uint16_t rh, int16_t temp_c)
{
    struct cs370_record r = {
        .mono_ns = 123456789ull,
        .seq     = 42,
        .wall_s  = 1700000000,
        .lux     = lux,
        .rh      = rh,
        .temp_c  = temp_c,
        .source  = source,
        .flags   = flags,
    };
    cs370_record_seal(&r);
    return r;
}

/* Re-seals after a change so the CRC is good and only the rule under test fails. */
static int valid_after(struct cs370_record r)
{
    cs370_record_seal(&r);
    return cs370_record_valid(&r);
}

int main(void)
{
    CHECK(sizeof(struct cs370_record) == CS370_RECORD_SIZE);
    CHECK(CRC_OFF == 28);

    /* DHT11 reading: 23 C, 62 %RH (whole units, x10). */
    struct cs370_record rec = make(CS370_SOURCE_DHT, 0, 0.0f, 620, 230);

    uint8_t expected[CS370_RECORD_SIZE];
    pack_expected(expected, rec.mono_ns, rec.seq, rec.wall_s, rec.lux, rec.rh,
                  rec.temp_c, rec.source, rec.flags);

    /* Independently-packed bytes must match the struct's own bytes exactly:
     * same field offsets, no padding, fixed byte order, and a CRC that is
     * reproducible from the documented layout rather than just self-consistent. */
    CHECK(memcmp(&rec, expected, CS370_RECORD_SIZE) == 0);
    CHECK(expected[8] == 42); /* seq low byte first */

    /* A TSL2591 reading and the two flag-carrying cases also match the layout. */
    struct cs370_record light = make(CS370_SOURCE_LIGHT, 0, 88.5f, 0, 0);
    pack_expected(expected, light.mono_ns, light.seq, light.wall_s, light.lux,
                  light.rh, light.temp_c, light.source, light.flags);
    CHECK(memcmp(&light, expected, CS370_RECORD_SIZE) == 0);

    struct cs370_record miss = make(CS370_SOURCE_DHT, CS370_FLAG_MISSING, 0.0f, 0, 0);
    pack_expected(expected, miss.mono_ns, miss.seq, miss.wall_s, miss.lux,
                  miss.rh, miss.temp_c, miss.source, miss.flags);
    CHECK(memcmp(&miss, expected, CS370_RECORD_SIZE) == 0);

    CHECK(cs370_record_valid(&rec));
    CHECK(cs370_record_valid(&light));
    CHECK(cs370_record_valid(&miss));
    struct cs370_record lmiss = make(CS370_SOURCE_LIGHT, CS370_FLAG_MISSING, 0.0f, 0, 0);
    CHECK(cs370_record_valid(&lmiss));
    struct cs370_record fake = make(CS370_SOURCE_DHT, CS370_FLAG_FAKE, 0.0f, 620, 230);
    CHECK(cs370_record_valid(&fake));
    struct cs370_record susp = make(CS370_SOURCE_LIGHT, CS370_FLAG_SUSPECT, 90000.0f, 0, 0);
    CHECK(cs370_record_valid(&susp));

    /* Flipping any single payload field, or the stored CRC, must be caught. */
    struct cs370_record f;

#define FLIP(stmt)                                          \
    do {                                                    \
        f = rec;                                            \
        stmt;                                               \
        CHECK(crc32_of(&f, CRC_OFF) != rec.crc32);          \
        CHECK(!cs370_record_valid(&f));                     \
    } while (0)

    FLIP(f.mono_ns += 1);
    FLIP(f.seq += 1);
    FLIP(f.wall_s += 1);
    FLIP(f.lux = 1.0f);
    FLIP(f.rh += 1);
    FLIP(f.temp_c += 1);
    FLIP(f.source = CS370_SOURCE_LIGHT);
    FLIP(f.flags |= CS370_FLAG_SUSPECT);
    FLIP(f.flags |= CS370_FLAG_FAKE);
    FLIP(f.flags |= CS370_FLAG_MISSING);
    FLIP(f.reserved[0] = 1);
    FLIP(f.reserved[1] = 1);

    f = rec;
    f.crc32 ^= 1;
    CHECK(!cs370_record_valid(&f));

    /* Rejection rules, each re-sealed so only that rule fails. */
    f = rec; f.source = CS370_SOURCE_COUNT;        CHECK(!valid_after(f));
    f = rec; f.source = 0xff;                      CHECK(!valid_after(f));
    f = rec; f.flags = 1u << 3;                    CHECK(!valid_after(f));
    f = rec; f.flags = 0x80;                       CHECK(!valid_after(f));
    f = rec; f.lux = 5.0f;                         CHECK(!valid_after(f)); /* DHT with lux */
    f = light; f.temp_c = 230;                     CHECK(!valid_after(f)); /* LIGHT with temp */
    f = light; f.rh = 620;                         CHECK(!valid_after(f)); /* LIGHT with rh */
    f = miss; f.temp_c = 230;                      CHECK(!valid_after(f)); /* MISSING with value */
    f = miss; f.rh = 620;                          CHECK(!valid_after(f));
    f = lmiss; f.lux = 5.0f;                       CHECK(!valid_after(f));
    f = light; f.lux = NAN;                        CHECK(!valid_after(f));
    f = light; f.lux = INFINITY;                   CHECK(!valid_after(f));
    f = light; f.lux = -1.0f;                      CHECK(!valid_after(f));
    f = light; f.lux = 0.0f;                       CHECK(valid_after(f)); /* dark room is real */

    /* seal() clears reserved bytes the caller left dirty. */
    f = rec; f.reserved[0] = 7; cs370_record_seal(&f);
    CHECK(f.reserved[0] == 0 && cs370_record_valid(&f));

    if (failures == 0)
        puts("test_record: ok");
    return failures ? 1 : 0;
}
