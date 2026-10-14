import unittest

from datetime import datetime

from sensors import Reading, get_sensors
from sensors.fake import FakeSensors


class FakeSensorTests(unittest.TestCase):
    def test_readings_in_realistic_range(self):
        with FakeSensors(seed=1) as s:
            for _ in range(200):
                r = s.read()
                self.assertGreaterEqual(r.lux, 0)
                if r.temperature_f is not None:
                    self.assertTrue(55 <= r.temperature_f <= 85)
                    self.assertTrue(0 <= r.humidity <= 100)

    def test_some_reads_fail_like_the_real_dht11(self):
        with FakeSensors(seed=1) as s:
            self.assertTrue(any(s.read().temperature_f is None for _ in range(200)))

    def test_get_sensors_fake_override(self):
        self.assertIsInstance(get_sensors(fake=True), FakeSensors)

    def test_fake_readings_are_labeled(self):
        with FakeSensors(seed=1) as s:
            reading = s.read()
        self.assertTrue(reading.is_fake)
        self.assertIn("[FAKE]", str(reading))


class ReadingTests(unittest.TestCase):
    def test_real_reading_has_no_fake_label(self):
        reading = Reading(datetime(2026, 1, 1, 12, 0, 0), 70.0, 50.0, 100.0)
        self.assertNotIn("FAKE", str(reading))

    def test_missed_read_prints_placeholders(self):
        reading = Reading(datetime(2026, 1, 1, 12, 0, 0), None, None, 100.0)
        self.assertIn("--°F", str(reading))


if __name__ == "__main__":
    unittest.main()
