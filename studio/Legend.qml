import QtQuick

// A row of colour keys: model = [{ label, color, dashed }]
Row {
  id: l
  property var theme
  property var items: []
  spacing: 14
  Repeater {
    model: l.items
    Row {
      required property var modelData
      spacing: 5
      Canvas {
        width: 16
        height: 10
        anchors.verticalCenter: parent.verticalCenter
        onPaint: {
          var c = getContext("2d")
          c.reset()
          c.strokeStyle = modelData.color
          c.lineWidth = 2
          c.setLineDash(modelData.dashed ? [3, 2] : [])
          c.beginPath(); c.moveTo(0, 5); c.lineTo(16, 5); c.stroke()
        }
      }
      Text {
        text: modelData.label
        color: l.theme ? l.theme.fg : "white"
        opacity: 0.75
        font.family: l.theme ? l.theme.font : "monospace"
        font.pixelSize: 11
      }
    }
  }
}
