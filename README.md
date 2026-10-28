# CS370-Term-Project — Koba Cage

A Raspberry Pi 4 terrarium monitor for Koba, a crested gecko. It uses a DHT11 temperature/humidity sensor and a TSL2591 light sensor.

**Team:** Zoe Ebert, Gerritt Ewing

## The problem

Koba's owner, Rose, mists the cage by hand twice a day. Each misting should bring humidity up to 80–90%, and between mistings it should sit around 55%. Cheap automatic misters run on a timer, not on measured humidity. Misting by hand means Rose can't tell whether the cage is actually in range. This matters more in a dry climate like Colorado. Commercial monitoring setups (sensors plus an app) are expensive.

## What the device does

- **Senses** temperature, humidity, and light inside the cage, 24/7.
- **Logs and analyzes** the readings and compares humidity and temperature against Koba's target conditions.
- **Shows** current conditions and alerts on a dashboard served only on the local network.
- **Advisory only:** it never controls a mister, heater, or light. Rose stays in control and the device tells her when to act.
- **Works offline:** no cloud services and no outbound network calls. It keeps working with the network cable unplugged.
- **Honest about failures:** if a sensor is dead, stale, or giving implausible readings, the dashboard says so instead of showing an old value as current.

## Safety

The biggest risk is harming Koba if the system is wrong. The device is advisory and all development happens away from the cage. It goes into Koba's cage only after it has been tested.

## How we work (the short version)

```
your PC                    GitHub                         the Pi
───────                    ──────                         ──────
code on a branch  ──push──▶ Pull Request
(fake sensors)              │  tests run automatically
                            │  partner reviews + approves
                            ▼
                           main  (merging does NOT deploy)
                            │
                            │  when we're happy with main:
                            ▼
                           Release v0.x.0  ───────────▶  runner deploys it
                                                          to /srv/cs370
```

- **Everyday coding happens on your own PC.** You don't need the Pi, because the code fakes the sensor readings when it's not running on one.
- **The Pi is only for testing with the real sensors.** Each of us has our own copy of the repo there, so we don't overwrite each other's work.
- **Nothing reaches the "live" Pi until we publish a release.** Merging into `main` is safe, and we decide together when to cut a release.

## Workflow (step by step)

1. Branch off `main`: `git switch -c my-feature`
2. Develop on your own PC. The code automatically uses **fake sensors** when it isn't on a Pi.
3. Need the real sensors? Test your branch in your own checkout on the Pi (see below).
4. Push and open a pull request. The **Tests** workflow runs on it. Get a teammate's approval, then merge into `main`.
   - The **Claude Review** workflow also reviews the PR and comments. If it goes red there are blocking issues: fix them, push, and it re-runs.
   - Nothing stops a direct push to `main` (branch protection needs GitHub Pro on a private repo), so stick to PRs.
5. Merging does **not** deploy. When `main` is at a good point, **publish a release** and that version deploys to the Pi:
   ```
   gh release create v0.1.0 --target main --generate-notes
   ```
   Or use the GitHub web UI: Releases → Draft a new release → choose a new tag like `v0.1.0`.
   - Pre-releases don't deploy.
   - To roll back: Actions → **Deploy to Pi** → Run workflow → pick an older tag.

Deploys run on the Pi's self-hosted runner (`.github/workflows/deploy.yml`). The workflow syncs the release into `/srv/cs370` and then runs `deploy.sh`. The deployed version is in `/srv/cs370/VERSION`. Watch it in the **Actions** tab.

## Developing on your PC

```
git clone https://github.com/Crinsp/CS370-Term-Project.git
cd CS370-Term-Project
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt        # Windows: .venv\Scripts\pip ...
.venv/bin/python -m sensors                      # prints fake readings
.venv/bin/python -m unittest discover -s tests   # run tests
```

Requires Python 3.10+ (the Pi runs 3.13).

The C core builds with `make` (output in `build/`); `make test` runs its tests and `make asan` reruns them under AddressSanitizer. Both also run in CI.

In code, always get sensors through `get_sensors()`, so the same code works on both PCs and the Pi:

```python
from sensors import get_sensors

with get_sensors() as s:
    r = s.read()   # r.temperature_f, r.humidity, r.lux (temp/humidity may be None on a missed read)
```

Set `CS370_FAKE_SENSORS=1` to force fake sensors (even on the Pi), or `0` to force real ones.

## Testing on the Pi

Connect over SSH/Tailscale (or Claude Code remote control), then use **your own checkout**:

```
mkdir -p ~/dev/<your-name> && cd ~/dev/<your-name>
git clone https://github.com/Crinsp/CS370-Term-Project.git && cd CS370-Term-Project
git switch <your-branch>
scripts/setup-venv.sh          # on the Pi this also installs the hardware libraries
.venv/bin/python -m sensors    # real readings, Ctrl+C to stop
```

Rules:
- **Never edit `/srv/cs370`.** That's the deployed release, and the next deploy overwrites it.
- **Only one program can use the DHT11 at a time.** Tell your teammates before running anything on the real sensors.
- If you see `Unable to set line 4 to input`, a leftover process is holding the pin. Clear it with `pkill -f libgpiod_pulsein`.

## Hardware

| Sensor | Connection |
|---|---|
| TSL2591 light | I2C bus 1: SDA pin 3, SCL pin 5, address `0x29` |
| DHT11 temp/humidity (3-wire board) | data → GPIO4 (pin 7), + → **3.3V** (pin 1), − → GND |

Check the I2C sensor with `i2cdetect -y 1`. Override the DHT settings with the `CS370_DHT_PIN` / `CS370_DHT_MODEL` env vars.

## Rebuilding the Pi from scratch

If the SD card dies, nothing on the Pi needs saving except your own `~/.claude/projects` transcripts. Everything else is rebuilt from this repo:

1. Flash **Raspberry Pi OS (64-bit, trixie)** with Raspberry Pi Imager. In its settings, set hostname `piewing`, user `gerritt`, Wi-Fi, and **enable SSH**.
2. Boot it, log in, and run:
   ```
   sudo apt-get install -y gh && gh auth login
   gh repo clone Crinsp/CS370-Term-Project && CS370-Term-Project/scripts/setup-pi.sh
   ```
   `setup-pi.sh` installs the packages, enables I2C, creates `/srv/cs370`, enables lingering, joins Tailscale (it prints a login link), and re-registers the `pi` Actions runner in place of the old one.
3. Reboot, then redeploy: Actions → **Deploy to Pi** → Run workflow → pick the latest release tag. Or publish a new release.

The new install has new SSH host keys, so on your PC run `ssh-keygen -R piewing` (and the same for its IP) before reconnecting.

## Running the app as a service

`deploy.sh` restarts a systemd **user** service named `cs370.service` if one is installed (lingering is already enabled for the `gerritt` user). Once there's a long-running app, install it on the Pi:

```
mkdir -p ~/.config/systemd/user
cat > ~/.config/systemd/user/cs370.service <<'EOF'
[Unit]
Description=CS370 app

[Service]
WorkingDirectory=/srv/cs370
ExecStart=%h/.local/share/cs370/venv/bin/python -m <your_app_module>
Restart=on-failure

[Install]
WantedBy=default.target
EOF
systemctl --user daemon-reload && systemctl --user enable --now cs370.service
```

Note: a service that reads the sensors holds the DHT11 pin, so stop it (`systemctl --user stop cs370`) before testing hardware from your own checkout.
