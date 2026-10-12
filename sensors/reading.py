from dataclasses import dataclass
from datetime import datetime


@dataclass
class Reading:
    timestamp: datetime
    # None when the DHT11 misses a read, which happens every so often.
    temperature_f: float | None
    humidity: float | None
    lux: float

    def __str__(self) -> str:
        temp = f"{self.temperature_f:.1f}°F" if self.temperature_f is not None else "--°F"
        hum = f"{self.humidity:.1f}% RH" if self.humidity is not None else "--% RH"
        return f"{self.timestamp:%H:%M:%S}  {temp}  {hum}  {self.lux:.1f} lux"
