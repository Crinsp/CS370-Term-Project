"""Fake sensors for developing on a PC. Same interface as HardwareSensors."""
import math
import random
from datetime import datetime

from .backend import SensorBackend
from .reading import Reading

TEMP_F_RANGE = (60.0, 80.0)
HUMIDITY_RANGE = (25.0, 70.0)
MAX_TEMP_STEP_F = 0.5
MAX_HUMIDITY_STEP = 1.0
MAX_LUX = 400
LUX_NOISE = 5
DHT_MISS_RATE = 0.1


def _drift(value: float, max_step: float, low_high: tuple[float, float], rng: random.Random) -> float:
    """Move value by a small random step, staying inside the range."""
    low, high = low_high
    return min(high, max(low, value + rng.uniform(-max_step, max_step)))


class FakeSensors(SensorBackend):
    def __init__(self, seed: int | None = None):
        self._rng = random.Random(seed)
        self._temp_f = 68.0
        self._humidity = 45.0

    def read(self) -> Reading:
        now = datetime.now()
        # Drift slowly like a real room instead of jumping around.
        self._temp_f = _drift(self._temp_f, MAX_TEMP_STEP_F, TEMP_F_RANGE, self._rng)
        self._humidity = _drift(self._humidity, MAX_HUMIDITY_STEP, HUMIDITY_RANGE, self._rng)
        lux = self._daylight_lux(now)
        # The real DHT11 only reports whole °C and sometimes misses a read.
        if self._rng.random() < DHT_MISS_RATE:
            return Reading(now, None, None, lux, is_fake=True)
        temp_c = round((self._temp_f - 32) * 5 / 9)
        return Reading(now, temp_c * 9 / 5 + 32, round(self._humidity), lux, is_fake=True)

    def _daylight_lux(self, now: datetime) -> float:
        """Bright around midday, dark at night."""
        hour = now.hour + now.minute / 60
        daylight = max(0.0, math.sin((hour - 6) / 12 * math.pi))
        return round(daylight * MAX_LUX + self._rng.uniform(0, LUX_NOISE), 1)

    def close(self) -> None:
        pass
