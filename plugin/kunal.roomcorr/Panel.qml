import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Services.Pipewire
import qs.Commons
import qs.Ui
import "Model.js" as Model

// Room Correction: bar widget + panel for the roomcorr engine.
// Talks to the daemon over its unix socket (newline-delimited JSON): a
// subscription streams meters, and settings go back as {"cmd":"set"}.
Panel {
  id: root
  moduleName: "kunal.roomcorr"
  ipcTarget: "kunal.roomcorr"

  readonly property string socketPath: (Quickshell.env("XDG_RUNTIME_DIR") || "/tmp") + "/roomcorr.sock"
  readonly property string responsePath: (Quickshell.env("XDG_DATA_HOME") || (Quickshell.env("HOME") + "/.local/share")) + "/roomcorr/response.json"
  readonly property int meterMs: Math.max(30, setting("meterRefreshMs", 60))

  property bool connected: false
  property var state: null            // last {"type":"state"} message
  property var status: null           // last {"type":"status"} message
  property var report: null           // response.json
  property string graphView: "bass"
  property bool designing: false

  readonly property var cfg: state && state.config ? state.config : null
  readonly property bool enabled: !!cfg && cfg.enabled
  readonly property bool muted: !!cfg && cfg.mute

  readonly property color foreground: bar ? bar.foreground : Color.foreground
  readonly property color urgent: bar ? bar.urgent : Color.urgent
  readonly property color accent: Color.accent
  readonly property color dim: Qt.darker(foreground, 1.4)
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family

  // ---------------------------------------------------------------- daemon

  function send(obj) {
    if (!sock.connected) return
    sock.write(JSON.stringify(obj) + "\n")
    sock.flush()
  }

  function setValue(key, value) {
    var v = {}
    v[key] = value
    send({ cmd: "set", values: v })
  }

  function subscribe() {
    send({ cmd: "subscribe", interval_ms: root.opened ? root.meterMs : 1000 })
  }

  function handle(line) {
    var msg
    try { msg = JSON.parse(line) } catch (e) { return }
    if (msg.type === "state") root.state = msg
    else if (msg.type === "status") root.status = msg
  }

  Socket {
    id: sock
    path: root.socketPath
    connected: true
    parser: SplitParser { onRead: function(line) { root.handle(line) } }
    onConnectionStateChanged: {
      root.connected = sock.connected
      if (sock.connected) root.subscribe()
    }
  }

  // Reconnect when the engine restarts.
  Timer {
    interval: 2000
    running: !sock.connected
    repeat: true
    onTriggered: { sock.connected = false; sock.connected = true }
  }

  onOpenedChanged: {
    subscribe()
    if (opened) responseFile.reload()
  }

  FileView {
    id: responseFile
    path: root.responsePath
    watchChanges: true
    printErrors: false
    onFileChanged: reload()
    onLoaded: {
      try { root.report = JSON.parse(text()) } catch (e) { root.report = null }
      graph.requestPaint()
    }
    onLoadFailed: root.report = null
  }

  // Rebuilds filters after a target-curve change (no re-measuring).
  Process {
    id: designProc
    command: ["roomcorr", "design"]
    onExited: root.designing = false
  }

  function redesign() {
    if (designProc.running) return
    root.designing = true
    designProc.running = true
  }

  function openStudio(tab) {
    Quickshell.execDetached(["roomcorr", "studio", tab || "live"])
    root.close()
  }

  // ---------------------------------------------------------------- volume

  readonly property var sinkNode: {
    var nodes = Pipewire.nodes ? Pipewire.nodes.values : []
    for (var i = 0; i < nodes.length; i++)
      if (nodes[i].name === "roomcorr_sink") return nodes[i]
    return null
  }
  PwObjectTracker { objects: root.sinkNode ? [root.sinkNode] : [] }
  readonly property real volume: sinkNode && sinkNode.audio ? sinkNode.audio.volume : 0
  readonly property bool volumeMuted: sinkNode && sinkNode.audio ? sinkNode.audio.muted : false

  // ---------------------------------------------------------------- bar

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  readonly property real openPanelIndicatorWidth: button.labelWidth
  readonly property real openPanelIndicatorHeight: Math.max(Style.space(10), Math.round(Style.bar.iconSlot * 0.55))

  WidgetButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: "󰓃"
    labelVisible: !vertical
    hasVisualContent: true
    // Tinted while bypassed/offline, so a glance says the EQ isn't running.
    active: !root.connected || !root.enabled || root.muted
    horizontalMargin: 8.75
    tooltipText: Model.statusLine(root.connected, root.state)
    onPressed: function(b) {
      if (b === Qt.RightButton) root.setValue("enabled", !root.enabled)
      else if (b === Qt.MiddleButton) root.setValue("mute", !root.muted)
      else root.toggle()
    }

    OpticalGlyph {
      visible: button.vertical
      anchors.centerIn: parent
      width: Style.bar.iconSlot
      height: Style.bar.iconSlot
      text: "󰓃"
      fontFamily: button.fontFamily
      fontSize: Style.bar.iconFont
      color: button.active ? button.activeColor : button.foreground
    }
  }

  // ---------------------------------------------------------------- panel

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: keyCatcher
    contentWidth: panel.fittedContentWidth(Style.space(380))
    contentHeight: panel.fittedContentHeight(column.implicitHeight)

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }

      Column {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: Style.space(12)

        // ---------- hero: title, status, master switch
        Item {
          width: parent.width
          implicitHeight: Math.max(heroIcon.implicitHeight, heroText.implicitHeight, masterSwitch.implicitHeight)

          Text {
            id: heroIcon
            textFormat: Text.PlainText
            text: "󰓃"
            color: root.enabled && root.connected ? root.foreground : root.dim
            font.family: root.fontFamily
            font.pixelSize: Style.font.display
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
          }

          Column {
            id: heroText
            anchors.left: heroIcon.right
            anchors.leftMargin: Style.space(14)
            anchors.right: masterSwitch.left
            anchors.rightMargin: Style.space(10)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Style.space(2)

            Text {
              textFormat: Text.PlainText
              text: "Room Correction"
              color: root.foreground
              font.family: root.fontFamily
              font.pixelSize: Style.font.title
              font.bold: true
            }
            Text {
              textFormat: Text.PlainText
              text: Model.statusLine(root.connected, root.state)
              color: root.connected ? root.dim : root.urgent
              font.family: root.fontFamily
              font.pixelSize: Style.font.caption
              font.bold: true
              font.letterSpacing: 1.2
              elide: Text.ElideRight
              width: parent.width
            }
          }

          ToggleSwitch {
            id: masterSwitch
            checked: root.enabled
            foreground: root.foreground
            interactive: root.connected
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            onToggled: root.setValue("enabled", !root.enabled)

            PanelToolTip {
              visible: masterSwitch.containsMouse
              text: root.enabled ? "Bypass (plain stereo, level-matched)" : "Enable room correction"
            }
          }
        }

        // ---------- engine offline
        Column {
          visible: !root.connected
          width: parent.width
          spacing: Style.space(8)

          Text {
            width: parent.width
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: "The roomcorr engine isn't running. Audio falls back to the Sound Blaster directly."
            color: root.foreground
            opacity: 0.7
            font.family: root.fontFamily
            font.pixelSize: Style.font.bodySmall
          }
          Button {
            width: parent.width
            text: "Start engine"
            iconText: ""
            foreground: root.foreground
            fontFamily: root.fontFamily
            fontSize: Style.font.bodySmall
            bordered: true
            onClicked: Quickshell.execDetached(["systemctl", "--user", "restart", "roomcorr"])
          }
        }

        // ---------- meters
        Column {
          visible: root.connected
          width: parent.width
          spacing: Style.space(5)

          PanelSectionHeader {
            text: "LEVELS"
            foreground: root.foreground
            fontFamily: root.fontFamily
          }

          LevelMeter { label: "In L"; meter: root.status ? root.status.in[0] : null }
          LevelMeter { label: "In R"; meter: root.status ? root.status.in[1] : null }
          LevelMeter { label: "Left"; meter: root.status ? root.status.out[0] : null }
          LevelMeter { label: "Right"; meter: root.status ? root.status.out[1] : null }
          LevelMeter { label: "Sub"; meter: root.status ? root.status.out[2] : null }

          Row {
            width: parent.width
            spacing: Style.space(8)
            InfoLabel { text: "Limiter" }
            InfoValue {
              text: root.status && root.status.limiter_db > 0.05 ? "−" + root.status.limiter_db.toFixed(1) + " dB" : "idle"
              color: root.status && root.status.limiter_db > 0.05 ? root.urgent : root.foreground
            }
            Item { width: Style.space(10); height: 1 }
            InfoLabel { text: "Headroom" }
            InfoValue { text: root.state ? Model.fmtDb(root.state.preamp_db) : "—" }
            Item { width: Style.space(10); height: 1 }
            InfoLabel { text: "DSP" }
            InfoValue { text: root.status ? Math.round(root.status.load * 100) + "%" : "—" }
          }
        }

        PanelSeparator { visible: root.connected; foreground: root.foreground }

        // ---------- controls
        Column {
          visible: root.connected && !!root.cfg
          width: parent.width
          spacing: Style.space(6)

          ControlSlider {
            label: "Volume"
            valueText: root.volumeMuted ? "muted" : Model.fmtPercent(root.volume)
            minimum: 0; maximum: 1; step: 0.02
            value: root.volume
            enabled: !!root.sinkNode
            onMoved: function(v) { if (root.sinkNode && root.sinkNode.audio) root.sinkNode.audio.volume = v }
            onRightClicked: if (root.sinkNode && root.sinkNode.audio) root.sinkNode.audio.muted = !root.sinkNode.audio.muted
          }

          ControlSlider {
            label: "Sub level"
            valueText: root.cfg ? Model.fmtDb(root.cfg.sub_gain_db) : "—"
            minimum: -12; maximum: 12; step: 0.5
            value: root.cfg ? root.cfg.sub_gain_db : 0
            enabled: !!root.cfg && root.cfg.bass_management
            onMoved: function(v) { root.setValue("sub_gain_db", v) }
            onRightClicked: root.setValue("sub_gain_db", 0)
          }

          ControlSlider {
            label: "Crossover"
            valueText: root.cfg ? Model.fmtHz(root.cfg.crossover_hz) : "—"
            minimum: 40; maximum: 150; step: 5
            value: root.cfg ? root.cfg.crossover_hz : 80
            enabled: !!root.cfg && root.cfg.bass_management
            onMoved: function(v) { root.setValue("crossover_hz", v) }
            onRightClicked: root.setValue("crossover_hz", 80)
          }

          ControlSlider {
            label: "Bass"
            valueText: root.cfg ? Model.fmtDb(root.cfg.bass_db) : "—"
            minimum: -8; maximum: 8; step: 0.5
            value: root.cfg ? root.cfg.bass_db : 0
            onMoved: function(v) { root.setValue("bass_db", v) }
            onRightClicked: root.setValue("bass_db", 0)
          }

          ControlSlider {
            label: "Treble"
            valueText: root.cfg ? Model.fmtDb(root.cfg.treble_db) : "—"
            minimum: -8; maximum: 8; step: 0.5
            value: root.cfg ? root.cfg.treble_db : 0
            onMoved: function(v) { root.setValue("treble_db", v) }
            onRightClicked: root.setValue("treble_db", 0)
          }

          SwitchRow {
            label: "Room EQ"
            detail: root.state ? "filters " + root.state.filters : ""
            checked: !!root.cfg && root.cfg.room_eq
            onToggled: root.setValue("room_eq", !root.cfg.room_eq)
          }
          SwitchRow {
            label: "Bass management"
            detail: "bass below the crossover goes to the sub"
            checked: !!root.cfg && root.cfg.bass_management
            onToggled: root.setValue("bass_management", !root.cfg.bass_management)
          }
          SwitchRow {
            label: "Crossover slope 48 dB/oct"
            detail: "off = 24 dB/oct (LR4)"
            checked: !!root.cfg && root.cfg.crossover_slope === 48
            onToggled: root.setValue("crossover_slope", root.cfg.crossover_slope === 48 ? 24 : 48)
          }
        }

        PanelSeparator { visible: root.connected; foreground: root.foreground }

        // ---------- response graph
        Column {
          width: parent.width
          spacing: Style.space(6)

          Row {
            width: parent.width
            spacing: Style.space(8)
            PanelSectionHeader {
              text: "RESPONSE"
              foreground: root.foreground
              fontFamily: root.fontFamily
            }
            InfoLabel {
              text: root.report ? "calibrated " + Model.relativeAge(root.report.created)
                                  + (root.report.verified ? " · verified " + Model.relativeAge(root.report.verified.created) : "")
                                : "not calibrated yet"
            }
          }

          ButtonGroup {
            options: [
              { value: "bass", label: "Bass" },
              { value: "left", label: "Left" },
              { value: "right", label: "Right" },
              { value: "sub", label: "Sub" }
            ]
            value: root.graphView
            foreground: root.foreground
            fontFamily: root.fontFamily
            fontSize: Style.font.bodySmall
            focusable: false
            onChanged: function(v) { root.graphView = v; graph.requestPaint() }
          }

          Canvas {
            id: graph
            width: parent.width
            height: Style.space(170)
            visible: !!root.report
            renderStrategy: Canvas.Cooperative
            onWidthChanged: requestPaint()

            onPaint: {
              var ctx = getContext("2d")
              ctx.reset()
              var g = Model.graphSeries(root.report, root.graphView)
              var r = Model.graphRange(g)
              var padL = 28, padB = 14, w = width - padL, h = height - padB
              var lx0 = Math.log(g.fmin), lx1 = Math.log(g.fmax)
              function X(f) { return padL + (Math.log(f) - lx0) / (lx1 - lx0) * w }
              function Y(db) { return (1 - (db - r.lo) / (r.hi - r.lo)) * h }

              ctx.font = Math.round(Style.font.caption * 0.9) + "px " + root.fontFamily
              ctx.fillStyle = Util.alpha(root.foreground, 0.5)
              ctx.strokeStyle = Util.alpha(root.foreground, 0.1)
              ctx.lineWidth = 1
              var ticks = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]
              for (var t = 0; t < ticks.length; t++) {
                var f = ticks[t]
                if (f < g.fmin || f > g.fmax) continue
                ctx.beginPath(); ctx.moveTo(X(f), 0); ctx.lineTo(X(f), h); ctx.stroke()
                ctx.fillText(f >= 1000 ? (f / 1000) + "k" : String(f), X(f) - 6, height - 2)
              }
              for (var db = r.lo; db <= r.hi; db += 10) {
                ctx.beginPath(); ctx.moveTo(padL, Y(db)); ctx.lineTo(width, Y(db)); ctx.stroke()
                ctx.fillText(String(db), 0, Y(db) + 4)
              }

              var styles = {
                target: { color: root.accent, width: 1.5, dash: [4, 3], alpha: 0.9 },
                before: { color: root.foreground, width: 1, dash: [], alpha: 0.35 },
                after: { color: root.foreground, width: 2, dash: [], alpha: 0.95 },
                measured: { color: root.urgent, width: 1.5, dash: [], alpha: 0.9 }
              }
              for (var s = 0; s < g.series.length; s++) {
                var ser = g.series[s], st = styles[ser.style]
                if (!ser.values) continue
                ctx.strokeStyle = Util.alpha(st.color, st.alpha)
                ctx.lineWidth = st.width
                ctx.setLineDash(st.dash)
                ctx.beginPath()
                var started = false
                for (var i = 0; i < ser.freqs.length; i++) {
                  var fq = ser.freqs[i]
                  if (fq < g.fmin || fq > g.fmax || ser.values[i] === null) continue
                  var y = Math.max(0, Math.min(h, Y(ser.values[i])))
                  if (!started) { ctx.moveTo(X(fq), y); started = true } else ctx.lineTo(X(fq), y)
                }
                ctx.stroke()
              }
              ctx.setLineDash([])
            }
          }

          Row {
            visible: !!root.report
            spacing: Style.space(12)
            LegendItem { label: "Target"; color: root.accent; dashed: true }
            LegendItem { label: "Before"; color: Util.alpha(root.foreground, 0.35) }
            LegendItem { label: "Predicted"; color: root.foreground }
            LegendItem { label: "Measured"; color: root.urgent; visible: !!root.report && !!root.report.verified }
          }

          ControlSlider {
            visible: !!root.cfg && !!root.report
            label: "House curve bass"
            valueText: root.designing ? "rebuilding…" : (root.cfg ? Model.fmtDb(root.cfg.target.bass_boost_db) : "—")
            minimum: 0; maximum: 10; step: 0.5
            value: root.cfg ? root.cfg.target.bass_boost_db : 4
            onReleased: function(v) {
              root.setValue("target.bass_boost_db", v)
              saveThenDesign.restart()
            }
          }
        }

        // ---------- actions
        Row {
          width: parent.width
          spacing: Style.space(8)

          Button {
            width: (parent.width - parent.spacing) / 2
            text: "Open Studio"
            iconText: "\u{f0ea2}"
            foreground: root.foreground
            fontFamily: root.fontFamily
            fontSize: Style.font.bodySmall
            bordered: true
            onClicked: root.openStudio("live")
          }
          Button {
            width: (parent.width - parent.spacing) / 2
            text: "Calibrate…"
            iconText: "\uf130"
            foreground: root.foreground
            fontFamily: root.fontFamily
            fontSize: Style.font.bodySmall
            bordered: true
            onClicked: root.openStudio("calibrate")
          }
        }
      }
    }
  }

  // The daemon saves settings ~1 s after a change; design reads them from
  // disk, so wait for that before rebuilding.
  Timer {
    id: saveThenDesign
    interval: 1500
    onTriggered: root.redesign()
  }

  // ---------------------------------------------------------------- components

  component LevelMeter: Row {
    property string label: ""
    property var meter: null
    readonly property real peakDb: meter ? meter.peak : -120
    readonly property real rmsDb: meter ? meter.rms : -120

    width: parent.width
    spacing: Style.space(8)

    InfoLabel { text: label; width: Style.space(38) }

    Item {
      width: parent.width - Style.space(38) - Style.space(62) - parent.spacing * 2
      height: Style.space(7)
      anchors.verticalCenter: parent.verticalCenter

      Rectangle {
        id: track
        anchors.fill: parent
        radius: height / 2
        color: Util.alpha(root.foreground, 0.12)
      }
      Rectangle {
        height: parent.height
        radius: track.radius
        color: peakDb > -1 ? root.urgent : root.foreground
        opacity: 0.85
        width: Math.max(0, parent.width * Model.meterFraction(rmsDb, -60))
        Behavior on width { NumberAnimation { duration: 80 } }
      }
      Rectangle {
        width: 2
        height: parent.height
        color: peakDb > -1 ? root.urgent : root.foreground
        x: Math.max(0, parent.width * Model.meterFraction(peakDb, -60) - 2)
        visible: peakDb > -60
      }
    }

    InfoValue {
      width: Style.space(62)
      horizontalAlignment: Text.AlignRight
      text: peakDb > -100 ? peakDb.toFixed(1) + " dB" : "—"
    }
  }

  component ControlSlider: Column {
    id: cs
    property string label: ""
    property string valueText: ""
    property alias minimum: slider.minimum
    property alias maximum: slider.maximum
    property alias step: slider.step
    property alias value: slider.value
    signal moved(real value)
    signal released(real value)
    signal rightClicked()

    width: parent.width
    spacing: Style.space(1)

    Row {
      width: parent.width
      InfoLabel { text: cs.label }
      Item { width: Math.max(0, parent.width - parent.children[0].implicitWidth - parent.children[2].implicitWidth); height: 1 }
      InfoValue { text: cs.valueText }
    }
    PanelSlider {
      id: slider
      bar: root.bar
      width: parent.width
      opacity: cs.enabled ? 1 : 0.4
      onMoved: function(v) { cs.moved(Math.round(v / cs.step) * cs.step) }
      onReleased: function(v) { cs.released(Math.round(v / cs.step) * cs.step) }
      onRightClicked: cs.rightClicked()
    }
  }

  component SwitchRow: Item {
    id: sr
    property string label: ""
    property string detail: ""
    property bool checked: false
    signal toggled()

    width: parent.width
    implicitHeight: Math.max(srText.implicitHeight, srSwitch.implicitHeight)

    Column {
      id: srText
      anchors.left: parent.left
      anchors.right: srSwitch.left
      anchors.rightMargin: Style.space(8)
      anchors.verticalCenter: parent.verticalCenter
      InfoValue { text: sr.label }
      InfoLabel { text: sr.detail; visible: sr.detail !== ""; font.pixelSize: Style.font.caption }
    }
    ToggleSwitch {
      id: srSwitch
      checked: sr.checked
      foreground: root.foreground
      trackHeight: Math.max(16, Math.round(Style.spacing.controlHeight * 0.42))
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      onToggled: sr.toggled()
    }
  }

  component LegendItem: Row {
    property string label: ""
    property color color: root.foreground
    property bool dashed: false
    spacing: Style.space(4)
    Rectangle {
      width: Style.space(12)
      height: 2
      color: parent.color
      opacity: parent.dashed ? 0.8 : 1
      anchors.verticalCenter: parent.verticalCenter
    }
    InfoLabel { text: parent.label; font.pixelSize: Style.font.caption }
  }

  component InfoLabel: Text {
    textFormat: Text.PlainText
    color: root.foreground
    opacity: 0.6
    font.family: root.fontFamily
    font.pixelSize: Style.font.bodySmall
  }

  component InfoValue: Text {
    textFormat: Text.PlainText
    color: root.foreground
    font.family: root.fontFamily
    font.pixelSize: Style.font.bodySmall
  }
}
