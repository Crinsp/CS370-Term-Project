# CS370-Term-Project

See README.md for the full workflow. Key points:

- Work on a branch and merge via PR. Never push to `main` directly. Only a published GitHub **release** deploys to the Pi.
- Access sensors only through `sensors.get_sensors()`. Off the Pi it returns fake readings, so the code runs anywhere.
- Hardware libraries (`adafruit-*`) go in `requirements-pi.txt` only; everything else goes in `requirements.txt`.
- Run tests with `python -m unittest discover -s tests`.
- On the Pi: never edit `/srv/cs370` (it's the deployed release). Use a personal checkout under `~/dev/<name>`. Only one process can use the DHT11 (GPIO4) at a time.
