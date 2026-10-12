"""Print sensor readings: python -m sensors [--fake] [--once] [--interval 2.5]"""
import argparse
import time

from . import get_sensors

parser = argparse.ArgumentParser(prog="python -m sensors")
parser.add_argument("--fake", action="store_true", help="use fake readings even on the Pi")
parser.add_argument("--once", action="store_true", help="print one reading and exit")
parser.add_argument("--interval", type=float, default=2.5, help="seconds between readings")
args = parser.parse_args()

with get_sensors(fake=True if args.fake else None) as sensors:
    try:
        while True:
            reading = sensors.read()
            if args.once and reading.temperature_f is None:
                # Missed DHT read; retry rather than print blanks.
                time.sleep(args.interval)
                continue
            print(reading, flush=True)
            if args.once:
                break
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
