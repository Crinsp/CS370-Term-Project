#ifndef CS370_RECORD_H
#define CS370_RECORD_H

#include <stddef.h>
#include <stdint.h>

/* Sensor source tag. */
enum cs370_record_source {
    CS370_SOURCE_DHT   = 0,
    CS370_SOURCE_LIGHT = 1,
};

/* Bitflags. 0 means real + not suspect. */
enum cs370_record_flags {
    CS370_FLAG_FAKE    = 1u << 0, /* synthesized, not a live sensor read */
    CS370_FLAG_SUSPECT = 1u << 1, /* out of plausible range / rate-of-change */
    CS370_FLAG_MISSING = 1u << 2, /* this source's read failed; temp_c/rh/lux are not meaningful */
};

/*
 * Fixed 32-byte wire/log format shared by sensord, hub, and store.
 * Field order is chosen so every member sits at its natural alignment
 * (no compiler padding) and the CRC covers exactly the preceding 28 bytes.
 * Native byte order (little-endian on the Pi and on x86_64 dev machines);
 * this is a same-host IPC/log format, not a portable wire format.
 *
 * temp_c and rh are Celsius/%RH x10 (0.1 resolution); sensors/reading.py
 * works in Fahrenheit, so the sensord/Python boundary must convert.
 * A bare 0 is a real reading, never "no reading" -- a failed DHT11 read
 * must be marked with CS370_FLAG_MISSING instead of being zeroed, so the
 * dashboard can tell a dead/stale sensor from an actual 0.0 (CLAUDE.md:
 * dead/stale/implausible sensors must be shown as such, never silently).
 */
struct cs370_record {
    uint64_t mono_ns;   /* CLOCK_MONOTONIC, ns: ordering + duplicate detection */
    uint32_t seq;       /* per-source sequence number */
    uint32_t wall_s;    /* wall-clock unix seconds, advisory only (clock can jump) */
    float    lux;
    uint16_t rh;        /* relative humidity x10, 0 if source == LIGHT */
    int16_t  temp_c;    /* Celsius x10, 0 if source == LIGHT */
    uint8_t  source;    /* enum cs370_record_source */
    uint8_t  flags;     /* bitmask of enum cs370_record_flags */
    uint8_t  reserved[2];
    uint32_t crc32;     /* crc32_of(this record, offsetof(crc32)) */
};

#define CS370_RECORD_SIZE 32

_Static_assert(sizeof(struct cs370_record) == CS370_RECORD_SIZE,
                "cs370_record must be exactly 32 bytes");

#endif
