from abc import ABC, abstractmethod

from .reading import Reading


class SensorBackend(ABC):
    """What every sensor source (real or fake) provides. Use it in a `with` block."""

    @abstractmethod
    def read(self) -> Reading:
        """Take one reading. Never raises on a missed DHT11 read; fields are None."""

    @abstractmethod
    def close(self) -> None:
        """Release the hardware."""

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
