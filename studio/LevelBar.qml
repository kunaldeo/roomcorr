import QtQuick

// Horizontal level meter: RMS bar, peak tick, dB readout.
Item {
  id: m
  property var theme
  property string label: ""
  property real peakDb: -120
  property real rmsDb: -120
  property real floorDb: -60
  property color tint: theme ? theme.fg : "white"
  implicitHeight: 18
  implicitWidth: 260

  function frac(db) { return Math.max(0, Math.min(1, (db - floorDb) / -floorDb)) }

  Text {
    id: lbl
    width: 46
    anchors.verticalCenter: parent.verticalCenter
    text: m.label
    color: theme ? theme.fg : "white"
    opacity: 0.7
    font.family: theme ? theme.font : "monospace"
    font.pixelSize: 11
  }
  Item {
    id: bar
    anchors.left: lbl.right
    anchors.right: val.left
    anchors.rightMargin: 8
    anchors.verticalCenter: parent.verticalCenter
    height: 8
    Rectangle { anchors.fill: parent; radius: 3; color: theme ? theme.alpha(theme.fg, 0.10) : "#333" }
    Rectangle {
      height: parent.height
      radius: 3
      width: parent.width * m.frac(m.rmsDb)
      color: m.peakDb > -1 ? (theme ? theme.red : "red") : m.tint
      opacity: 0.85
      Behavior on width { NumberAnimation { duration: 60 } }
    }
    Rectangle {
      visible: m.peakDb > m.floorDb
      width: 2
      height: parent.height + 4
      y: -2
      x: Math.max(0, parent.width * m.frac(m.peakDb) - 2)
      color: m.peakDb > -1 ? (theme ? theme.red : "red") : (theme ? theme.fg : "white")
    }
  }
  Text {
    id: val
    width: 58
    anchors.right: parent.right
    anchors.verticalCenter: parent.verticalCenter
    horizontalAlignment: Text.AlignRight
    text: m.peakDb > -100 ? m.peakDb.toFixed(1) + " dB" : "—"
    color: theme ? theme.fg : "white"
    font.family: theme ? theme.font : "monospace"
    font.pixelSize: 11
  }
}
