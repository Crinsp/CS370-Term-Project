from dataclasses import dataclass
from datetime import datetime


@dataclass
class Reading:
    timestamp: datetime
    # None when the DHT11 misses a read, which happens every so often.
    temperature_f: float | None
    humidity: float | None
    lux: float
    # True for synthesized data; it must stay labeled everywhere it appears.
    is_fake: bool = False

    def temp_c_x10(self) -> int | None:
        """Record units (degC x10); the one place the F -> C boundary conversion lives."""
        if self.temperature_f is None:
            return None
        return round((self.temperature_f - 32) * 5 / 9 * 10)

    def rh_x10(self) -> int | None:
        """Record units (%RH x10)."""
        return None if self.humidity is None else round(self.humidity * 10)

    def __str__(self) -> str:
        temp = f"{self.temperature_f:.1f}°F" if self.temperature_f is not None else "--°F"
        hum = f"{self.humidity:.1f}% RH" if self.humidity is not None else "--% RH"
        label = "  [FAKE]" if self.is_fake else ""
        return f"{self.timestamp:%H:%M:%S}  {temp}  {hum}  {self.lux:.1f} lux{label}"
