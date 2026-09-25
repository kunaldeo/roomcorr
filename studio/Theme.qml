import QtQuick
import Quickshell
import Quickshell.Io

// Colours from the active Omarchy theme (colors.toml), live-reloaded on
// theme switches. Channel colours are fixed roles on top of the palette.
Item {
  id: t
  visible: false

  property color bg: "#1d1f21"
  property color bgDark: "#16181a"
  property color bgDarker: "#111314"
  property color bgLight: "#2a2d30"
  property color fg: "#e0e0e0"
  property color fgLight: "#c5c8c6"
  property color muted: "#707880"
  property color accent: "#81a2be"
  property color selection: "#373b41"
  property color red: "#cc6666"
  property color yellow: "#f0c674"
  property color green: "#b5bd68"
  property color cyan: "#8abeb7"
  property color magenta: "#b294bb"
  property color orange: "#de935f"
  property string font: "JetBrainsMono Nerd Font"

  readonly property color cLeft: cyan
  readonly property color cRight: magenta
  readonly property color cSub: yellow
  readonly property color cInput: muted
  readonly property color cTarget: fg
  readonly property color cBefore: muted
  readonly property color cPredicted: accent
  readonly property color cMeasured: green

  readonly property int fs: 12  // base font size

  function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

  FileView {
    path: (Quickshell.env("XDG_STATE_HOME") || (Quickshell.env("HOME") + "/.local/state")) + "/omarchy/current/theme/colors.toml"
    watchChanges: true
    printErrors: false
    onFileChanged: reload()
    onLoaded: t.parse(text())
  }

  function parse(txt) {
    var map = {}
    var lines = String(txt).split("\n")
    for (var i = 0; i < lines.length; i++) {
      var m = lines[i].match(/^\s*([a-z_]+)\s*=\s*"(#[0-9a-fA-F]{6})"/)
      if (m) map[m[1]] = m[2]
    }
    if (map.background) bg = map.background
    if (map.dark_background) bgDark = map.dark_background
    if (map.darker_background) bgDarker = map.darker_background
    if (map.lighter_background) bgLight = map.lighter_background
    if (map.foreground) fg = map.foreground
    if (map.light_foreground) fgLight = map.light_foreground
    if (map.muted) muted = map.muted
    if (map.accent) accent = map.accent
    if (map.selection) selection = map.selection
    if (map.red) red = map.red
    if (map.yellow) yellow = map.yellow
    if (map.green) green = map.green
    if (map.cyan) cyan = map.cyan
    if (map.magenta) magenta = map.magenta
    if (map.orange) orange = map.orange
  }
}
