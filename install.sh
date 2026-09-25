#!/usr/bin/env bash
# Builds roomcorr, installs the binary and systemd user service, and links
# the Omarchy plugin. Safe to re-run after changes.
set -euo pipefail
cd "$(dirname "$0")"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)"
./build/roomcorr_tests

install -Dm755 build/roomcorr "$HOME/.local/bin/roomcorr"
install -Dm644 dist/roomcorr.service "$HOME/.config/systemd/user/roomcorr.service"
systemctl --user daemon-reload
systemctl --user enable roomcorr.service >/dev/null 2>&1
systemctl --user restart roomcorr.service

# Omarchy plugin: symlink so edits here hot-reload in the shell.
if [ -d "$HOME/.config/omarchy/plugins" ]; then
  ln -sfn "$PWD/plugin/kunal.roomcorr" "$HOME/.config/omarchy/plugins/kunal.roomcorr"
  if ! grep -q '"kunal.roomcorr"' "$HOME/.config/omarchy/shell.json" 2>/dev/null; then
    omarchy bar put kunal.roomcorr --before omarchy.audio >/dev/null 2>&1 || true
  fi
fi

echo "Installed. Next: roomcorr setup (once), then roomcorr calibrate."
