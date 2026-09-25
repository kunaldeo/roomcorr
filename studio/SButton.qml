import QtQuick

Rectangle {
  id: b
  property var theme
  property string text: ""
  property string icon: ""
  property bool primary: false
  property bool selected: false
  property color tint: theme ? theme.accent : "white"
  signal clicked()

  implicitHeight: 30
  implicitWidth: row.implicitWidth + 24
  radius: 6
  opacity: enabled ? 1 : 0.4
  color: primary ? tint
       : selected ? (theme ? theme.alpha(tint, 0.22) : "#444")
       : ma.containsMouse ? (theme ? theme.alpha(theme.fg, 0.10) : "#333") : "transparent"
  border.color: primary ? tint : selected ? tint : (theme ? theme.alpha(theme.fg, 0.18) : "#555")
  border.width: 1

  Row {
    id: row
    anchors.centerIn: parent
    spacing: 7
    Text {
      visible: b.icon !== ""
      text: b.icon
      color: b.primary ? (theme ? theme.bg : "black") : (theme ? theme.fg : "white")
      font.family: theme ? theme.font : "monospace"
      font.pixelSize: 14
      anchors.verticalCenter: parent.verticalCenter
    }
    Text {
      text: b.text
      color: b.primary ? (theme ? theme.bg : "black") : (theme ? theme.fg : "white")
      font.family: theme ? theme.font : "monospace"
      font.pixelSize: 12
      font.bold: b.primary || b.selected
      anchors.verticalCenter: parent.verticalCenter
    }
  }
  MouseArea {
    id: ma
    anchors.fill: parent
    hoverEnabled: true
    cursorShape: Qt.PointingHandCursor
    onClicked: if (b.enabled) b.clicked()
  }
}
