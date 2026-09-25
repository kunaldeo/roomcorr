import QtQuick

// Labelled slider. Emits moved() while dragging (throttled by the caller's
// needs) and released() at the end; double-click resets to `defaultValue`.
Item {
  id: s
  property var theme
  property string label: ""
  property string valueText: ""
  property real from: 0
  property real to: 1
  property real step: 0.1
  property real value: 0
  property real defaultValue: NaN
  property color tint: theme ? theme.accent : "white"
  property bool dragging: mouse.pressed
  property real live: value
  signal moved(real value)
  signal released(real value)

  onValueChanged: if (!dragging) live = value
  implicitHeight: 40
  implicitWidth: 240
  opacity: enabled ? 1 : 0.4

  function snap(v) {
    var r = Math.round((v - from) / step) * step + from
    return Math.max(from, Math.min(to, Math.round(r * 1000) / 1000))
  }
  readonly property real frac: (live - from) / (to - from)

  Text {
    id: lbl
    text: s.label
    color: theme ? theme.fg : "white"
    opacity: 0.7
    font.family: theme ? theme.font : "monospace"
    font.pixelSize: 12
  }
  Text {
    anchors.right: parent.right
    text: s.valueText
    color: theme ? theme.fg : "white"
    font.family: theme ? theme.font : "monospace"
    font.pixelSize: 12
    font.bold: true
  }
  Item {
    id: track
    anchors.left: parent.left
    anchors.right: parent.right
    anchors.bottom: parent.bottom
    height: 20
    Rectangle {
      anchors.verticalCenter: parent.verticalCenter
      width: parent.width
      height: 4
      radius: 2
      color: theme ? theme.alpha(theme.fg, 0.14) : "#444"
    }
    Rectangle {
      anchors.verticalCenter: parent.verticalCenter
      width: Math.max(4, parent.width * s.frac)
      height: 4
      radius: 2
      color: s.tint
    }
    Rectangle {
      width: 14
      height: 14
      radius: 7
      x: Math.max(0, Math.min(parent.width - width, parent.width * s.frac - width / 2))
      anchors.verticalCenter: parent.verticalCenter
      color: theme ? theme.fg : "white"
      border.color: s.tint
      border.width: 2
    }
    MouseArea {
      id: mouse
      anchors.fill: parent
      anchors.margins: -6
      preventStealing: true
      function at(mx) { return s.snap(s.from + Math.max(0, Math.min(1, (mx - 6) / track.width)) * (s.to - s.from)) }
      onPressed: function(e) { s.live = at(e.x); s.moved(s.live) }
      onPositionChanged: function(e) {
        if (!pressed) return
        var v = at(e.x)
        if (v !== s.live) { s.live = v; s.moved(v) }
      }
      onReleased: s.released(s.live)
      onDoubleClicked: if (!isNaN(s.defaultValue)) { s.live = s.defaultValue; s.moved(s.live); s.released(s.live) }
    }
  }
}
