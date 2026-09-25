#!/usr/bin/env bash
# Builds roomcorr and installs everything: engine binary + systemd user
# service, Room Correction Studio, the Omarchy bar plugin, and a Hyprland
# window rule for the Studio. Safe to re-run after changes.
set -euo pipefail
cd "$(dirname "$0")"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)"
./build/roomcorr_tests

# Engine
install -Dm755 build/roomcorr "$HOME/.local/bin/roomcorr"
install -Dm644 dist/roomcorr.service "$HOME/.config/systemd/user/roomcorr.service"
systemctl --user daemon-reload
systemctl --user enable roomcorr.service >/dev/null 2>&1
systemctl --user restart roomcorr.service

# Studio (a Quickshell app, started with `roomcorr studio`)
studio="$HOME/.local/share/roomcorr/studio"
[ -L "$studio" ] && rm "$studio"
mkdir -p "$studio"
cp -r studio/. "$studio/"

# Omarchy bar plugin. Copied, not symlinked: the shell's hot reload doesn't
# reliably follow symlinked plugin directories.
if [ -d "$HOME/.config/omarchy/plugins" ]; then
  rm -rf "$HOME/.config/omarchy/plugins/kunal.roomcorr"
  cp -r plugin/kunal.roomcorr "$HOME/.config/omarchy/plugins/"
  omarchy plugin enable kunal.roomcorr >/dev/null 2>&1 || true
  grep -q '"kunal.roomcorr"' "$HOME/.config/omarchy/shell.json" 2>/dev/null ||
    omarchy bar put kunal.roomcorr --after omarchy.audio >/dev/null 2>&1 || true
fi

# Hyprland: Studio opens as a large floating window on workspace 2.
hypr="$HOME/.config/hypr"
if [ -f "$hypr/hyprland.lua" ]; then
  cat >"$hypr/roomcorr.lua" <<'EOF'
-- Room Correction Studio (roomcorr): large centred floating window on workspace 2.
o.window({ title = "^Room Correction Studio$" }, { float = true, center = true, size = { 1480, 940 }, workspace = "2" })
EOF
  grep -q 'require("hypr.roomcorr")' "$hypr/hyprland.lua" ||
    printf '\n-- Room Correction Studio window\nrequire("hypr.roomcorr")\n' >>"$hypr/hyprland.lua"
  hyprctl reload >/dev/null 2>&1 || true
fi

echo "Installed. First time: roomcorr setup, then calibrate from the Studio (roomcorr studio calibrate)."
