#!/usr/bin/env bash
# Rebuild the Pi from a freshly flashed SD card. Safe to re-run.
# Usage (as the gerritt user, on the Pi):
#   sudo apt-get install -y gh && gh auth login    # needs admin on the repo, to register the runner
#   gh repo clone Crinsp/CS370-Term-Project && CS370-Term-Project/scripts/setup-pi.sh
set -euo pipefail

REPO="Crinsp/CS370-Term-Project"
RUNNER_DIR="$HOME/actions-runner"
RUNNER_NAME="pi"

echo "== apt packages"
sudo apt-get update
# python3-lgpio comes from the Raspberry Pi apt repo; setup-venv.sh uses it via --system-site-packages.
sudo apt-get install -y git gh rsync curl tmux i2c-tools python3-venv python3-dev \
    python3-lgpio libgpiod3 swig build-essential

echo "== enable I2C (TSL2591 is on bus 1)"
sudo raspi-config nonint do_i2c 0
sudo usermod -aG gpio,i2c "$USER"

echo "== deploy target and user services"
sudo mkdir -p /srv/cs370
sudo chown "$USER:$USER" /srv/cs370
# Lets deploy.sh run `systemctl --user` with nobody logged in.
sudo loginctl enable-linger "$USER"

echo "== Tailscale"
if ! command -v tailscale >/dev/null; then
    curl -fsSL https://tailscale.com/install.sh | sh
fi
sudo tailscale up

echo "== GitHub Actions runner"
if ! gh auth status >/dev/null 2>&1; then
    gh auth login
fi
gh auth setup-git
if [ ! -x "$RUNNER_DIR/config.sh" ]; then
    ver="$(gh api repos/actions/runner/releases/latest --jq .tag_name)"
    ver="${ver#v}"
    mkdir -p "$RUNNER_DIR"
    curl -fsSL "https://github.com/actions/runner/releases/download/v$ver/actions-runner-linux-arm64-$ver.tar.gz" \
        | tar xz -C "$RUNNER_DIR"
fi
cd "$RUNNER_DIR"
if [ ! -f .runner ]; then
    token="$(gh api -X POST "repos/$REPO/actions/runners/registration-token" --jq .token)"
    # --replace takes over the old runner's name, so deploy.yml's `runs-on: [self-hosted, pi]` keeps working.
    ./config.sh --unattended --replace --url "https://github.com/$REPO" --token "$token" \
        --name "$RUNNER_NAME" --labels pi
fi
sudo ./svc.sh install "$USER" || true   # errors if the service is already installed
sudo ./svc.sh start

echo
echo "Done. Log out and back in (or reboot) so the gpio/i2c groups apply, then check:"
echo "  i2cdetect -y 1                          # expect 0x29"
echo "  gh api repos/$REPO/actions/runners --jq '.runners[] | .name + \" \" + .status'"
echo "To redeploy the latest release: gh workflow run deploy.yml --repo $REPO --ref <tag>"
