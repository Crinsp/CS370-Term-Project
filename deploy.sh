#!/usr/bin/env bash
# Runs on the Pi from /srv/cs370 after every push to main.
# Add install/build/restart steps here once the stack is chosen, e.g.:
#   npm ci && npm run build
#   sudo systemctl restart cs370
set -euo pipefail

echo "Deployed $(git -C "$GITHUB_WORKSPACE" rev-parse --short HEAD 2>/dev/null || echo '?') at $(date)"
