# 4. Storage and data

**Record:** fixed 32 bytes, little-endian, no padding, defined in `src/common/record.h` (the header is the source of truth; `tests/c/test_record.c` checks every offset).

| Off | Field | Type | Meaning |
|---|---|---|---|
| 0 | `mono_ns` | u64 | `CLOCK_MONOTONIC` ns: ordering and duplicate detection |
| 8 | `seq` | u32 | per-source sequence number |
| 12 | `wall_s` | u32 | unix seconds, advisory only (the clock can jump) |
| 16 | `lux` | f32 | TSL2591 lux; 0 unless source = light and not MISSING |
| 20 | `rh` | u16 | %RH x10; 0 unless source = dht and not MISSING |
| 22 | `temp_c` | i16 | degC x10; 0 unless source = dht and not MISSING |
| 24 | `source` | u8 | 0 = dht, 1 = light |
| 25 | `flags` | u8 | bit 0 FAKE, bit 1 SUSPECT, bit 2 MISSING; 0 = real |
| 26 | `reserved` | u8[2] | must be zero |
| 28 | `crc32` | u32 | IEEE CRC-32 of bytes 0-27 |

- A failed DHT read is a MISSING record (value fields zero), never a zero reading. On the Pi, the DHT11 reported temperature in 0.1 degC steps and humidity in whole %RH, so `rh` is a multiple of 10 today.
- Valid record (`cs370_record_valid()`, used by hub and store): CRC matches, `source < 2`, no undefined flag bits, `reserved` zero, `lux` finite and >= 0, and fields that do not apply to the source are zero.
- SUSPECT bounds (targets **[Pi: unverified]**): DHT11 temperature outside 0-50 degC or RH outside 20-90 %, or a change of more than 5 degC or 20 %RH between consecutive samples; light outside 0-88000 lux or a jump not explained by a gain change.
- Rate: 2 Hz light + 0.5 Hz DHT = 2.5 records/s; 32 B x 2.5 = 80 B/s, about 6.9 MB/day.
- Retention: 14 days (about 100 MB), rotated by segment file with the oldest deleted; the dashboard reads history from the log, not from memory.
- Durability: `write` per record into a buffered segment, `fdatasync` every N records or 5 s, whichever comes first. The loss window is at most 5 s and will be reported as a measured number.
- Power dies mid-write: on start, scan the tail segment, verify each CRC, and truncate at the first bad record.
- Replayed or synthetic records carry the FAKE flag, and the dashboard and logs label them.
