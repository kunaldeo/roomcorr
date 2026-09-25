import QtQuick
import QtQuick.Layouts
import Quickshell.Services.Pipewire

// Live view: what the engine is doing right now.
Item {
  id: page
  property var theme
  property var daemon
  property var report

  readonly property var cfg: daemon ? daemon.cfg : null
  readonly property var st: daemon ? daemon.status : null
  readonly property bool on: !!cfg && cfg.enabled
  readonly property var chanCfg: cfg ? cfg.channels : null

  function fmtDb(v) { return v === undefined || v === null ? "—" : (v > 0 ? "+" : "") + Number(v).toFixed(1) + " dB" }
  function meter(list, i) { return st && st[list] && st[list][i] ? st[list][i] : { peak: -120, rms: -120 } }

  // PipeWire volume of our sink (what the keyboard volume keys change).
  readonly property var sinkNode: {
    var nodes = Pipewire.nodes ? Pipewire.nodes.values : []
    for (var i = 0; i < nodes.length; i++) if (nodes[i].name === "roomcorr_sink") return nodes[i]
    return null
  }
  PwObjectTracker { objects: page.sinkNode ? [page.sinkNode] : [] }
  readonly property real volume: sinkNode && sinkNode.audio ? sinkNode.audio.volume : 0

  RowLayout {
    anchors.fill: parent
    spacing: 16

    // ---------------------------------------------------------- left
    ColumnLayout {
      Layout.fillWidth: true
      Layout.fillHeight: true
      spacing: 16

      Card {
        theme: page.theme
        title: "Live spectrum"
        subtitle: "1/6-octave RTA of the signal entering the engine and each output after crossover, EQ and delays"
        Layout.fillWidth: true
        Layout.fillHeight: true

        Graph {
          width: parent.width
          height: Math.max(220, page.height - 250)
          theme: page.theme
          xMin: 20
          xMax: 20000
          yMin: -90
          yMax: 0
          yUnit: "dBFS"
          markers: page.cfg && page.cfg.bass_management
                   ? [{ x: page.cfg.crossover_hz, color: page.theme.accent, label: "crossover " + Math.round(page.cfg.crossover_hz) + " Hz" }]
                   : []
          series: [
            { xs: daemon.freqs, ys: daemon.spec["in"], color: page.theme.fg, width: 1, alpha: 0.35, fill: true, fillAlpha: 0.08 },
            { xs: daemon.freqs, ys: daemon.spec["left"], color: page.theme.cLeft, width: 1.8 },
            { xs: daemon.freqs, ys: daemon.spec["right"], color: page.theme.cRight, width: 1.8 },
            { xs: daemon.freqs, ys: daemon.spec["sub"], color: page.theme.cSub, width: 2.2, fill: true, fillAlpha: 0.18 },
            { xs: daemon.freqs, ys: daemon.hold["sub"], color: page.theme.cSub, width: 1, alpha: 0.45, dash: [2, 3] }
          ]
        }
        Legend {
          theme: page.theme
          items: [
            { label: "Input (L+R)", color: page.theme.alpha(page.theme.fg, 0.5) },
            { label: "Left", color: page.theme.cLeft },
            { label: "Right", color: page.theme.cRight },
            { label: "Sub", color: page.theme.cSub },
            { label: "Sub peak hold", color: page.theme.cSub, dashed: true }
          ]
        }
      }

      Card {
        theme: page.theme
        title: "Signal flow"
        Layout.fillWidth: true

        Row {
          id: flow
          width: parent.width
          spacing: 0
          readonly property real bw: (width - 6 * 22 - 280) / 6

          FlowBlock {
            width: flow.bw; theme: page.theme; active: true
            title: "Input"; detail: "5.1 / stereo\n48 kHz"
          }
          Arrow { theme: page.theme }
          FlowBlock {
            width: flow.bw; theme: page.theme
            active: page.on && !!page.cfg && (page.cfg.bass_db !== 0 || page.cfg.treble_db !== 0)
            title: "Tone"
            detail: page.cfg ? "bass " + page.fmtDb(page.cfg.bass_db) + "\ntreble " + page.fmtDb(page.cfg.treble_db) : ""
          }
          Arrow { theme: page.theme }
          FlowBlock {
            width: flow.bw; theme: page.theme
            active: page.on && !!page.cfg && page.cfg.bass_management
            title: "Crossover"
            detail: page.cfg ? Math.round(page.cfg.crossover_hz) + " Hz · LR" + (page.cfg.crossover_slope === 48 ? "8" : "4")
                               + "\nLFE +10 dB → sub" : ""
          }
          Arrow { theme: page.theme }
          FlowBlock {
            width: flow.bw; theme: page.theme
            active: page.on && !!page.cfg && page.cfg.room_eq && daemon.state && daemon.state.filters === "loaded"
            title: "Room EQ"
            detail: daemon.state ? "262k-tap FIR ×3\nfilters " + daemon.state.filters : ""
          }
          Arrow { theme: page.theme }
          FlowBlock {
            width: flow.bw; theme: page.theme
            active: page.on
            title: "Time & level"
            detail: page.chanCfg
                    ? "sub " + Number(page.chanCfg.sub.delay_ms).toFixed(1) + " ms" + (page.chanCfg.sub.invert ? " · inv" : "")
                      + "\nsub trim " + page.fmtDb(page.chanCfg.sub.trim_db + (page.cfg.sub_gain_db || 0))
                    : ""
          }
          Arrow { theme: page.theme }
          FlowBlock {
            width: flow.bw; theme: page.theme
            active: page.on && !!page.cfg && page.cfg.limiter
            warn: !!page.st && page.st.limiter_db > 0.05
            title: "Limiter"
            detail: page.st && page.st.limiter_db > 0.05 ? "−" + page.st.limiter_db.toFixed(1) + " dB" : "idle\nheadroom " + (daemon.state ? page.fmtDb(daemon.state.preamp_db) : "")
          }
          Arrow { theme: page.theme }
          Column {
            width: 280
            spacing: 6
            anchors.verticalCenter: parent.verticalCenter
            LevelBar { width: parent.width; theme: page.theme; label: "Left"; tint: page.theme.cLeft; peakDb: page.meter("out", 0).peak; rmsDb: page.meter("out", 0).rms }
            LevelBar { width: parent.width; theme: page.theme; label: "Right"; tint: page.theme.cRight; peakDb: page.meter("out", 1).peak; rmsDb: page.meter("out", 1).rms }
            LevelBar { width: parent.width; theme: page.theme; label: "Sub"; tint: page.theme.cSub; peakDb: page.meter("out", 2).peak; rmsDb: page.meter("out", 2).rms }
          }
        }
        Text {
          visible: !!page.cfg && !page.cfg.enabled
          text: "Bypassed: plain stereo to the mains (level-matched), sub silent."
          color: page.theme.yellow
          font.family: page.theme.font
          font.pixelSize: 11
        }
      }
    }

    // ---------------------------------------------------------- right
    ColumnLayout {
      Layout.preferredWidth: 360
      Layout.maximumWidth: 360
      Layout.fillHeight: true
      spacing: 16

      Card {
        theme: page.theme
        title: "Controls"
        Layout.fillWidth: true
        enabled: daemon.connected && !!page.cfg

        SSlider {
          width: parent.width; theme: page.theme
          label: "Volume"
          valueText: Math.round(page.volume * 100) + "%"
          from: 0; to: 1; step: 0.01
          value: page.volume
          onMoved: function(v) { if (page.sinkNode && page.sinkNode.audio) page.sinkNode.audio.volume = v }
        }
        SSlider {
          width: parent.width; theme: page.theme; tint: page.theme.cSub
          label: "Sub level"
          valueText: page.cfg ? page.fmtDb(page.cfg.sub_gain_db) : "—"
          from: -12; to: 12; step: 0.5; defaultValue: 0
          value: page.cfg ? page.cfg.sub_gain_db : 0
          enabled: !!page.cfg && page.cfg.bass_management
          onMoved: function(v) { daemon.set("sub_gain_db", v) }
        }
        SSlider {
          width: parent.width; theme: page.theme
          label: "Crossover"
          valueText: page.cfg ? Math.round(page.cfg.crossover_hz) + " Hz" : "—"
          from: 40; to: 150; step: 5; defaultValue: 80
          value: page.cfg ? page.cfg.crossover_hz : 80
          enabled: !!page.cfg && page.cfg.bass_management
          onMoved: function(v) { daemon.set("crossover_hz", v) }
        }
        SSlider {
          width: parent.width; theme: page.theme
          label: "Bass"
          valueText: page.cfg ? page.fmtDb(page.cfg.bass_db) : "—"
          from: -8; to: 8; step: 0.5; defaultValue: 0
          value: page.cfg ? page.cfg.bass_db : 0
          onMoved: function(v) { daemon.set("bass_db", v) }
        }
        SSlider {
          width: parent.width; theme: page.theme
          label: "Treble"
          valueText: page.cfg ? page.fmtDb(page.cfg.treble_db) : "—"
          from: -8; to: 8; step: 0.5; defaultValue: 0
          value: page.cfg ? page.cfg.treble_db : 0
          onMoved: function(v) { daemon.set("treble_db", v) }
        }
        SToggle {
          width: parent.width; theme: page.theme
          label: "Room EQ"; detail: "measured correction filters"
          checked: !!page.cfg && page.cfg.room_eq
          onToggled: daemon.set("room_eq", !page.cfg.room_eq)
        }
        SToggle {
          width: parent.width; theme: page.theme
          label: "Bass management"; detail: "bass below the crossover (and LFE) to the sub"
          checked: !!page.cfg && page.cfg.bass_management
          onToggled: daemon.set("bass_management", !page.cfg.bass_management)
        }
        SToggle {
          width: parent.width; theme: page.theme
          label: "Steep crossover (LR8)"; detail: "48 dB/oct instead of 24"
          checked: !!page.cfg && page.cfg.crossover_slope === 48
          onToggled: daemon.set("crossover_slope", page.cfg.crossover_slope === 48 ? 24 : 48)
        }
        SToggle {
          width: parent.width; theme: page.theme; tint: page.theme.red
          label: "Mute"
          checked: !!page.cfg && page.cfg.mute
          onToggled: daemon.set("mute", !page.cfg.mute)
        }
      }

      Card {
        theme: page.theme
        title: "Engine"
        Layout.fillWidth: true
        Layout.fillHeight: true

        LevelBar { width: parent.width; theme: page.theme; label: "In L"; peakDb: page.meter("in", 0).peak; rmsDb: page.meter("in", 0).rms }
        LevelBar { width: parent.width; theme: page.theme; label: "In R"; peakDb: page.meter("in", 1).peak; rmsDb: page.meter("in", 1).rms }
        Grid {
          width: parent.width
          columns: 2
          rowSpacing: 6
          columnSpacing: 12
          Repeater {
            model: [
              ["Headroom", daemon.state ? page.fmtDb(daemon.state.preamp_db) : "—"],
              ["Limiter", page.st ? (page.st.limiter_db > 0.05 ? "−" + page.st.limiter_db.toFixed(1) + " dB" : "idle") : "—"],
              ["Limited / late blocks", page.st ? String(page.st.limited) + " / " + String(page.st.tail_misses || 0) : "—"],
              ["DSP load", page.st ? (page.st.load * 100).toFixed(2) + "%" + (page.st.idle_channels === 3 ? " (idle)" : "") : "—"],
              ["Latency", daemon.state ? daemon.state.latency_ms.toFixed(1) + " ms + graph" : "—"],
              ["Sink / output", page.st ? page.st.sink_state + " / " + page.st.output_state : "offline"]
            ]
            Column {
              required property var modelData
              width: (parent.width - 12) / 2
              Text { text: modelData[0]; color: page.theme.muted; font.family: page.theme.font; font.pixelSize: 10 }
              Text { text: modelData[1]; color: page.theme.fg; font.family: page.theme.font; font.pixelSize: 12 }
            }
          }
        }
      }
    }
  }

  component FlowBlock: Rectangle {
    property var theme
    property bool active: true
    property bool warn: false
    property string title: ""
    property string detail: ""
    height: 74
    radius: 7
    color: active ? theme.alpha(theme.accent, 0.14) : theme.alpha(theme.fg, 0.04)
    border.color: warn ? theme.red : active ? theme.alpha(theme.accent, 0.8) : theme.alpha(theme.fg, 0.15)
    border.width: 1
    anchors.verticalCenter: parent.verticalCenter
    Behavior on color { ColorAnimation { duration: 150 } }
    Column {
      anchors.fill: parent
      anchors.margins: 9
      spacing: 3
      Text {
        text: title
        color: active ? theme.fg : theme.muted
        font.family: theme.font
        font.pixelSize: 12
        font.bold: true
      }
      Text {
        text: detail
        width: parent.width
        elide: Text.ElideRight
        color: theme.muted
        font.family: theme.font
        font.pixelSize: 10
        lineHeight: 1.1
      }
    }
  }

  component Arrow: Text {
    property var theme
    width: 22
    horizontalAlignment: Text.AlignHCenter
    anchors.verticalCenter: parent.verticalCenter
    text: "›"
    color: theme.muted
    font.family: theme.font
    font.pixelSize: 20
  }
}
