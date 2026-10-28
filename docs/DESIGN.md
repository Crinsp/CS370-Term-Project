# Design Document (M2): Gecko Enclosure Humidity Advisor

Authors: Gerritt Ewing, Zoe Ebert. Status: DRAFT. Items marked **[PI]** need real evidence from the Pi; items marked **[TEAM]** need a decision from both partners.

User: Rosalie Endris, keeper of a gecko. The device is advisory only: it senses, logs, analyzes and displays on a LAN dashboard. It never controls the humidifier.

## 1. Architecture

```
 kernel:  I2C subsystem (/dev/i2c-1)            GPIO (DHT11 on GPIO4)
              |                                        |
        [sensord light]  2 Hz                [sensord dht]  0.5 Hz (kernel dht11 IIO, sysfs)
          raw I2C, our register code                   |
              |  SEQPACKET, 32 B record/msg            |  SEQPACKET, 32 B record/msg
              +----------------+-----------------------+
                               v  $CS370_RUN_DIR/hub.sock
                           [hub]  validates CRC/size/source, per-source LIVE/STALE/DEAD,
                               |  fuses latest-of-each into a 1 Hz snapshot
                +--------------+------------------------------+
                | every valid record, 2.5 rec/s               | 1 Hz status.json (tmp + rename)
                v  $CS370_RUN_DIR/store.sock                  v
            [store]  append-only log      [analysis] / [dashboard] read the snapshot
             fsync batch, CRC, seq            | ~1 Hz status
                                              v
                                        [dashboard]  LAN HTTP, read-only, serves status + history
 [supervisor]: parent of all of the above; restarts on exit/hang; writes heartbeat to log
```

Rates are targets, to be confirmed against measurements **[PI]**. DHT11 cannot be read faster than about 1 Hz and about 10% of reads fail; the light sensor is cheap and fast, so the two sources deliberately run at different rates and the hub reconciles them.

Every process is a separate C binary under `src/`. Python is limited to the dashboard front end and tooling.

**IPC contract (sensord, hub, store).**
- Transport: `AF_UNIX`, `SOCK_SEQPACKET`. Each message is exactly one `struct cs370_record` (`src/common/record.h`, 32 bytes). SEQPACKET keeps message boundaries, and a peer that dies shows up as EOF on the next read, so the hub learns about a dead sensord without waiting for a timeout.
- Sockets: the hub listens on `$CS370_RUN_DIR/hub.sock`, and every sensord connects to it. The store listens on `$CS370_RUN_DIR/store.sock`, and the hub connects to it. `CS370_RUN_DIR` defaults to `$XDG_RUNTIME_DIR/cs370`.
- Every fd is opened with `SOCK_CLOEXEC`/`O_CLOEXEC`, so a restarted child never inherits a stale socket. Every send uses `MSG_NOSIGNAL`, so a dead peer returns `EPIPE` instead of killing us with SIGPIPE.
- sensord does not buffer. If the hub is down, it keeps sampling, drops the samples and counts them, and reconnects with exponential backoff (0.5 s, doubling up to 8 s). The gap shows up in the store as a `seq` jump.
- The hub forwards every valid record to the store. While the store is down, the hub holds them in a bounded 256-record queue (about 100 s of data). When the queue overflows, the oldest records are dropped, counted, and logged.
- Invalid records (wrong size, bad CRC, unknown source) are rejected and logged, never forwarded.

**Liveness (hub), per source.** Thresholds are targets until the Pi measurements are in **[PI]**.

| State | Condition |
|---|---|
| LIVE | a valid, non-MISSING sample within 3 sample periods (DHT 6 s, light 1.5 s) |
| STALE | no valid sample for 3 periods, or the sensord connection closed |
| DEAD | no valid sample for 60 s |
| SUSPECT | the latest value failed the range or rate-of-change check (tracked separately from staleness) |

Every second the hub atomically replaces `$CS370_RUN_DIR/status.json` (write to a temp file, then `rename`). For each source, the file carries the state, the last value, its age, and the FAKE flag. The dashboard renders this state and never infers freshness on its own.

## 2. Mechanism mapping

| Menu item | Component | Justification from Rosalie's needs |
|---|---|---|
| **E** multi-process + supervisor | `sensord-*`, `hub`, `supervisor` | A dead sensor must never look like "humidity is fine". Each sensor runs in its own process, so a hung DHT11 read cannot stall light sampling or the dashboard. When a sensord dies, its SEQPACKET connection hits EOF and the hub marks that source STALE right away. If it stays silent for 60 s, the hub marks it DEAD. The supervisor restarts it, and the gap is visible in the log as a `seq` jump. The dashboard shows STALE or DEAD from `status.json` instead of the last value. |
| **D** crash-consistent storage | `store` | The drift model needs days of history and Rosalie's question after a bad night is "what happened", so the log must survive a power cut. Append-only fixed-size CRC records plus a recovery scan that truncates a torn tail. |

Not chosen, and why: A (kernel driver) adds no value for a 1 Hz sensor; B and C do not matter at DHT11 timescales (jitter of a few ms is irrelevant to humidity that moves over minutes); F has no rate pressure. We say this plainly rather than bolt on a mechanism.

## 3. Failure-mode table

| Component | Failure | Detection | System response | Log shows |
|---|---|---|---|---|
| sensord-dht | single read fails (checksum/timeout, ~10%) | read returns error | retry once after 2 s; report miss, no value | `dht miss n=<consecutive>` |
| sensord-dht | sensor unplugged / dead | N consecutive misses or no sample for 60 s | mark DEAD; dashboard banner; keep other sensor running | `dht DEAD since <ts>` |
| sensord-light | I2C NACK / bus error | errno from ioctl/read | reinit device; after N failures mark DEAD | `light err errno=<n>` |
| any sensord | process killed (`kill -9`) | supervisor `waitpid` | restart with backoff, new generation id | `child dht died sig=9 restart=<n>` |
| any sensord | hang (alive, no output) | no heartbeat/sample within deadline | supervisor SIGKILL then restart | `child dht hung, killed` |
| hub | exits | supervisor; each sensord gets `EPIPE`/`ECONNREFUSED` | restart; each sensord counts dropped samples and reconnects with backoff (0.5 s up to 8 s) | `hub restart`, `sensord dht hub lost, dropped=<n>` |
| hub | store down / not yet started | `connect` fails or `EPIPE` on send | hold up to 256 records, reconnect with backoff, drop the oldest on overflow | `hub store lost`, `hub store drop n=<total>` |
| hub | malformed record from a client | size != 32, CRC mismatch, or unknown source | reject the record, keep the connection | `hub reject src=<fd> reason=crc` |
| sensord | dht11 driver/overlay missing | sysfs path absent at startup | exit 1 (supervisor logs and backs off); dashboard shows DHT DEAD | `sensord dht no iio device` |
| store | power cut mid-write | CRC/length check at startup scan | truncate to last good record; continue | `recover truncated <bytes>` |
| store | disk full | `write` returns ENOSPC | stop appending, dashboard flag, keep sensing | `store ENOSPC` |
| supervisor | dies | systemd (or runner) restarts it; children detect parent loss and exit | clean restart; orphan fds closed | `supervisor start gen=<n>` |
| clock | jump (no RTC, NTP step) | monotonic vs wall delta | order by monotonic + seq, wall time is advisory | `clock step <s>` |
| any | sensor value implausible | range / rate-of-change check | mark SUSPECT, do not feed model | `dht suspect rh=<v>` |

Rule from CLAUDE.md: stale, dead or implausible data is shown as such, never as the last good value.

## 4. Storage and data

- Record: fixed 32 bytes, little-endian, no padding, defined in `src/common/record.h` (the header is the source of truth; `tests/c/test_record.c` checks every offset):

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `mono_ns` | u64 | `CLOCK_MONOTONIC` ns: ordering and duplicate detection |
| 8 | `seq` | u32 | per-source sequence number |
| 12 | `wall_s` | u32 | unix seconds, advisory only (the clock can jump) |
| 16 | `lux` | f32 | TSL2591 lux; 0 unless `source = light` and not MISSING |
| 20 | `rh` | u16 | %RH x10; 0 unless `source = dht` and not MISSING |
| 22 | `temp_c` | i16 | degC x10; 0 unless `source = dht` and not MISSING |
| 24 | `source` | u8 | 0 = dht, 1 = light |
| 25 | `flags` | u8 | bit 0 FAKE, bit 1 SUSPECT, bit 2 MISSING; 0 = real |
| 26 | `reserved` | u8[2] | must be zero |
| 28 | `crc32` | u32 | IEEE CRC-32 of bytes 0-27 |

- The DHT11 resolves whole degC and whole %RH, so `temp_c` and `rh` are always multiples of 10 today; the x10 leaves room for a finer sensor. A failed DHT read is a MISSING record (value fields zero), never a zero reading.
- Valid record (`cs370_record_valid()`, used by hub and store): CRC matches, `source < 2`, no undefined flag bits, `reserved` is zero, `lux` is finite and >= 0, and fields that do not apply to the source (or any value field of a MISSING record) are zero. Anything else is rejected and logged.
- SUSPECT bounds (targets, to confirm against Pi data **[PI]**): DHT11 temperature outside 0-50 degC or RH outside 20-90 %, or a change of more than 5 degC or 20 %RH between consecutive samples; light outside 0-88000 lux (TSL2591 range) or a jump that is not explained by gain change.
- Rate: 2 Hz light + 0.5 Hz DHT = 2.5 records/s, 32 B x 2.5 = 80 B/s, about 6.9 MB/day.
- Retention: keep 14 days (about 100 MB at 80 B/s, small next to the Pi's 32 GB card), rotate by segment file and delete the oldest; the dashboard reads history from the log, not from memory.
- Durability: `write` per record into a buffered segment, `fdatasync` every N records or 5 s, whichever is first; the loss window is at most 5 s and is recorded as a measured number in the evaluation.
- Recovery: on start, scan the tail segment, verify CRC, truncate at the first bad record.
- Replayed or synthetic records carry `flags=fake` and the dashboard and logs label them.

## 5. Constraints and substitutions

| Ideal | Actual | Cost |
|---|---|---|
| SHT45 RH/temp (+/-1% RH, 0.1 C) | DHT11 (about +/-5% RH, whole degrees, 1 Hz, about 10% missed reads) | cannot resolve small drifts; the model works on trends over minutes, and claims are narrowed accordingly |
| DS18B20 second temperature | none | no independent temperature cross-check; light sensor is the second source |
| Actuator-in-the-loop control | advisory only | no closed-loop result to measure; evaluation is prediction quality, not control quality |
| Months of enclosure data | days of provoked episodes | claims limited to episodes we induce (lamp on/off, vent open, misting) |
| DHT11 read with timing we control | kernel `dht11` IIO driver (`dtoverlay=dht11,gpiopin=4`), read through sysfs | the bit timing is the kernel's, not ours (DHT timing is not one of our chosen mechanisms); bit-banging 26-70 us pulses from userspace is exactly where scheduling jitter breaks reads; the overlay claims GPIO4, so the Python `adafruit_dht` path no longer works on the Pi. Fallback: libgpiod v2 edge timestamps |

**Pi evidence (2026-10-27, piewing, kernel 6.18.50+rpt-rpi-v8 aarch64, libgpiod 2.2.1).**

TSL2591, alive (no Python; raw `i2c-tools` using the same register addresses the C backend uses):
- `i2cdetect -y 1`: device at 0x29, nothing else on bus 1.
- ID register (`i2cget -y 1 0x29 0xB2`) returns `0x50`, the TSL2591 part ID.
- After writing ENABLE = PON|AEN (`0xA0` ← `0x03`), the status register's AVALID bit is set.
- Channel counts (`0xB4`–`0xB7`) were near 0 at medium gain in the dark room and steady, non-zero at maximum gain. The counts scale with the CONTROL (`0xA1`) gain setting. Gain was restored to medium (`0x10`).
- Implementation note: on this bus, SMBus word reads return only one byte per register. The C backend reads C0DATAL..C1DATAH (4 bytes) as one block starting at command `0xB4`, not as two word reads.

TSL2591 lux (the C sensord reimplements what `adafruit_tsl2591` does in Python): read C0 (visible + IR, `0xB4`/`0xB5`) and C1 (IR, `0xB6`/`0xB7`); with integration time `ATIME_ms` and gain multiplier `G` (medium = 25x, max = 9876x), `CPL = ATIME_ms * G / 408.0` and `lux = max((C0 - C1) * (1 - C1/C0), 0) / CPL`. A saturated channel (36863 counts at 100 ms integration, 65535 at longer times) is reported as SUSPECT. Gain and integration time are fixed at startup so lux stays comparable over time.

DHT11: **[PI]** pending. Expected units: the kernel `dht11` driver reports `in_temp_input` in milli-degC and `in_humidityrelative_input` in milli-%RH, so the sensord divides by 100 to get the record's x10 units (to confirm). A read error from sysfs becomes a MISSING record. Will contain: `dtoverlay=dht11,gpiopin=4` added, reboot, `dmesg | grep -i dht`, `/sys/bus/iio/devices/iio:device0` present, repeated `in_humidityrelative_input` / `in_temp_input` reads (including misses), and humidity rising when breathed on. Measured miss rate over about 1 hour to follow.

## 6. Evaluation plan

| Measurement | Method | Target |
|---|---|---|
| Sample-to-log latency distribution | timestamp at sensord read vs at log append; p50/p95/p99, idle and under CPU stress | p99 under 250 ms (to confirm) |
| Per-process CPU and RSS | heartbeat series over the 48 h soak | RSS flat (no leak), CPU under 10% |
| Domain metric: band-exit prediction | N provoked episodes labeled by us; compare drift model with a plain threshold on lead time, misses, false alarms | model alerts earlier than threshold with no more false alarms |
| Fault injection | `kill -9` each child, unplug each sensor 10 min, power cut mid-write | each row in section 3 produces its log line and recovers |
| Durability | cut power at random during writes, repeat M times | zero corrupt records read back; loss window within 5 s |

## 7. Ownership and AI-use plan

Contributions so far: Gerritt set up the repo, the deploy and test scaffolding, the dummy/fake sensor code, and wired and configured the sensors on the Pi. Zoe drove the project design, including choosing the two-sensor idea, and wrote the README contributions.

Ownership (first authorship and answerability, not exclusivity) **[TEAM]**:

| Subsystem | Owner |
|---|---|
| sensord-dht, sensord-light | Zoe |
| hub: the M3 pipeline that fuses both sensors (merge, liveness, status snapshot, forwarding to store) | Gerritt |
| record format (`src/common/record.h`) | Zoe |
| store (format, recovery) | Zoe |
| analysis (drift model) | TBD |
| supervisor, dashboard | shared |

Gerritt owns the hub, the cooperating-sensors pipeline. Zoe owns everything that feeds records into the hub and on to the store (sensord, record format, store).

Claude Code is used for: exploring, drafting from approved plans, tests, adversarial review of diffs. It is not used for: the drift model's design decisions, the failure-mode table claims, or anything at runtime. Boundary kept visible by: no network client in `src/` (the only socket is the LAN listener; a CI grep checks for `connect(`/curl/http client usage), and no model files in the tree.
