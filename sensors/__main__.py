"""Print sensor readings: python -m sensors [--fake] [--once] [--interval 2.5]"""
import argparse
import time

from . import get_sensors


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(prog="python -m sensors")
    parser.add_argument("--fake", action="store_true", help="use fake readings even on the Pi")
    parser.add_argument("--once", action="store_true", help="print one reading and exit")
    parser.add_argument("--interval", type=float, default=2.5, help="seconds between readings")
    return parser.parse_args()


def print_once(sensors, interval: float) -> None:
    """Print the first complete reading, retrying while the DHT11 misses."""
    while True:
        reading = sensors.read()
        if reading.temperature_f is not None:
            print(reading, flush=True)
            return
        time.sleep(interval)


def print_forever(sensors, interval: float) -> None:
    while True:
        print(sensors.read(), flush=True)
        time.sleep(interval)


def main() -> None:
    args = parse_args()
    with get_sensors(fake=True if args.fake else None) as sensors:
        try:
            if args.once:
                print_once(sensors, args.interval)
            else:
                print_forever(sensors, args.interval)
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
