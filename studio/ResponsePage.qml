import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Io

// Frequency response: before / predicted / measured against the target,
// the correction filters, and the target-curve editor.
Item {
  id: page
  property var theme
  property var daemon
  property var report
  property string view: "system"
  property bool rebuilding: false
  property string rebuildLog: ""

  readonly property var cfg: daemon ? daemon.cfg : null
  readonly property var tgt: cfg ? cfg.target : null
  readonly property var sum: report ? report.summary : null

  // Draft target being edited (applied on "Rebuild filters").
  property real dBass: tgt ? tgt.bass_boost_db : 6.5
  property real dCorner: tgt ? tgt.bass_corner_hz : 105
  property real dTreble: tgt ? tgt.treble_db : -2.5
  property real dTransition: tgt ? tgt.transition_hz : 250
  property real dMaxBoost: tgt ? tgt.max_boost_db : 4
  readonly property bool dirty: !!tgt && (dBass !== tgt.bass_boost_db || dCorner !== tgt.bass_corner_hz
                                          || dTreble !== tgt.treble_db || dTransition !== tgt.transition_hz
                                          || dMaxBoost !== tgt.max_boost_db)
  onTgtChanged: if (tgt && !rebuilding) {
    dBass = tgt.bass_boost_db; dCorner = tgt.bass_corner_hz; dTreble = tgt.treble_db
    dTransition = tgt.transition_hz; dMaxBoost = tgt.max_boost_db
  }

  // Same shelves as the C++ target (RBJ, S = 1, evaluated at 192 kHz).
  function shelfDb(f, f0, gainDb, high) {
    var fs = 192000, A = Math.pow(10, gainDb / 40), w = 2 * Math.PI * f0 / fs
    var c = Math.cos(w), s = Math.sin(w), al = s / 2 * Math.SQRT2, sq = 2 * Math.sqrt(A) * al
    var b0, b1, b2, a0, a1, a2
    if (!high) {
      b0 = A * ((A + 1) - (A - 1) * c + sq); b1 = 2 * A * ((A - 1) - (A + 1) * c); b2 = A * ((A + 1) - (A - 1) * c - sq)
      a0 = (A + 1) + (A - 1) * c + sq; a1 = -2 * ((A - 1) + (A + 1) * c); a2 = (A + 1) + (A - 1) * c - sq
    } else {
      b0 = A * ((A + 1) + (A - 1) * c + sq); b1 = -2 * A * ((A - 1) + (A + 1) * c); b2 = A * ((A + 1) + (A - 1) * c - sq)
      a0 = (A + 1) - (A - 1) * c + sq; a1 = 2 * ((A - 1) - (A + 1) * c); a2 = (A + 1) - (A - 1) * c - sq
    }
    var wf = 2 * Math.PI * f / fs
    var c1 = Math.cos(wf), s1 = Math.sin(wf), c2 = Math.cos(2 * wf), s2 = Math.sin(2 * wf)
    var nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2)
    var dr = a0 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2)
    return 10 * Math.log((nr * nr + ni * ni) / (dr * dr + di * di)) / Math.LN10
  }
  function targetDb(f, bass, corner, treble) {
    return shelfDb(f, corner, bass, false) + shelfDb(f, 2500, treble, true)
  }

  // Draft target on the report's scale: the report's target plus the change.
  readonly property var draftTarget: {
    if (!report || !report.freqs || !tgt) return []
    var out = []
    for (var i = 0; i < report.freqs.length; i++) {
      var f = report.freqs[i]
      var delta = targetDb(f, dBass, dCorner, dTreble) - targetDb(f, tgt.bass_boost_db, tgt.bass_corner_hz, tgt.treble_db)
      out.push(report.target[i] + delta)
    }
    return out
  }

  readonly property var graph: {
    var g = { xMin: 20, xMax: 20000, series: [], legend: [] }
    if (!report || !report.freqs) return g
    var v = report.verified || null
    var th = page.theme
    var tSeries = { xs: report.freqs, ys: report.target, color: th.cTarget, width: 1.5, dash: [6, 4], alpha: 0.8 }
    if (view === "system") {
      g.xMin = 15; g.xMax = 500
      g.series.push(tSeries)
      if (report.system) {
        g.series.push({ xs: report.system.freqs, ys: report.system.before, color: th.cBefore, width: 1.5 })
        g.series.push({ xs: report.system.freqs, ys: report.system.after, color: th.cPredicted, width: 2.5 })
      }
      if (v && v.both) g.series.push({ xs: v.freqs, ys: v.both, color: th.cMeasured, width: 2 })
    } else if (view === "filters") {
      g.xMin = 15; g.xMax = 20000
      var names = ["left", "right", "sub"], cols = [th.cLeft, th.cRight, th.cSub]
      for (var c = 0; c < 3; c++) {
        var ch = report.channels ? report.channels[names[c]] : null
        if (ch) g.series.push({ xs: report.freqs, ys: ch.correction, color: cols[c], width: 2 })
      }
      return g
    } else {
      var chn = report.channels ? report.channels[view] : null
      if (view === "sub") { g.xMin = 15; g.xMax = 500 }
      g.series.push(tSeries)
      if (chn) {
        g.series.push({ xs: report.freqs, ys: chn.measured, color: th.cBefore, width: 1.5 })
        g.series.push({ xs: report.freqs, ys: chn.corrected, color: th.cPredicted, width: 2.5 })
      }
      if (v && v[view]) g.series.push({ xs: v.freqs, ys: v[view], color: th.cMeasured, width: 2 })
    }
    if (dirty && view !== "filters") g.series.push({ xs: report.freqs, ys: draftTarget, color: th.orange, width: 2, dash: [3, 3] })
    return g
  }

  // Keep the target in view.
  readonly property var yRange: {
    if (view === "filters") return { lo: -18, hi: 8 }
    if (!report || !report.target) return { lo: 40, hi: 100 }
    var lo = 1e9, hi = -1e9
    for (var i = 0; i < report.freqs.length; i++) {
      var f = report.freqs[i]
      if (f < graph.xMin || f > graph.xMax) continue
      lo = Math.min(lo, report.target[i]); hi = Math.max(hi, report.target[i])
    }
    return { lo: Math.floor((lo - 25) / 5) * 5, hi: Math.ceil((hi + 15) / 5) * 5 }
  }

  Process {
    id: designProc
    command: ["roomcorr", "design"]
    stdout: StdioCollector { onStreamFinished: page.rebuildLog = text }
    onExited: function(code) { page.rebuilding = false; if (code !== 0) page.rebuildLog = "rebuild failed (" + code + ")\n" + page.rebuildLog }
  }
  Timer {
    id: afterSave
    interval: 400
    onTriggered: designProc.running = true
  }
  function rebuild() {
    rebuilding = true
    rebuildLog = ""
    daemon.setMany({
      "target.bass_boost_db": dBass, "target.bass_corner_hz": dCorner, "target.treble_db": dTreble,
      "target.transition_hz": dTransition, "target.max_boost_db": dMaxBoost
    })
    daemon.save()
    afterSave.restart()
  }
  function preset(bass, corner, treble) { dBass = bass; dCorner = corner; dTreble = treble }

  RowLayout {
    anchors.fill: parent
    spacing: 16

    ColumnLayout {
      Layout.fillWidth: true
      Layout.fillHeight: true
      spacing: 12

      Row {
        spacing: 6
        Repeater {
          model: [
            { id: "system", label: "System bass (L+R+sub)" },
            { id: "left", label: "Left" },
            { id: "right", label: "Right" },
            { id: "sub", label: "Sub" },
            { id: "filters", label: "Correction filters" }
          ]
          SButton {
            required property var modelData
            theme: page.theme
            text: modelData.label
            selected: page.view === modelData.id
            onClicked: page.view = modelData.id
          }
        }
      }

      Card {
        theme: page.theme
        Layout.fillWidth: true
        Layout.fillHeight: true
        title: page.view === "filters" ? "Correction filters (what the FIRs do)" : "Frequency response at the listening position"
        subtitle: !page.report ? "No calibration yet: run one on the Calibrate tab."
                  : page.view === "filters" ? "Full correction below " + Math.round(page.dTransition) + " Hz, gentle broad correction above " + Math.round(2 * page.dTransition) + " Hz"
                  : "Levels relative: the reference level is drawn at 75 dB"

        Graph {
          width: parent.width
          height: Math.max(260, page.height - 150)
          theme: page.theme
          xMin: page.graph.xMin
          xMax: page.graph.xMax
          yMin: page.yRange.lo
          yMax: page.yRange.hi
          yStep: page.view === "filters" ? 3 : 5
          yUnit: "dB"
          series: page.graph.series
          bands: page.view === "filters" || page.view === "left" || page.view === "right"
                 ? [{ from: 2 * page.dTransition, to: 30000, color: page.theme.alpha(page.theme.fg, 0.035) }] : []
          hlines: page.view === "filters" ? [{ y: 0, color: page.theme.alpha(page.theme.fg, 0.3) }] : []
          markers: page.cfg && page.view !== "left" && page.view !== "right"
                   ? [{ x: page.cfg.crossover_hz, color: page.theme.alpha(page.theme.accent, 0.8), label: "xover" }] : []
        }
        Legend {
          theme: page.theme
          items: page.view === "filters"
                 ? [{ label: "Left", color: page.theme.cLeft }, { label: "Right", color: page.theme.cRight }, { label: "Sub", color: page.theme.cSub }]
                 : [{ label: "Target", color: page.theme.cTarget, dashed: true },
                    { label: "Before", color: page.theme.cBefore },
                    { label: "Predicted", color: page.theme.cPredicted },
                    { label: "Measured (verify)", color: page.theme.cMeasured }]
                   .concat(page.dirty ? [{ label: "New target (not applied)", color: page.theme.orange, dashed: true }] : [])
        }
      }
    }

    ColumnLayout {
      Layout.preferredWidth: 360
      Layout.maximumWidth: 360
      Layout.fillHeight: true
      spacing: 16

      Card {
        theme: page.theme
        title: "Target curve"
        subtitle: "Harman in-room is what listeners prefer on average"
        Layout.fillWidth: true
        enabled: daemon.connected && !!page.tgt

        Flow {
          width: parent.width
          spacing: 6
          SButton { theme: page.theme; text: "Harman"; selected: page.dBass === 6.5 && page.dTreble === -2.5; onClicked: page.preset(6.5, 105, -2.5) }
          SButton { theme: page.theme; text: "Harman +bass"; selected: page.dBass === 9 && page.dTreble === -3; onClicked: page.preset(9, 105, -3) }
          SButton { theme: page.theme; text: "Neutral"; selected: page.dBass === 3 && page.dTreble === -1.5; onClicked: page.preset(3, 105, -1.5) }
          SButton { theme: page.theme; text: "Flat"; selected: page.dBass === 0 && page.dTreble === 0; onClicked: page.preset(0, 105, 0) }
        }
        SSlider {
          width: parent.width; theme: page.theme
          label: "Bass shelf"; valueText: "+" + page.dBass.toFixed(1) + " dB"
          from: 0; to: 12; step: 0.5; value: page.dBass; defaultValue: 6.5
          onMoved: function(v) { page.dBass = v }
        }
        SSlider {
          width: parent.width; theme: page.theme
          label: "Shelf frequency"; valueText: Math.round(page.dCorner) + " Hz"
          from: 60; to: 200; step: 5; value: page.dCorner; defaultValue: 105
          onMoved: function(v) { page.dCorner = v }
        }
        SSlider {
          width: parent.width; theme: page.theme
          label: "Treble shelf (>2.5 kHz)"; valueText: (page.dTreble > 0 ? "+" : "") + page.dTreble.toFixed(1) + " dB"
          from: -6; to: 3; step: 0.5; value: page.dTreble; defaultValue: -2.5
          onMoved: function(v) { page.dTreble = v }
        }
        SSlider {
          width: parent.width; theme: page.theme
          label: "Full correction up to"; valueText: Math.round(page.dTransition) + " Hz"
          from: 100; to: 1000; step: 10; value: page.dTransition; defaultValue: 250
          onMoved: function(v) { page.dTransition = v }
        }
        SSlider {
          width: parent.width; theme: page.theme
          label: "Max boost into dips"; valueText: page.dMaxBoost.toFixed(1) + " dB"
          from: 0; to: 8; step: 0.5; value: page.dMaxBoost; defaultValue: 4
          onMoved: function(v) { page.dMaxBoost = v }
        }
        SButton {
          width: parent.width
          theme: page.theme
          primary: page.dirty
          text: page.rebuilding ? "Rebuilding filters…" : page.dirty ? "Rebuild filters with new target" : "Rebuild filters"
          icon: "\u{f021}"
          enabled: !page.rebuilding && !!page.report
          onClicked: page.rebuild()
        }
        Text {
          visible: page.rebuildLog !== ""
          width: parent.width
          wrapMode: Text.WordWrap
          text: page.rebuildLog.trim()
          color: page.theme.muted
          font.family: page.theme.font
          font.pixelSize: 10
        }
      }

      Card {
        theme: page.theme
        title: "Calibration"
        Layout.fillWidth: true
        Layout.fillHeight: true

        Repeater {
          model: page.sum ? [
            ["Crossover", Math.round(page.sum.crossover_hz || (page.cfg ? page.cfg.crossover_hz : 0)) + " Hz"],
            ["Predicted bass accuracy", (page.sum.bass_deviation_db || 0).toFixed(1) + " dB RMS"],
            ["Sub ↔ mains summation", Math.round(page.sum.crossover_sum_before * 100) + "% → " + Math.round(page.sum.crossover_sum_after * 100) + "%"],
            ["Sub range", Math.round(page.sum.sub_range_hz[0]) + "–" + Math.round(page.sum.sub_range_hz[1]) + " Hz"],
            ["Mains −6 dB", Math.round(page.sum.mains_low_hz) + " Hz"],
            ["Positions", String(page.sum.positions)]
          ] : []
          Row {
            required property var modelData
            width: parent.width
            Text { width: parent.width * 0.55; text: modelData[0]; color: page.theme.muted; font.family: page.theme.font; font.pixelSize: 11 }
            Text { width: parent.width * 0.45; horizontalAlignment: Text.AlignRight; text: modelData[1]; color: page.theme.fg; font.family: page.theme.font; font.pixelSize: 11 }
          }
        }
      }
    }
  }
}
