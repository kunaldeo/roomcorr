import QtQuick
import QtQuick.Layouts

// Titled panel. Children go into the content column.
Rectangle {
  id: card
  property var theme
  property string title: ""
  property string subtitle: ""
  default property alias content: body.data
  property alias bodySpacing: body.spacing

  color: theme ? theme.bgDark : "#222"
  radius: 8
  border.color: theme ? theme.alpha(theme.fg, 0.08) : "#333"
  implicitHeight: body.implicitHeight + header.implicitHeight + 28

  Column {
    id: header
    x: 14
    y: 10
    width: parent.width - 28
    spacing: 1
    visible: card.title !== ""
    Text {
      text: card.title.toUpperCase()
      color: theme ? theme.fg : "white"
      opacity: 0.55
      font.family: theme ? theme.font : "monospace"
      font.pixelSize: 11
      font.bold: true
      font.letterSpacing: 1.4
    }
    Text {
      visible: card.subtitle !== ""
      width: parent.width
      wrapMode: Text.WordWrap
      text: card.subtitle
      color: theme ? theme.muted : "gray"
      font.family: theme ? theme.font : "monospace"
      font.pixelSize: 11
    }
  }
  Column {
    id: body
    x: 14
    anchors.top: card.title !== "" ? header.bottom : card.top
    anchors.topMargin: card.title !== "" ? 10 : 12
    width: parent.width - 28
    spacing: 8
  }
}
