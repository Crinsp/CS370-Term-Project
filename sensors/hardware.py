"""Real sensors. Only importable on the Pi (needs the packages in requirements-pi.txt).

Wiring:
  TSL2591 light sensor  -> I2C bus 1 (SDA = pin 3, SCL = pin 5), address 0x29
  DHT11 temp/humidity   -> data on GPIO4 (pin 7), + on 3.3V (pin 1), - on GND

The DHT11 is read through the kernel dht11 IIO driver (`dtoverlay=dht11,gpiopin=4`
in config.txt, see scripts/setup-pi.sh), which owns GPIO4. Don't use adafruit_dht.
"""
import glob
import logging
import os
from datetime import datetime

import adafruit_tsl2591
import board
import busio

from .backend import SensorBackend
from .reading import Reading

IIO_GLOB = "/sys/bus/iio/devices/iio:device*"

log = logging.getLogger(__name__)


def _find_dht11_iio() -> str:
    for dev in sorted(glob.glob(IIO_GLOB)):
        try:
            with open(os.path.join(dev, "name")) as f:
                if f.read().strip() == "dht11":
                    return dev
        except OSError:
            continue
    raise RuntimeError("no dht11 IIO device; is dtoverlay=dht11,gpiopin=4 set and the Pi rebooted?")


def _read_milli(dev: str, name: str) -> float:
    """The IIO *_input files are in milli-units (milli-degC, milli-%RH). [PI] confirm."""
    with open(os.path.join(dev, name)) as f:
        return int(f.read().strip()) / 1000


class HardwareSensors(SensorBackend):
    def __init__(self):
        self._i2c = busio.I2C(board.SCL, board.SDA)
        try:
            self._tsl = adafruit_tsl2591.TSL2591(self._i2c)
        except Exception:
            self._i2c.deinit()
            raise
        # Found lazily and re-found after a failure: the iio:deviceN index can change if the driver re-probes.
        self._dht: str | None = None

    def _dht_read(self, name: str) -> float | None:
        """One DHT11 value, or None on a miss. Logs why, so a dead sensor isn't mistaken for an ordinary miss."""
        try:
            if self._dht is None:
                self._dht = _find_dht11_iio()
            return _read_milli(self._dht, name)
        except (OSError, ValueError, RuntimeError) as e:
            # The kernel returns an error on an ordinary miss (ETIMEDOUT/EIO); look the device up again next time.
            log.warning("dht11 %s read failed: %s: %s", name, type(e).__name__, e)
            self._dht = None
            return None

    def _lux(self) -> float:
        try:
            return self._tsl.lux
        except (OSError, RuntimeError) as e:
            log.warning("tsl2591 read failed: %s: %s", type(e).__name__, e)
            return float("nan")

    def read(self) -> Reading:
        temp_c = self._dht_read("in_temp_input")
        humidity = self._dht_read("in_humidityrelative_input")
        temp_f = temp_c * 9 / 5 + 32 if temp_c is not None else None
        return Reading(datetime.now(), temp_f, humidity, self._lux())

    def close(self) -> None:
        self._i2c.deinit()
