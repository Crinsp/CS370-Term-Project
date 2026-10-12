import unittest

from sensors import get_sensors
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


if __name__ == "__main__":
    unittest.main()
