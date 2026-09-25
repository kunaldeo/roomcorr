import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Io

Item {
  id: root
  property var theme
  property var daemon
  property var report: null
  property string page: {
    var t = Quickshell.env("ROOMCORR_STUDIO_TAB")
    return ["live", "response", "alignment", "calibrate"].indexOf(t) >= 0 ? t : "live"
  }

  readonly property var cfg: daemon ? daemon.cfg : null

  FileView {
    id: reportFile
    path: (Quickshell.env("XDG_DATA_HOME") || (Quickshell.env("HOME") + "/.local/share")) + "/roomcorr/response.json"
    watchChanges: true
    printErrors: false
    onFileChanged: reload()
    onLoaded: { try { root.report = JSON.parse(text()) } catch (e) { root.report = null } }
    onLoadFailed: root.report = null
  }

  // ---------------------------------------------------------------- header
  Rectangle {
    id: header
    anchors.left: parent.left
    anchors.right: parent.right
    height: 58
    color: theme.bgDark

    Row {
      anchors.left: parent.left
      anchors.leftMargin: 20
      anchors.verticalCenter: parent.verticalCenter
      spacing: 14
      Text {
        text: "\u{f1729}"
        color: theme.accent
        font.family: theme.font
        font.pixelSize: 26
        anchors.verticalCenter: parent.verticalCenter
      }
      Column {
        anchors.verticalCenter: parent.verticalCenter
        Text {
          text: "Room Correction Studio"
          color: theme.fg
          font.family: theme.font
          font.pixelSize: 16
          font.bold: true
        }
        Text {
          text: !daemon.connected ? "engine offline"
              : !cfg ? "connecting…"
              : (cfg.enabled ? "active" : "bypassed") + " · " + (cfg.room_eq ? "room EQ" : "no EQ") + " · "
                + (cfg.bass_management ? "sub " + Math.round(cfg.crossover_hz) + " Hz" : "no bass management")
                + " · " + (daemon.state ? daemon.state.filters : "")
          color: daemon.connected ? theme.muted : theme.red
          font.family: theme.font
          font.pixelSize: 11
        }
      }
    }

    Row {
      anchors.centerIn: parent
      spacing: 6
      Repeater {
        model: [
          { id: "live", label: "Live", icon: "\u{f0ea2}" },
          { id: "response", label: "Response", icon: "\u{f201}" },
          { id: "alignment", label: "Alignment", icon: "\u{f017}" },
          { id: "calibrate", label: "Calibrate", icon: "\u{f130}" }
        ]
        SButton {
          required property var modelData
          theme: root.theme
          text: modelData.label
          icon: modelData.icon
          selected: root.page === modelData.id
          implicitHeight: 34
          onClicked: root.page = modelData.id
        }
      }
    }

    Row {
      anchors.right: parent.right
      anchors.rightMargin: 20
      anchors.verticalCenter: parent.verticalCenter
      spacing: 14
      Text {
        anchors.verticalCenter: parent.verticalCenter
        text: cfg && cfg.enabled ? "Correction ON" : "Bypass"
        color: cfg && cfg.enabled ? theme.fg : theme.muted
        font.family: theme.font
        font.pixelSize: 12
        font.bold: true
      }
      SToggle {
        width: 40
        anchors.verticalCenter: parent.verticalCenter
        theme: root.theme
        checked: !!cfg && cfg.enabled
        enabled: daemon.connected
        onToggled: daemon.set("enabled", !cfg.enabled)
      }
    }
  }

  // ---------------------------------------------------------------- pages
  Item {
    anchors.top: header.bottom
    anchors.left: parent.left
    anchors.right: parent.right
    anchors.bottom: parent.bottom
    anchors.margins: 16

    LivePage { anchors.fill: parent; visible: root.page === "live"; theme: root.theme; daemon: root.daemon; report: root.report }
    ResponsePage { anchors.fill: parent; visible: root.page === "response"; theme: root.theme; daemon: root.daemon; report: root.report }
    AlignmentPage { anchors.fill: parent; visible: root.page === "alignment"; theme: root.theme; daemon: root.daemon; report: root.report }
    CalibratePage { anchors.fill: parent; visible: root.page === "calibrate"; theme: root.theme; daemon: root.daemon; report: root.report }
  }
}
