import QtQuick
import QtQuick.Layouts

// Time alignment: arrival of each speaker at the seat, and how sub and
// mains line up through the crossover.
Item {
  id: page
  property var theme
  property var daemon
  property var report
  property bool afterDelays: true
  property bool xoverBand: true

  readonly property var timing: report ? report.timing : null
  readonly property var etc: timing ? (xoverBand && timing.crossover_band ? timing.crossover_band : timing) : null
  readonly property var align: report ? report.alignment : null
  readonly property var sum: report ? report.summary : null

  function xs(list) {
    var out = []
    if (!timing || !list) return out
    for (var i = 0; i < list.length; i++) out.push(timing.start_ms + i * timing.step_ms)
    return out
  }
  // Delays as the engine applies them (relative to the smallest).
  function applied(name) {
    if (!timing || !timing.delay_ms) return 0
    var d = timing.delay_ms
    var mn = Math.min(d.left, d.right, d.sub)
    return d[name] - mn
  }
  function shift(name) { return afterDelays ? applied(name) : 0 }
  function cm(ms) { return Math.round(ms * 34.3) }

  RowLayout {
    anchors.fill: parent
    spacing: 16

    ColumnLayout {
      Layout.fillWidth: true
      Layout.fillHeight: true
      spacing: 16

      Card {
        theme: page.theme
        Layout.fillWidth: true
        Layout.fillHeight: true
        title: "Arrival at the listening position"
        subtitle: (page.afterDelays ? "As you hear it, with the engine's delays applied" : "As measured, no delays")
                  + (page.xoverBand ? " · all three limited to the crossover band, so sub and mains are comparable" : " · mains 100 Hz–16 kHz, sub 20–200 Hz")

        Row {
          spacing: 6
          SButton { theme: page.theme; text: "As measured"; selected: !page.afterDelays; onClicked: page.afterDelays = false }
          SButton { theme: page.theme; text: "After alignment"; selected: page.afterDelays; onClicked: page.afterDelays = true }
          Item { width: 18; height: 1 }
          SButton { theme: page.theme; text: "Crossover band"; selected: page.xoverBand; onClicked: page.xoverBand = true }
          SButton { theme: page.theme; text: "Full band"; selected: !page.xoverBand; onClicked: page.xoverBand = false }
        }
        Graph {
          width: parent.width
          height: Math.max(200, (page.height - 120) * 0.45)
          theme: page.theme
          logX: false
          xMin: -10
          xMax: 60
          xUnit: "ms"
          xTicks: [-10, 0, 10, 20, 30, 40, 50, 60]
          yMin: -40
          yMax: 0
          yStep: 10
          yUnit: "dB"
          series: page.etc ? [
            { xs: page.xs(page.etc.sub), ys: page.etc.sub, color: page.theme.cSub, width: 2, fill: true, fillAlpha: 0.12, offset: page.shift("sub") },
            { xs: page.xs(page.etc.left), ys: page.etc.left, color: page.theme.cLeft, width: 1.5, offset: page.shift("left") },
            { xs: page.xs(page.etc.right), ys: page.etc.right, color: page.theme.cRight, width: 1.5, offset: page.shift("right") }
          ] : []
        }
        Legend {
          theme: page.theme
          items: [{ label: "Left", color: page.theme.cLeft }, { label: "Right", color: page.theme.cRight }, { label: "Sub", color: page.theme.cSub }]
        }
      }

      RowLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        spacing: 16

        Card {
          theme: page.theme
          Layout.fillWidth: true
          Layout.fillHeight: true
          title: "Crossover: level"
          subtitle: "Mains and sub after correction; their sum before and after alignment"
          Graph {
            width: parent.width
            height: Math.max(180, (page.height - 120) * 0.40)
            theme: page.theme
            xMin: page.align ? page.align.freqs[0] : 20
            xMax: page.align ? page.align.freqs[page.align.freqs.length - 1] : 320
            xTicks: [20, 30, 40, 50, 60, 80, 100, 150, 200, 300]
            yMin: 50
            yMax: 95
            yStep: 5
            markers: page.align ? [{ x: page.align.crossover_hz, color: page.theme.accent, label: Math.round(page.align.crossover_hz) + " Hz" }] : []
            series: page.align ? [
              { xs: page.align.freqs, ys: page.align.mains_db, color: page.theme.cLeft, width: 1.5 },
              { xs: page.align.freqs, ys: page.align.sub_db, color: page.theme.cSub, width: 1.5 },
              { xs: page.align.freqs, ys: page.align.sum_before_db, color: page.theme.fg, width: 1.5, dash: [4, 3], alpha: 0.5 },
              { xs: page.align.freqs, ys: page.align.sum_after_db, color: page.theme.fg, width: 2.5 }
            ] : []
          }
          Legend {
            theme: page.theme
            items: [{ label: "Mains", color: page.theme.cLeft }, { label: "Sub", color: page.theme.cSub },
                    { label: "Sum before", color: page.theme.fg, dashed: true }, { label: "Sum aligned", color: page.theme.fg }]
          }
        }

        Card {
          theme: page.theme
          Layout.fillWidth: true
          Layout.fillHeight: true
          title: "Crossover: phase difference"
          subtitle: "Sub minus mains. Near 0° through the crossover = they add up"
          Graph {
            width: parent.width
            height: Math.max(180, (page.height - 120) * 0.40)
            theme: page.theme
            xMin: page.align ? page.align.freqs[0] : 20
            xMax: page.align ? page.align.freqs[page.align.freqs.length - 1] : 320
            xTicks: [20, 30, 40, 50, 60, 80, 100, 150, 200, 300]
            yMin: -180
            yMax: 180
            yStep: 90
            yUnit: "°"
            hlines: [{ y: 0, color: page.theme.alpha(page.theme.green, 0.6) }]
            bands: page.align ? [{ from: page.align.crossover_hz / 2, to: page.align.crossover_hz * 2, color: page.theme.alpha(page.theme.accent, 0.07) }] : []
            series: page.align ? [
              { xs: page.align.freqs, ys: page.align.phase_before_deg, color: page.theme.muted, width: 1.5, dash: [4, 3] },
              { xs: page.align.freqs, ys: page.align.phase_after_deg, color: page.theme.accent, width: 2.5 }
            ] : []
          }
          Legend {
            theme: page.theme
            items: [{ label: "Before alignment", color: page.theme.muted, dashed: true }, { label: "After", color: page.theme.accent }]
          }
        }
      }
    }

    ColumnLayout {
      Layout.preferredWidth: 320
      Layout.maximumWidth: 320
      Layout.fillHeight: true
      spacing: 16

      Card {
        theme: page.theme
        title: "Measured arrival"
        subtitle: "Relative to the first speaker"
        Layout.fillWidth: true
        Repeater {
          model: page.timing ? [
            ["Left", page.timing.arrival_ms.left, page.theme.cLeft],
            ["Right", page.timing.arrival_ms.right, page.theme.cRight],
            ["Sub", page.timing.arrival_ms.sub, page.theme.cSub]
          ] : []
          Row {
            required property var modelData
            width: parent.width
            Rectangle { width: 10; height: 10; radius: 5; color: modelData[2]; anchors.verticalCenter: parent.verticalCenter }
            Item { width: 8; height: 1 }
            Text { width: parent.width * 0.35; text: modelData[0]; color: page.theme.fg; font.family: page.theme.font; font.pixelSize: 12 }
            Text {
              width: parent.width * 0.55
              horizontalAlignment: Text.AlignRight
              text: Number(modelData[1]).toFixed(2) + " ms  (" + page.cm(modelData[1]) + " cm)"
              color: page.theme.fg; font.family: page.theme.font; font.pixelSize: 12
            }
          }
        }
        Text {
          width: parent.width
          wrapMode: Text.WordWrap
          text: "The sub's figure includes its own internal processing delay, so it usually reads later than its distance alone."
          color: page.theme.muted
          font.family: page.theme.font
          font.pixelSize: 10
        }
      }

      Card {
        theme: page.theme
        title: "Applied by the engine"
        Layout.fillWidth: true
        Repeater {
          model: page.timing ? [
            ["Left delay", page.applied("left").toFixed(2) + " ms"],
            ["Right delay", page.applied("right").toFixed(2) + " ms"],
            ["Sub delay", page.applied("sub").toFixed(2) + " ms"],
            ["Sub polarity", page.sum && page.sum.sub_invert ? "inverted" : "normal"],
            ["Summation", page.sum ? Math.round(page.sum.crossover_sum_before * 100) + "% → " + Math.round(page.sum.crossover_sum_after * 100) + "% of ideal" : "—"]
          ] : []
          Row {
            required property var modelData
            width: parent.width
            Text { width: parent.width * 0.45; text: modelData[0]; color: page.theme.muted; font.family: page.theme.font; font.pixelSize: 12 }
            Text { width: parent.width * 0.55; horizontalAlignment: Text.AlignRight; text: modelData[1]; color: page.theme.fg; font.family: page.theme.font; font.pixelSize: 12 }
          }
        }
      }

      Card {
        theme: page.theme
        title: "Reading these graphs"
        Layout.fillWidth: true
        Layout.fillHeight: true
        Text {
          width: parent.width
          wrapMode: Text.WordWrap
          lineHeight: 1.2
          text: "Arrival: the peaks should line up after alignment, so the kick of a drum hits from the sub and the mains together.\n\n"
              + "Crossover: the thick sum line should sit above both the mains and the sub near the crossover. If the two cancel, the sum dips there: the \"hollow\" sound. "
              + "Phase difference near 0° in the shaded band means they add up; ±180° means they cancel."
          color: page.theme.muted
          font.family: page.theme.font
          font.pixelSize: 11
        }
      }
    }
  }
}
