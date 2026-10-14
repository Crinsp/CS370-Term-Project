# Design Document (M2): Gecko Enclosure Humidity Advisor

Authors: Gerritt Ewing, Zoe Ebert. Status: DRAFT. Items marked **[PI]** need real evidence from the Pi; items marked **[TEAM]** need a decision from both partners.

User: Rosalie Endris, keeper of a gecko. The device is advisory only: it senses, logs, analyzes and displays on a LAN dashboard. It never controls the humidifier.

## 1. Architecture

```
 kernel:  I2C subsystem (/dev/i2c-1)            GPIO (DHT11 on GPIO4)
              |                                        |
        [sensord-light]  ~2 Hz               [sensord-dht]  ~0.5 Hz
              |  UDS, 1 msg/sample                     |  UDS, 1 msg/sample
              +----------------+-----------------------+
                               v
                           [hub]  merges by timestamp, tags source, tracks per-sensor liveness
                               | 1 Hz fused record (shared-memory ring is a later option)
                +--------------+---------------+
                v                              v
            [store]  append-only log      [analysis]  drift model, band-exit prediction
             fsync batch, CRC, seq            | ~1 Hz status
                                              v
                                        [dashboard]  LAN HTTP, read-only, serves status + history
 [supervisor]: parent of all of the above; restarts on exit/hang; writes heartbeat to log
```

Rates are targets, to be confirmed against measurements **[PI]**. DHT11 cannot be read faster than about 1 Hz and about 10% of reads fail; the light sensor is cheap and fast, so the two sources deliberately run at different rates and the hub reconciles them.

Every process is a separate C binary under `src/`. Python is limited to the dashboard front end and tooling.

## 2. Mechanism mapping

| Menu item | Component | Justification from Rosalie's needs |
|---|---|---|
| **E** multi-process + supervisor | `sensord-*`, `hub`, `supervisor` | A dead sensor must never look like "humidity is fine". Each sensor is isolated so a hung DHT11 read cannot stall light sampling or the dashboard, and the supervisor restarts it and logs the gap. The dashboard reports the sensor as dead or stale. |
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
| hub | exits | supervisor | restart; sensord reconnect UDS | `hub restart` |
| store | power cut mid-write | CRC/length check at startup scan | truncate to last good record; continue | `recover truncated <bytes>` |
| store | disk full | `write` returns ENOSPC | stop appending, dashboard flag, keep sensing | `store ENOSPC` |
| supervisor | dies | systemd (or runner) restarts it; children detect parent loss and exit | clean restart; orphan fds closed | `supervisor start gen=<n>` |
| clock | jump (no RTC, NTP step) | monotonic vs wall delta | order by monotonic + seq, wall time is advisory | `clock step <s>` |
| any | sensor value implausible | range / rate-of-change check | mark SUSPECT, do not feed model | `dht suspect rh=<v>` |

Rule from CLAUDE.md: stale, dead or implausible data is shown as such, never as the last good value.

## 4. Storage and data

- Record: fixed 32 bytes: `seq u64, mono_ns u64, wall_s u32, source u8 (dht/light), flags u8 (real/fake/suspect), temp_c i16 (x10), rh u16 (x10), lux f32, crc32 u32`. Exact layout **[TEAM]**.
- Rate: about 1.5 records/s combined, about 4 MB/day.
- Retention: keep 30 days, rotate by segment file; the dashboard reads history from the log, not from memory.
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

**[PI]** Evidence to paste here: `i2cdetect -y 1` (expect 0x29), `python -m sensors --once` with real values, measured DHT11 miss rate over about 1 hour.

## 6. Evaluation plan

| Measurement | Method | Target |
|---|---|---|
| Sample-to-log latency distribution | timestamp at sensord read vs at log append; p50/p95/p99, idle and under CPU stress | p99 under 100 ms (to confirm) |
| Per-process CPU and RSS | heartbeat series over the 48 h soak | RSS flat (no leak), CPU under 5% |
| Domain metric: band-exit prediction | N provoked episodes labeled by us; compare drift model with a plain threshold on lead time, misses, false alarms | model alerts earlier than threshold with no more false alarms |
| Fault injection | `kill -9` each child, unplug each sensor 10 min, power cut mid-write | each row in section 3 produces its log line and recovers |
| Durability | cut power at random during writes, repeat M times | zero corrupt records read back; loss window within 5 s |

## 7. Ownership and AI-use plan

Ownership (first authorship and answerability, not exclusivity) **[TEAM]**:

| Subsystem | Owner |
|---|---|
| sensord-dht, sensord-light | TBD |
| store (format, recovery) | TBD |
| hub + analysis (drift model) | TBD |
| supervisor, dashboard | shared |

Claude Code is used for: exploring, drafting from approved plans, tests, adversarial review of diffs. It is not used for: the drift model's design decisions, the failure-mode table claims, or anything at runtime. Boundary kept visible by: no network client in `src/` (the only socket is the LAN listener; a CI grep checks for `connect(`/curl/http client usage), and no model files in the tree.
