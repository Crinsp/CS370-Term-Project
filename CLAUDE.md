# CS370-Term-Project

See README.md for the full workflow. Key points:

- Work on a branch and merge via PR. Never push to `main` directly. Only a published GitHub **release** deploys to the Pi.
- Access sensors only through `sensors.get_sensors()`. Off the Pi it returns fake readings, so the code runs anywhere.
- Hardware libraries (`adafruit-*`) go in `requirements-pi.txt` only; everything else goes in `requirements.txt`.
- Run tests with `python -m unittest discover -s tests`.
- C core: `make` builds to `build/`, `make test` runs `tests/c/`, `make asan` reruns them under ASan+UBSan. Sources live in `src/<component>/main.c`; shared code in `src/common/`.
- On the Pi: never edit `/srv/cs370` (it's the deployed release). Use a personal checkout under `~/dev/<name>`. The DHT11 is read via the kernel `dht11` IIO driver (sysfs), not `adafruit_dht`.

## Hard constraints (graded; team law)
- The PRODUCT makes no network calls except serving its own LAN interface. No LLM APIs, no cloud inference, no pretrained models. All analysis/control logic is code we wrote. The device must work with the network cable unplugged.
- The device is ADVISORY ONLY: it senses, logs, analyzes and displays; it never controls the humidifier. No actuator code. Alerts appear only on the LAN-served dashboard (no push, email, or outbound requests). If a sensor is dead, stale, or implausible, the dashboard must say so plainly rather than show a last-good value as current.
- Every daemon must be supervisable: clean exit codes, no orphaned fds across restart, heartbeat within 60s of start.
- Every allocation checked; every syscall's error path handled and logged.
- NEVER weaken, skip, or delete a test to make the suite pass.
- Replayed or synthesized (fake) data must be labeled as such everywhere it appears (logs, reports, demo).

## Style
- Systems core: C17, `-Wall -Wextra -Werror`, no VLAs, goto-cleanup for multi-resource functions. Graded mechanisms live in C.
- Python is permitted only for tooling and the UI (the existing `sensors/` package, dev fakes, dashboard). No graded mechanism may live there.
- Smallest diff that passes. Do not refactor unrelated code.

## Ownership (TBD: fill in at M2; ownership = first authorship + answerability, not exclusivity)
- Gerritt: hub (`src/hub/`), the M3 pipeline that fuses both sensors
- Zoe: sensord (`src/sensord/`), record format (`src/common/record.h`), store (`src/store/`)
- Shared: supervisor, interface. The agent edits outside the current session owner's area only when told explicitly.

## Course process
- Plan first for multi-file or algorithmic changes; wait for approval.
- Hardware bugs: paste real evidence (dmesg, timing capture, /proc/interrupts, i2cdetect). No fixes proposed from a verbal description.
- Every milestone diff gets an agent review and a human review by the partner who didn't write it.
- At every milestone each partner copies their raw `~/.claude/projects/<dir>/*.jsonl` transcripts out (30-day purge). Keep PROMPTLOG.md (6-10 annotated episodes).
- Target >= 40 meaningful commits, both partners represented.
