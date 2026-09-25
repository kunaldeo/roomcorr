import QtQuick

// Label + description + switch.
Item {
  id: t
  property var theme
  property string label: ""
  property string detail: ""
  property bool checked: false
  property color tint: theme ? theme.accent : "white"
  signal toggled()

  implicitHeight: Math.max(col.implicitHeight, 22)
  implicitWidth: 240
  opacity: enabled ? 1 : 0.4

  Column {
    id: col
    anchors.left: parent.left
    anchors.right: sw.left
    anchors.rightMargin: 10
    anchors.verticalCenter: parent.verticalCenter
    Text {
      text: t.label
      color: theme ? theme.fg : "white"
      font.family: theme ? theme.font : "monospace"
      font.pixelSize: 12
    }
    Text {
      visible: t.detail !== ""
      text: t.detail
      width: parent.width
      wrapMode: Text.WordWrap
      color: theme ? theme.muted : "gray"
      font.family: theme ? theme.font : "monospace"
      font.pixelSize: 10
    }
  }
  Rectangle {
    id: sw
    anchors.right: parent.right
    anchors.verticalCenter: parent.verticalCenter
    width: 38
    height: 20
    radius: 10
    color: t.checked ? t.tint : (theme ? theme.alpha(theme.fg, 0.15) : "#444")
    Behavior on color { ColorAnimation { duration: 120 } }
    Rectangle {
      width: 14
      height: 14
      radius: 7
      y: 3
      x: t.checked ? parent.width - width - 3 : 3
      color: theme ? theme.bg : "black"
      Behavior on x { NumberAnimation { duration: 120 } }
    }
  }
  MouseArea {
    anchors.fill: parent
    cursorShape: Qt.PointingHandCursor
    onClicked: t.toggled()
  }
}
