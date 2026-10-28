# 3. Failure-mode table

| Component | Failure | Detection | System response | Log shows |
|---|---|---|---|---|
| sensord-dht | single read fails (checksum/timeout, ~10%) | read returns error | retry once after 2 s; report miss, no value | `dht miss n=<consecutive>` |
| sensord-dht | sensor unplugged / dead | N consecutive misses or no sample for 60 s | mark DEAD; dashboard banner; other sensor keeps running | `dht DEAD since <ts>` |
| sensord-light | I2C NACK / bus error | errno from ioctl/read | reinit device; after N failures mark DEAD | `light err errno=<n>` |
| any sensord | process killed (`kill -9`) | supervisor `waitpid` | restart with backoff, new generation id | `child dht died sig=9 restart=<n>` |
| any sensord | hang (alive, no output) | no heartbeat/sample within deadline | supervisor SIGKILL then restart | `child dht hung, killed` |
| hub | exits | supervisor; sensords get `EPIPE`/`ECONNREFUSED` | restart; sensords count dropped samples and reconnect (0.5 s up to 8 s) | `hub restart`, `sensord dht hub lost, dropped=<n>` |
| hub | store down / not yet started | `connect` fails or `EPIPE` on send | hold up to 256 records, reconnect with backoff, drop oldest on overflow | `hub store lost`, `hub store drop n=<total>` |
| hub | malformed record from a client | size != 32, CRC mismatch or unknown source | reject the record, keep the connection | `hub reject src=<fd> reason=crc` |
| sensord | dht11 driver/overlay missing | sysfs path absent at startup | exit 1 (supervisor logs and backs off); dashboard shows DHT DEAD | `sensord dht no iio device` |
| store | power cut mid-write | CRC/length check at startup scan | truncate to last good record; continue | `recover truncated <bytes>` |
| store | disk full | `write` returns ENOSPC | stop appending, dashboard flag, keep sensing | `store ENOSPC` |
| supervisor | dies | systemd (or runner) restarts it; children detect parent loss and exit | clean restart; orphan fds closed | `supervisor start gen=<n>` |
| clock | jump (no RTC, NTP step) | monotonic vs wall delta | order by monotonic + seq; wall time is advisory | `clock step <s>` |
| any | sensor value implausible | range / rate-of-change check | mark SUSPECT, do not feed model | `dht suspect rh=<v>` |

Rule from CLAUDE.md: stale, dead or implausible data is shown as such, never as the last good value. The log strings are the planned formats; the C daemons are not yet implemented.
