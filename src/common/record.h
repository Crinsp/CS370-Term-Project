#ifndef CS370_RECORD_H
#define CS370_RECORD_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "crc32.h"

/* Sensor source tag. */
enum cs370_record_source {
    CS370_SOURCE_DHT   = 0, /* DHT11 on GPIO4: whole degC and whole %RH */
    CS370_SOURCE_LIGHT = 1, /* TSL2591 on I2C bus 1, address 0x29 */
    CS370_SOURCE_COUNT = 2, /* number of valid sources; not a source itself */
};

/* Bitflags. 0 means real + not suspect. */
enum cs370_record_flags {
    CS370_FLAG_FAKE    = 1u << 0, /* synthesized, not a live sensor read */
    CS370_FLAG_SUSPECT = 1u << 1, /* out of plausible range / rate-of-change */
    CS370_FLAG_MISSING = 1u << 2, /* this source's read failed; no value */
};

#define CS370_FLAGS_MASK \
    (CS370_FLAG_FAKE | CS370_FLAG_SUSPECT | CS370_FLAG_MISSING)

/*
 * Fixed 32-byte wire/log format shared by sensord, hub, and store.
 * Field order is chosen so every member sits at its natural alignment
 * (no compiler padding) and the CRC covers exactly the preceding 28 bytes.
 * Native byte order, enforced little-endian below (the Pi and x86_64 dev
 * machines); this is a same-host IPC/log format, not a portable wire format.
 *
 * Each record carries one source's reading. Fields that do not apply to the
 * record's source are ZERO, as is `reserved`; cs370_record_valid() rejects
 * anything else. So:
 *   DHT   : temp_c, rh set; lux == 0
 *   LIGHT : lux set; temp_c == rh == 0
 *   MISSING (either source): temp_c == rh == 0 and lux == 0
 * A zero in an applicable field is a real reading, never "no reading": a
 * failed read must set CS370_FLAG_MISSING, so the dashboard can tell a
 * dead/stale sensor from an actual 0.0 (CLAUDE.md: dead/stale/implausible
 * sensors must be shown as such, never silently).
 *
 * temp_c and rh are degC / %RH x10. The DHT11 only resolves whole units, so
 * they are always multiples of 10 today; the x10 leaves room for a finer
 * sensor. sensors/reading.py works in Fahrenheit, so the sensord/Python
 * boundary must convert. lux is computed by the sensord from the TSL2591
 * channel counts (finite and >= 0 for a non-MISSING LIGHT record).
 */
struct cs370_record {
    uint64_t mono_ns;   /* CLOCK_MONOTONIC, ns: ordering + duplicate detection */
    uint32_t seq;       /* per-source sequence number */
    uint32_t wall_s;    /* wall-clock unix seconds, advisory only (clock can jump) */
    float    lux;       /* 0 unless source == LIGHT and not MISSING */
    uint16_t rh;        /* relative humidity x10; 0 unless DHT and not MISSING */
    int16_t  temp_c;    /* Celsius x10; 0 unless DHT and not MISSING */
    uint8_t  source;    /* enum cs370_record_source */
    uint8_t  flags;     /* bitmask of enum cs370_record_flags */
    uint8_t  reserved[2]; /* must be zero */
    uint32_t crc32;     /* crc32_of(this record, offsetof(crc32)) */
};

#define CS370_RECORD_SIZE 32

_Static_assert(sizeof(struct cs370_record) == CS370_RECORD_SIZE,
                "cs370_record must be exactly 32 bytes");
_Static_assert(offsetof(struct cs370_record, crc32) == CS370_RECORD_SIZE - 4,
                "crc32 must be the last 4 bytes");
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
                "record format is little-endian only");

/* Zeroes `reserved` and computes the CRC. Call after filling every other field. */
static inline void cs370_record_seal(struct cs370_record *r)
{
    memset(r->reserved, 0, sizeof(r->reserved));
    r->crc32 = crc32_of(r, offsetof(struct cs370_record, crc32));
}

/* 1 if the record is well-formed, 0 if the hub/store must reject it. Checks
 * the CRC, source, flag bits, zero-fill rules and lux sanity; it does not
 * judge plausibility of values (that is the SUSPECT flag's job). */
static inline int cs370_record_valid(const struct cs370_record *r)
{
    if (r->crc32 != crc32_of(r, offsetof(struct cs370_record, crc32)))
        return 0;
    if (r->source >= CS370_SOURCE_COUNT)
        return 0;
    if (r->flags & ~CS370_FLAGS_MASK)
        return 0;
    if (r->reserved[0] || r->reserved[1])
        return 0;
    if (!isfinite(r->lux) || r->lux < 0.0f)
        return 0;

    int missing = (r->flags & CS370_FLAG_MISSING) != 0;
    int has_th  = r->source == CS370_SOURCE_DHT && !missing;
    int has_lux = r->source == CS370_SOURCE_LIGHT && !missing;
    if (!has_th && (r->temp_c != 0 || r->rh != 0))
        return 0;
    if (!has_lux && r->lux != 0.0f)
        return 0;
    return 1;
}

#endif
