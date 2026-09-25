import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Io

// Graphical calibration: drives `roomcorr calibrate --json` and shows
// every step, prompt, level and measurement as it happens.
Item {
  id: page
  property var theme
  property var daemon
  property var report

  property int positions: 5
  property bool running: proc.running || verifyProc.running
  property int step: 0
  property var prompt: null            // { kind, text, default }
  property var levels: ({})            // what -> { dbfs, spl, snr }
  property var noise: null
  property var subDetect: []
  property var subBalance: null
  property var subLive: null           // live sub-level reading while adjusting
  property string logText: ""
  property int curPos: 0
  property int posOf: 0
  property string posHint: ""
  property var posResults: ({})        // n -> { left, right, sub, clipped }
  property real measureSeconds: 0
  property real measureLeft: 0
  property var design: null
  property var verify: null
  property var result: null            // { ok, error }

  property string mode: "calibrate"   // or "sublevel"
  readonly property var stepNames: mode === "sublevel"
      ? ["Room noise", "Reference levels", "Sub level"]
      : ["Devices", "Before we start", "Room noise", "Find the subwoofer", "Levels", "Measure", "Design filters", "Verify"]
  // Mic offsets for the position hints (cm; -y is toward the speakers).
  readonly property var posOffsets: [[0, 0], [-30, 0], [30, 0], [0, -30], [0, 30], [-20, 0], [20, 0], [-20, -20], [20, 20]]


  function reset() {
    step = 0; prompt = null; levels = {}; noise = null; subDetect = []; subBalance = null
    curPos = 0; posOf = positions; posHint = ""; posResults = {}; design = null; verify = null; result = null
    logText = ""
    subLive = null
  }
  function startSubLevel() {
    reset()
    mode = "sublevel"
    proc.command = ["roomcorr", "sublevel", "--json"]
    proc.running = true
  }
  function start() {
    reset()
    mode = "calibrate"
    proc.command = ["roomcorr", "calibrate", "--json", "--positions", String(positions)]
    proc.running = true
  }
  function startVerify() {
    verify = null; result = null; prompt = null; step = 8
    verifyProc.running = true
  }
  function answer(text) {
    prompt = null
    var p = proc.running ? proc : verifyProc
    p.write(text + "\n")
  }
  function stop() {
    if (proc.running) proc.signal(15)
    if (verifyProc.running) verifyProc.signal(15)
  }

  function addLog(t) {
    var lines = (logText === "" ? [] : logText.split("\n")).concat([t])
    if (lines.length > 500) lines = lines.slice(lines.length - 500)
    logText = lines.join("\n")
  }

  function handle(line) {
    var e
    try { e = JSON.parse(line) } catch (x) { addLog(line); return }
    switch (e.ev) {
    case "log": addLog(e.text); break
    case "sub_live": subLive = e; break
    case "step": step = e.n; if (e.n === 6) posOf = page.positions; break
    case "prompt": prompt = e; break
    case "level": { var l = Object.assign({}, levels); l[e.what] = e; levels = l; break }
    case "noise": noise = e; break
    case "sub_detect": subDetect = subDetect.concat([e]); break
    case "sub_balance": subBalance = e; if (prompt && prompt.kind === "sub_live") prompt = null; subLive = null; break
    case "position": curPos = e.n; posOf = e.of; posHint = e.hint; break
    case "measuring": measureSeconds = e.seconds; measureLeft = e.seconds; break
    case "snr": { var r = Object.assign({}, posResults); r[e.position] = e; posResults = r; measureLeft = 0; break }
    case "design": design = e; step = 7; break
    case "verify": verify = e; break
    case "done": result = e; prompt = null; measureLeft = 0; if (e.ok && step < 7) step = 7; break
    }
  }

  Process {
    id: proc
    stdinEnabled: true
    stdout: SplitParser { onRead: function(l) { page.handle(l) } }
    stderr: SplitParser { onRead: function(l) { page.addLog("! " + l) } }
  }
  Process {
    id: verifyProc
    command: ["roomcorr", "verify", "--json"]
    stdinEnabled: true
    stdout: SplitParser { onRead: function(l) { page.handle(l) } }
    stderr: SplitParser { onRead: function(l) { page.addLog("! " + l) } }
  }
  Timer {
    interval: 200
    repeat: true
    running: page.measureLeft > 0
    onTriggered: page.measureLeft = Math.max(0, page.measureLeft - 0.2)
  }

  RowLayout {
    anchors.fill: parent
    spacing: 16

    // ---------------------------------------------------------- steps
    ColumnLayout {
      Layout.preferredWidth: 300
      Layout.maximumWidth: 300
      Layout.fillHeight: true
      spacing: 16

      Card {
        theme: page.theme
        title: "Calibration"
        Layout.fillWidth: true

        Text {
          width: parent.width
          wrapMode: Text.WordWrap
          text: "Mic on its stand at ear height, pointing at the ceiling. Set the Marantz to your usual listening level and leave it there afterwards."
          color: page.theme.muted
          font.family: page.theme.font
          font.pixelSize: 11
        }
        Text { text: "Mic positions"; color: page.theme.fg; font.family: page.theme.font; font.pixelSize: 12 }
        Row {
          spacing: 6
          Repeater {
            model: [1, 3, 5, 7, 9]
            SButton {
              required property var modelData
              theme: page.theme
              text: String(modelData)
              selected: page.positions === modelData
              enabled: !page.running
              onClicked: page.positions = modelData
            }
          }
        }
        Text {
          width: parent.width
          wrapMode: Text.WordWrap
          text: "5+ positions average out seat-specific nulls, so the correction holds for your head moving, not just one point."
          color: page.theme.muted
          font.family: page.theme.font
          font.pixelSize: 10
        }
        SButton {
          width: parent.width
          theme: page.theme
          primary: !page.running
          text: page.running ? "Stop" : "Start calibration"
          icon: page.running ? "\u{f04d}" : "\u{f130}"
          tint: page.running ? page.theme.red : page.theme.accent
          onClicked: page.running ? page.stop() : page.start()
        }
        SButton {
          width: parent.width
          theme: page.theme
          text: "Adjust sub level only"
          icon: "\u{f04c3}"
          enabled: !page.running
          onClicked: page.startSubLevel()
        }
        Text {
          width: parent.width
          wrapMode: Text.WordWrap
          text: "Live meter for the sub's volume knob, like an AV receiver's level setup. Recalibrate afterwards if you moved the knob."
          color: page.theme.muted
          font.family: page.theme.font
          font.pixelSize: 10
        }
      }

      Card {
        theme: page.theme
        title: "Steps"
        Layout.fillWidth: true
        Layout.fillHeight: true
        Repeater {
          model: page.stepNames
          Row {
            id: stepRow
            required property var modelData
            required property int index
            readonly property int n: index + 1
            readonly property bool done: page.step > n || !!(page.result && page.result.ok && page.step >= n)
            readonly property bool current: page.step === n && page.running
            spacing: 10
            Rectangle {
              width: 22; height: 22; radius: 11
              color: parent.done ? page.theme.green : parent.current ? page.theme.accent : "transparent"
              border.color: parent.done || parent.current ? "transparent" : page.theme.alpha(page.theme.fg, 0.25)
              Text {
                anchors.centerIn: parent
                text: parent.parent.done ? "✓" : String(parent.parent.n)
                color: parent.parent.done || parent.parent.current ? page.theme.bg : page.theme.muted
                font.family: page.theme.font
                font.pixelSize: 11
                font.bold: true
              }
              SequentialAnimation on opacity {
                running: stepRow.current
                loops: Animation.Infinite
                alwaysRunToEnd: true
                NumberAnimation { to: 0.45; duration: 600 }
                NumberAnimation { to: 1; duration: 600 }
              }
            }
            Text {
              anchors.verticalCenter: parent.verticalCenter
              text: modelData
              color: parent.current ? page.theme.fg : parent.done ? page.theme.fgLight : page.theme.muted
              font.family: page.theme.font
              font.pixelSize: 12
              font.bold: parent.current
            }
          }
        }
      }
    }

    // ---------------------------------------------------------- centre
    ColumnLayout {
      Layout.fillWidth: true
      Layout.fillHeight: true
      spacing: 16

      // Prompt / activity banner
      Rectangle {
        Layout.fillWidth: true
        implicitHeight: banner.implicitHeight + 28
        radius: 8
        color: page.prompt ? page.theme.alpha(page.theme.accent, 0.16) : page.theme.bgDark
        border.color: page.prompt ? page.theme.accent : page.theme.alpha(page.theme.fg, 0.08)

        Column {
          id: banner
          x: 16; y: 14
          width: parent.width - 32
          spacing: 10
          Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: page.prompt ? page.prompt.text
                : page.measureLeft > 0 ? "Measuring position " + page.curPos + "… stay quiet (" + Math.ceil(page.measureLeft) + " s)"
                : page.result ? (page.result.ok ? (page.mode === "sublevel" ? "Sub level set. Recalibrate (or Rebuild filters) so the correction matches the new knob position." : "Done. The new correction is active.")
                                                : "Stopped: " + (page.result.error || "see log"))
                : page.running ? (page.step > 0 ? page.stepNames[page.step - 1] + "…" : "Starting…")
                : "Ready. Choose the number of mic positions and press Start."
            color: page.theme.fg
            font.family: page.theme.font
            font.pixelSize: 15
            font.bold: !!page.prompt
          }
          Text {
            visible: !!page.prompt && page.curPos > 0 && page.step === 6
            text: "Position " + page.curPos + " of " + page.posOf + ": " + page.posHint
            color: page.theme.accent
            font.family: page.theme.font
            font.pixelSize: 12
          }
          Rectangle {
            visible: page.measureLeft > 0
            width: parent.width
            height: 6
            radius: 3
            color: page.theme.alpha(page.theme.fg, 0.1)
            Rectangle {
              width: parent.width * (1 - page.measureLeft / Math.max(1, page.measureSeconds))
              height: parent.height
              radius: 3
              color: page.theme.accent
            }
          }
          // Live subwoofer level: needle against the target zone.
          Column {
            visible: !!page.prompt && page.prompt.kind === "sub_live"
            width: parent.width
            spacing: 8
            readonly property var r: page.subLive
            readonly property real off: r ? r.offset_db : 0
            readonly property real win: r ? r.window_db : 2
            Text {
              text: !parent.r ? "Listening…"
                  : parent.r.direction === "ok" ? "✓  In range. Leave the knob here."
                  : (parent.r.direction === "down" ? "▼  Turn the sub's volume knob DOWN" : "▲  Turn the sub's volume knob UP")
                    + "   ·   " + Math.abs(parent.off).toFixed(1) + " dB to go"
              color: !parent.r ? page.theme.muted : parent.r.direction === "ok" ? page.theme.green : page.theme.yellow
              font.family: page.theme.font
              font.pixelSize: 22
              font.bold: true
            }
            Item {
              width: parent.width
              height: 46
              readonly property real range: 12   // dB each side
              function xAt(v) { return (Math.max(-range, Math.min(range, v)) + range) / (2 * range) * width }
              Rectangle { y: 14; width: parent.width; height: 14; radius: 7; color: page.theme.alpha(page.theme.fg, 0.08) }
              Rectangle {
                y: 14; height: 14; radius: 7
                x: parent.xAt(-parent.parent.win); width: parent.xAt(parent.parent.win) - x
                color: page.theme.alpha(page.theme.green, 0.45)
              }
              Rectangle {
                width: 4; height: 34; y: 4; radius: 2
                x: parent.xAt(parent.parent.off) - 2
                color: parent.parent.r && parent.parent.r.direction === "ok" ? page.theme.green : page.theme.yellow
                Behavior on x { NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }
              }
              Text { y: 32; x: 0; text: "sub too quiet"; color: page.theme.muted; font.family: page.theme.font; font.pixelSize: 10 }
              Text { y: 32; x: parent.width / 2 - width / 2; text: "target"; color: page.theme.green; font.family: page.theme.font; font.pixelSize: 10 }
              Text { y: 32; x: parent.width - width; text: "sub too loud"; color: page.theme.muted; font.family: page.theme.font; font.pixelSize: 10 }
            }
            Text {
              text: parent.r ? "Sub vs mains " + (parent.r.diff_db > 0 ? "+" : "") + parent.r.diff_db.toFixed(1) + " dB  ·  target +"
                               + parent.r.target_db.toFixed(1) + " ± " + parent.r.window_db.toFixed(0) + " dB. Small turns: the reading updates 3× a second." : ""
              color: page.theme.muted
              font.family: page.theme.font
              font.pixelSize: 11
            }
          }
          Row {
            visible: !!page.prompt
            spacing: 8
            SButton {
              visible: !!page.prompt && page.prompt.kind === "sub_live"
              theme: page.theme; primary: true; text: "Done"; icon: "\u{f00c}"
              onClicked: page.answer("")
            }
            SButton {
              visible: !!page.prompt && page.prompt.kind === "continue"
              theme: page.theme; primary: true; text: "Continue"; icon: "\u{f04b}"
              onClicked: page.answer("")
            }
            SButton {
              visible: !!page.prompt && page.prompt.kind === "yesno"
              theme: page.theme; primary: true; text: "Yes"
              onClicked: page.answer("y")
            }
            SButton {
              visible: !!page.prompt && page.prompt.kind === "yesno"
              theme: page.theme; text: "No"
              onClicked: page.answer("n")
            }
            SButton {
              visible: !!page.prompt && page.prompt.kind === "text"
              theme: page.theme; primary: true; text: "Re-check"
              onClicked: page.answer("")
            }
            SButton {
              visible: !!page.prompt && page.prompt.kind === "text"
              theme: page.theme; text: "Skip"
              onClicked: page.answer("s")
            }
          }
        }
      }

      RowLayout {
        Layout.fillWidth: true
        spacing: 16

        Card {
          theme: page.theme
          title: "Levels at the mic"
          Layout.fillWidth: true
          Layout.preferredHeight: 190
          SplGauge { width: parent.width; theme: page.theme; label: "Room noise"; spl: page.noise ? page.noise.mid_spl : 0; tint: page.theme.muted }
          SplGauge { width: parent.width; theme: page.theme; label: "Mains"; spl: page.levels.mains ? page.levels.mains.spl : 0; tint: page.theme.cLeft }
          SplGauge { width: parent.width; theme: page.theme; label: "Sub"; spl: page.levels.sub ? page.levels.sub.spl : 0; tint: page.theme.cSub }
          Text {
            text: page.subBalance ? "Sub vs mains sensitivity " + (page.subBalance.diff_db > 0 ? "+" : "") + page.subBalance.diff_db.toFixed(1) + " dB"
                                    + (page.subBalance.ok ? "  ✓" : "  (adjust the sub's knob)") : "Target: 75 dB SPL"
            color: page.subBalance && !page.subBalance.ok ? page.theme.yellow : page.theme.muted
            font.family: page.theme.font
            font.pixelSize: 11
          }
        }

        Card {
          theme: page.theme
          title: "Subwoofer output"
          Layout.preferredWidth: 260
          Layout.preferredHeight: 190
          Repeater {
            model: page.subDetect
            Row {
              required property var modelData
              spacing: 8
              Text {
                text: modelData.heard ? "●" : "○"
                color: modelData.heard ? page.theme.green : page.theme.muted
                font.pixelSize: 13
              }
              Text {
                text: (modelData.output === "LFE" ? "C/Sub ring (LFE)" : "C/Sub tip (FC)") + "  " + (modelData.snr > 0 ? "+" : "") + modelData.snr.toFixed(0) + " dB"
                color: page.theme.fg
                font.family: page.theme.font
                font.pixelSize: 11
              }
            }
          }
          Text {
            visible: page.subDetect.length === 0
            text: "Detected automatically"
            color: page.theme.muted
            font.family: page.theme.font
            font.pixelSize: 11
          }
        }
      }

      RowLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        spacing: 16

        Card {
          theme: page.theme
          title: "Mic positions"
          Layout.preferredWidth: 380
          Layout.fillHeight: true

          Canvas {
            id: seatMap
            width: parent.width
            height: 250
            property var results: page.posResults
            property int cur: page.curPos
            property int total: page.running || page.posOf ? Math.max(page.posOf, page.positions) : page.positions
            onResultsChanged: requestPaint()
            onCurChanged: requestPaint()
            onTotalChanged: requestPaint()
            onPaint: {
              var c = getContext("2d")
              c.reset()
              var th = page.theme
              var w = width, h = height
              function col(x, a) { return Qt.rgba(x.r, x.g, x.b, a) }
              // speakers
              c.fillStyle = col(th.fg, 0.15)
              c.fillRect(w * 0.18, 12, 34, 44); c.fillRect(w * 0.82 - 34, 12, 34, 44)
              // sub sits below the right speaker
              c.fillStyle = col(th.cSub, 0.25)
              c.fillRect(w * 0.82 - 42, 66, 50, 40)
              c.fillStyle = col(th.fg, 0.6)
              c.font = "11px \"" + th.font + "\""
              c.fillText("L", w * 0.18 + 13, 38); c.fillText("R", w * 0.82 - 21, 38); c.fillText("Sub", w * 0.82 - 29, 90)
              // seat
              var cx = w / 2, cy = h * 0.66, sc = 1.9
              c.fillStyle = col(th.fg, 0.07)
              c.fillRect(cx - 60, cy - 30, 120, 90)
              c.fillStyle = col(th.fg, 0.35)
              c.fillText("seat", cx + 64, cy + 56)
              for (var i = 0; i < total && i < page.posOffsets.length; i++) {
                var x = cx + page.posOffsets[i][0] * sc, y = cy + page.posOffsets[i][1] * sc
                var r = results[i + 1]
                c.beginPath(); c.arc(x, y, 9, 0, Math.PI * 2)
                if (r) { c.fillStyle = r.clipped ? th.red : th.green; c.fill() }
                else if (cur === i + 1) { c.fillStyle = th.accent; c.fill() }
                else { c.strokeStyle = col(th.fg, 0.4); c.lineWidth = 1.5; c.stroke() }
                c.fillStyle = r || cur === i + 1 ? th.bg : col(th.fg, 0.6)
                var s = String(i + 1)
                c.fillText(s, x - c.measureText(s).width / 2, y + 4)
              }
            }
          }
          Repeater {
            model: Object.keys(page.posResults)
            Text {
              required property var modelData
              readonly property var r: page.posResults[modelData]
              text: "Pos " + modelData + "   SNR  L " + r.left.toFixed(0) + "  R " + r.right.toFixed(0) + "  Sub " + r.sub.toFixed(0) + " dB" + (r.clipped ? "   clipped!" : "")
              color: r.clipped || r.sub < 30 ? page.theme.yellow : page.theme.fg
              font.family: page.theme.font
              font.pixelSize: 11
            }
          }
        }

        Card {
          theme: page.theme
          title: "Log"
          Layout.fillWidth: true
          Layout.fillHeight: true
          Row {
            spacing: 6
            SButton {
              theme: page.theme
              text: copied.running ? "Copied" : "Copy log"
              icon: "\u{f0c5}"
              enabled: page.logText !== ""
              onClicked: { Quickshell.clipboardText = page.logText; copied.restart() }
              Timer { id: copied; interval: 1500 }
            }
            SButton {
              theme: page.theme
              text: "Clear"
              enabled: page.logText !== "" && !page.running
              onClicked: page.logText = ""
            }
          }
          Flickable {
            id: logFlick
            width: parent.width
            height: Math.max(120, page.height - 560)
            clip: true
            contentHeight: logEdit.contentHeight
            boundsBehavior: Flickable.StopAtBounds
            function toEnd() { contentY = Math.max(0, contentHeight - height) }
            TextEdit {
              id: logEdit
              width: logFlick.width
              readOnly: true
              selectByMouse: true
              persistentSelection: true
              wrapMode: TextEdit.Wrap
              text: page.logText
              color: page.theme.fgLight
              selectionColor: page.theme.alpha(page.theme.accent, 0.4)
              selectedTextColor: page.theme.fg
              font.family: page.theme.font
              font.pixelSize: 11
              onTextChanged: Qt.callLater(logFlick.toEnd)
            }
          }
        }
      }
    }

    // ---------------------------------------------------------- results
    ColumnLayout {
      Layout.preferredWidth: 340
      Layout.maximumWidth: 340
      Layout.fillHeight: true
      spacing: 16

      Card {
        theme: page.theme
        title: "Result"
        Layout.fillWidth: true
        Text {
          visible: !page.design
          width: parent.width
          wrapMode: Text.WordWrap
          text: "Filter design results appear here."
          color: page.theme.muted
          font.family: page.theme.font
          font.pixelSize: 11
        }
        Repeater {
          model: page.design ? page.design.notes : []
          Text {
            required property var modelData
            width: parent.width
            wrapMode: Text.WordWrap
            text: String(modelData).replace(/\s{2,}/g, "  ")
            color: page.theme.fg
            font.family: page.theme.font
            font.pixelSize: 11
          }
        }
        Repeater {
          model: page.design ? page.design.warnings : []
          Text {
            required property var modelData
            width: parent.width
            wrapMode: Text.WordWrap
            text: "⚠ " + modelData
            color: page.theme.yellow
            font.family: page.theme.font
            font.pixelSize: 11
          }
        }
      }

      Card {
        theme: page.theme
        title: "Verify"
        subtitle: "Plays sweeps through the corrected system and measures the result"
        Layout.fillWidth: true
        Layout.fillHeight: true
        SButton {
          width: parent.width
          theme: page.theme
          text: verifyProc.running ? "Verifying…" : "Verify with the mic"
          icon: "\u{f00c}"
          enabled: !page.running && daemon.connected && !!page.report
          onClicked: page.startVerify()
        }
        Repeater {
          model: page.verify ? [
            ["Bass 20–200 Hz, all", page.verify.both_bass],
            ["Mid/treble, all", page.verify.both_mid],
            ["Bass, left", page.verify.left_bass],
            ["Bass, right", page.verify.right_bass]
          ] : []
          Row {
            required property var modelData
            width: parent.width
            Text { width: parent.width * 0.6; text: modelData[0]; color: page.theme.muted; font.family: page.theme.font; font.pixelSize: 11 }
            Text {
              width: parent.width * 0.4
              horizontalAlignment: Text.AlignRight
              text: Number(modelData[1]).toFixed(1) + " dB RMS"
              color: page.theme.fg
              font.family: page.theme.font
              font.pixelSize: 11
            }
          }
        }
        Text {
          visible: !!page.verify
          width: parent.width
          wrapMode: Text.WordWrap
          text: "Deviation from target at the mic. The Response tab shows the measured curves."
          color: page.theme.muted
          font.family: page.theme.font
          font.pixelSize: 10
        }
      }
    }
  }

  component SplGauge: Item {
    property var theme
    property string label: ""
    property real spl: 0
    property color tint: theme.fg
    implicitHeight: 22
    Text { id: gl; width: 84; text: label; color: theme.fg; opacity: 0.7; font.family: theme.font; font.pixelSize: 11; anchors.verticalCenter: parent.verticalCenter }
    Item {
      anchors.left: gl.right
      anchors.right: gv.left
      anchors.rightMargin: 8
      anchors.verticalCenter: parent.verticalCenter
      height: 10
      Rectangle { anchors.fill: parent; radius: 4; color: theme.alpha(theme.fg, 0.08) }
      Rectangle {
        height: parent.height; radius: 4; color: tint
        width: parent.width * Math.max(0, Math.min(1, (spl - 30) / 60))
        Behavior on width { NumberAnimation { duration: 300 } }
      }
      Rectangle {  // 75 dB target
        x: parent.width * (75 - 30) / 60
        width: 2; height: parent.height + 6; y: -3
        color: theme.alpha(theme.fg, 0.5)
      }
    }
    Text {
      id: gv
      width: 70
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      horizontalAlignment: Text.AlignRight
      text: spl > 0 ? spl.toFixed(1) + " dB" : "—"
      color: theme.fg
      font.family: theme.font
      font.pixelSize: 11
    }
  }
}
