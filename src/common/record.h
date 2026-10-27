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
};

/*
 * Fixed 32-byte wire/log format shared by sensord, hub, and store.
 * Field order is chosen so every member sits at its natural alignment
 * (no compiler padding) and the CRC covers exactly the preceding 28 bytes.
 *
 * temp_c and rh are Celsius/%RH x10 (0.1 resolution); sensors/reading.py
 * works in Fahrenheit, so the sensord/Python boundary must convert.
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

#endif
