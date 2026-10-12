"""Sensor access for the project.

Always go through get_sensors() instead of importing a backend directly, so the
same code runs on the Pi (real hardware) and on your PC (fake readings).

    from sensors import get_sensors

    with get_sensors() as s:
        print(s.read())
"""
import os

from .reading import Reading

__all__ = ["Reading", "get_sensors", "on_pi"]


def on_pi() -> bool:
    try:
        with open("/proc/device-tree/model") as f:
            return "Raspberry Pi" in f.read()
    except OSError:
        return False


def get_sensors(fake: bool | None = None):
    """Return the real sensors on a Pi, fake ones everywhere else.

    Override with fake=True/False or the CS370_FAKE_SENSORS=1/0 env var.
    """
    if fake is None:
        env = os.environ.get("CS370_FAKE_SENSORS")
        fake = env == "1" if env in ("0", "1") else not on_pi()
    if fake:
        from .fake import FakeSensors

        return FakeSensors()
    from .hardware import HardwareSensors

    return HardwareSensors()
