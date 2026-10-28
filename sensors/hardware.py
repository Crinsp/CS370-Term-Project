"""Real sensors. Only importable on the Pi (needs the packages in requirements-pi.txt).

Wiring:
  TSL2591 light sensor  -> I2C bus 1 (SDA = pin 3, SCL = pin 5), address 0x29
  DHT11 temp/humidity   -> data on GPIO4 (pin 7), + on 3.3V (pin 1), - on GND

The DHT11 is read through the kernel dht11 IIO driver (`dtoverlay=dht11,gpiopin=4`
in config.txt, see scripts/setup-pi.sh), which owns GPIO4. Don't use adafruit_dht.
"""
import glob
import os
from datetime import datetime

import adafruit_tsl2591
import board
import busio

from .backend import SensorBackend
from .reading import Reading

IIO_GLOB = "/sys/bus/iio/devices/iio:device*"


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
        self._tsl = adafruit_tsl2591.TSL2591(busio.I2C(board.SCL, board.SDA))
        self._dht = _find_dht11_iio()

    def read(self) -> Reading:
        try:
            temp_c = _read_milli(self._dht, "in_temp_input")
            humidity = _read_milli(self._dht, "in_humidityrelative_input")
        except (OSError, ValueError):
            # DHT reads fail intermittently (the kernel returns an error); callers just get None this time.
            temp_c = humidity = None
        temp_f = temp_c * 9 / 5 + 32 if temp_c is not None else None
        return Reading(datetime.now(), temp_f, humidity, self._tsl.lux)

    def close(self) -> None:
        pass
