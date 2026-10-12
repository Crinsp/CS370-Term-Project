"""Fake sensors for developing on a PC. Same interface as HardwareSensors."""
import math
import random
from datetime import datetime

from .reading import Reading


class FakeSensors:
    def __init__(self, seed: int | None = None):
        self._rng = random.Random(seed)
        self._temp_f = 68.0
        self._humidity = 45.0

    def read(self) -> Reading:
        now = datetime.now()
        # Drift slowly like a real room instead of jumping around.
        self._temp_f = min(80.0, max(60.0, self._temp_f + self._rng.uniform(-0.5, 0.5)))
        self._humidity = min(70.0, max(25.0, self._humidity + self._rng.uniform(-1, 1)))
        # Bright around midday, dark at night, like the kitchen.
        hour = now.hour + now.minute / 60
        daylight = max(0.0, math.sin((hour - 6) / 12 * math.pi))
        lux = round(daylight * 400 + self._rng.uniform(0, 5), 1)
        # The real DHT11 only reports whole °C and sometimes misses a read.
        if self._rng.random() < 0.1:
            return Reading(now, None, None, lux)
        temp_c = round((self._temp_f - 32) * 5 / 9)
        return Reading(now, temp_c * 9 / 5 + 32, round(self._humidity), lux)

    def close(self) -> None:
        pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
