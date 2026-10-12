"""Real sensors. Only importable on the Pi (needs the packages in requirements-pi.txt).

Wiring:
  TSL2591 light sensor  -> I2C bus 1 (SDA = pin 3, SCL = pin 5), address 0x29
  DHT11 temp/humidity   -> data on GPIO4 (pin 7), + on 3.3V (pin 1), - on GND
"""
import os
from datetime import datetime

import adafruit_dht
import adafruit_tsl2591
import board
import busio

from .reading import Reading

DHT_PIN = int(os.environ.get("CS370_DHT_PIN", "4"))
DHT_MODEL = os.environ.get("CS370_DHT_MODEL", "11")


class HardwareSensors:
    def __init__(self):
        self._tsl = adafruit_tsl2591.TSL2591(busio.I2C(board.SCL, board.SDA))
        dht_cls = adafruit_dht.DHT22 if DHT_MODEL == "22" else adafruit_dht.DHT11
        # pulseio mode (the default) is the one that works on the Pi 4;
        # use_pulseio=False never sees the sensor respond.
        self._dht = dht_cls(getattr(board, f"D{DHT_PIN}"))

    def read(self) -> Reading:
        try:
            temp_c, humidity = self._dht.temperature, self._dht.humidity
        except RuntimeError:
            # DHT reads fail intermittently; callers just get None this time.
            temp_c = humidity = None
        temp_f = temp_c * 9 / 5 + 32 if temp_c is not None else None
        return Reading(datetime.now(), temp_f, humidity, self._tsl.lux)

    def close(self) -> None:
        self._dht.exit()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
