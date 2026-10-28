# 5. Constraints and substitutions

| Ideal | Actual | Cost |
|---|---|---|
| SHT45 RH/temp (+/-1% RH, 0.1 C) | DHT11 (about +/-5% RH, whole-percent RH, 0.1 C steps seen, 1 Hz, ~10% missed reads **[Pi: unverified]**) | cannot resolve small drifts; the model works on trends over minutes and claims are narrowed accordingly |
| DS18B20 second temperature | none | no independent temperature cross-check; the light sensor is the second source |
| Actuator-in-the-loop control | advisory only | no closed-loop result to measure; evaluation is prediction quality, not control quality |
| Months of enclosure data | days of provoked episodes | claims limited to episodes we induce (lamp on/off, vent open, misting) |
| DHT11 read with timing we control | kernel `dht11` IIO driver (`dtoverlay=dht11,gpiopin=4`) via sysfs | the bit timing is the kernel's, not ours (not one of our chosen mechanisms). Bit-banging 26-70 us pulses from userspace is where scheduling jitter breaks reads. The overlay claims GPIO4, so Python `adafruit_dht` no longer works on the Pi. Fallback: libgpiod v2 edge timestamps |

## Pi evidence

Host `piewing`, kernel 6.18.50+rpt-rpi-v8 aarch64, libgpiod 2.2.1.

### TSL2591 (2026-10-27)

Checked with raw `i2c-tools` using the same register addresses the C backend uses.

- `i2cdetect -y 1` shows 0x29 and nothing else; the ID register (0xB2) reads 0x50; after ENABLE = PON|AEN the AVALID bit is set; channel counts were near 0 at medium gain in a dark room and steady, non-zero at maximum gain, and scale with the gain setting.
- On this bus SMBus word reads return only one byte per register, so the C backend reads C0DATAL..C1DATAH as one 4-byte block starting at command 0xB4.
- Lux: with C0 (visible + IR) and C1 (IR), `CPL = ATIME_ms * G / 408.0` (G: medium 25x, max 9876x) and `lux = max((C0 - C1) * (1 - C1/C0), 0) / CPL`. A saturated channel (36863 counts at 100 ms, 65535 at longer times) is reported SUSPECT. Gain and integration time are fixed at startup so lux stays comparable.

### DHT11 (2026-10-28)

- Before the change, `/boot/firmware/config.txt` had no `dht11` line and `/sys/bus/iio/devices/` was empty.
- With `dtoverlay=dht11,gpiopin=4` added under `[all]` and a reboot, the kernel exposes `/sys/bus/iio/devices/iio:device0` named `dht11@4`. The name carries the GPIO number, so sensord must match the `dht11` prefix, not the exact name.
- 20 reads, 2 s apart, returned `in_temp_input` = 17800 to 18300 and `in_humidityrelative_input` = 39000 to 47000.
- These are milli-units (17.8 to 18.3 degC, 39 to 47 %RH), so dividing by 100 gives the record's x10 fields.
- Temperature moved in 0.1 degC steps and humidity in whole percent.
- No read failed in the 20, so the ~10% miss rate is still an unverified estimate **[Pi: unverified]**; a longer run is pending.
