#!/usr/bin/env bash
# Runs on the Pi from /srv/cs370 whenever a release is published.
set -euo pipefail

# Kept outside /srv/cs370 because the deploy rsync --delete would wipe it.
VENV="$HOME/.local/share/cs370/venv"

./scripts/setup-venv.sh "$VENV"

# Check the code imports cleanly. This doesn't touch the GPIO pins, so it won't
# clash with anyone testing on the hardware.
"$VENV/bin/python" -c "import sensors.hardware"

# Once there's a long-running app, install it as a user service
# (see README "Running the app as a service") and it gets restarted here.
if systemctl --user cat cs370.service >/dev/null 2>&1; then
    systemctl --user restart cs370.service
    echo "restarted cs370.service"
fi

echo "${GITHUB_REF_NAME:-unknown}" > VERSION
echo "Deployed ${GITHUB_REF_NAME:-unknown} (${GITHUB_SHA:-unknown}) at $(date)"
