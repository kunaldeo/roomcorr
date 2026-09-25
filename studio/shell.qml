import QtQuick
import Quickshell

// Room Correction Studio: `roomcorr studio` (qs -p <this dir>).
ShellRoot {
  FloatingWindow {
    id: win
    title: "Room Correction Studio"
    implicitWidth: 1480
    implicitHeight: 940
    color: theme.bg

    Theme { id: theme }
    Daemon { id: daemon }

    Studio {
      anchors.fill: parent
      theme: theme
      daemon: daemon
    }

    onVisibleChanged: if (!visible) Qt.quit()
  }
}
