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


class RecordUnitTests(unittest.TestCase):
    def test_temp_c_x10(self):
        r = Reading(datetime(2026, 1, 1), 70.0, 50.0, 1.0)
        self.assertEqual(r.temp_c_x10(), 211)
        self.assertEqual(Reading(datetime(2026, 1, 1), 32.0, 50.0, 1.0).temp_c_x10(), 0)

    def test_rh_x10(self):
        self.assertEqual(Reading(datetime(2026, 1, 1), 70.0, 55.5, 1.0).rh_x10(), 555)

    def test_missed_read_has_no_record_units(self):
        r = Reading(datetime(2026, 1, 1), None, None, 1.0)
        self.assertIsNone(r.temp_c_x10())
        self.assertIsNone(r.rh_x10())


class HardwareSensorsTests(unittest.TestCase):
    """HardwareSensors against a fake IIO directory, with the Pi-only libraries stubbed out."""

    def setUp(self):
        import os
        import sys
        import tempfile
        from unittest import mock

        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dev = os.path.join(self.tmp.name, "iio:device0")
        os.mkdir(self.dev)
        self.write("name", "dht11\n")
        self.write("in_temp_input", "21000\n")
        self.write("in_humidityrelative_input", "55000\n")

        stubs = {name: mock.MagicMock() for name in ("adafruit_tsl2591", "board", "busio")}
        stubs["adafruit_tsl2591"].TSL2591.return_value.lux = 12.5
        patcher = mock.patch.dict(sys.modules, stubs)
        patcher.start()
        self.addCleanup(patcher.stop)
        sys.modules.pop("sensors.hardware", None)
        self.addCleanup(sys.modules.pop, "sensors.hardware", None)
        from sensors import hardware

        glob_patch = mock.patch.object(hardware, "IIO_GLOB", os.path.join(self.tmp.name, "iio:device*"))
        glob_patch.start()
        self.addCleanup(glob_patch.stop)
        self.hardware = hardware

    def write(self, name, text):
        import os

        with open(os.path.join(self.dev, name), "w") as f:
            f.write(text)

    def test_milli_units_become_degrees_and_percent(self):
        r = self.hardware.HardwareSensors().read()
        self.assertAlmostEqual(r.temperature_f, 69.8)
        self.assertEqual(r.humidity, 55.0)
        self.assertEqual(r.lux, 12.5)

    def test_dht_miss_is_none_and_recovers(self):
        s = self.hardware.HardwareSensors()
        self.write("in_temp_input", "not a number\n")
        with self.assertLogs("sensors.hardware", "WARNING"):
            r = s.read()
        self.assertIsNone(r.temperature_f)
        self.assertEqual(r.humidity, 55.0)  # a temperature miss doesn't discard humidity
        self.write("in_temp_input", "22000\n")
        self.assertIsNotNone(s.read().temperature_f)

    def test_missing_dht_device_does_not_raise(self):
        import os

        os.remove(os.path.join(self.dev, "name"))
        with self.assertLogs("sensors.hardware", "WARNING"):
            r = self.hardware.HardwareSensors().read()
        self.assertIsNone(r.temperature_f)

    def test_close_releases_i2c(self):
        s = self.hardware.HardwareSensors()
        s.close()
        self.hardware.busio.I2C.return_value.deinit.assert_called_once()
