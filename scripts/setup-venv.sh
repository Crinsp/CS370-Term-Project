#!/usr/bin/env bash
# Create/update a Python venv with the project's dependencies.
# Usage: scripts/setup-venv.sh [venv-dir]   (default: .venv in the repo)
# On the Pi it also installs the hardware libraries.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
venv="${1:-$repo/.venv}"

if grep -qs "Raspberry Pi" /proc/device-tree/model; then
    # --system-site-packages lets the venv use the apt python3-lgpio package;
    # building lgpio from pip needs liblgpio-dev, which isn't installed.
    [ -d "$venv" ] || python3 -m venv --system-site-packages "$venv"
    reqs="$repo/requirements-pi.txt"
else
    [ -d "$venv" ] || python3 -m venv "$venv"
    reqs="$repo/requirements.txt"
fi

"$venv/bin/pip" install --quiet --disable-pip-version-check -r "$reqs"
echo "venv ready: $venv"
