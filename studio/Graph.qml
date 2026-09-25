import QtQuick

// A plot on a Canvas: log or linear x, grid, labels, markers, and any
// number of series. Series: { xs, ys, color, width, dash: [..],
// fill: bool, alpha, offset (added to x) }.
Canvas {
  id: g
  property var theme
  property var series: []
  property real xMin: 20
  property real xMax: 20000
  property bool logX: true
  property real yMin: 40
  property real yMax: 100
  property real yStep: 10
  property var xTicks: [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]
  property string xUnit: "Hz"   // "Hz" or "ms"
  property string yUnit: "dB"
  property var markers: []      // { x, color, label }
  property var bands: []        // { from, to, color } shaded x-ranges
  property var hlines: []       // { y, color, dash }

  readonly property real padL: 42
  readonly property real padR: 12
  readonly property real padT: 10
  readonly property real padB: 22

  renderStrategy: Canvas.Cooperative
  onSeriesChanged: requestPaint()
  onWidthChanged: requestPaint()
  onHeightChanged: requestPaint()
  onYMinChanged: requestPaint()
  onYMaxChanged: requestPaint()
  onXMinChanged: requestPaint()
  onXMaxChanged: requestPaint()
  onMarkersChanged: requestPaint()
  Connections {
    target: g.theme
    function onFgChanged() { g.requestPaint() }
  }

  function px(x) {
    var w = width - padL - padR
    if (logX) return padL + (Math.log(x) - Math.log(xMin)) / (Math.log(xMax) - Math.log(xMin)) * w
    return padL + (x - xMin) / (xMax - xMin) * w
  }
  function py(y) {
    var h = height - padT - padB
    return padT + (1 - (y - yMin) / (yMax - yMin)) * h
  }
  function fmtX(x) {
    if (xUnit === "ms") return String(x)
    return x >= 1000 ? (x / 1000) + "k" : String(x)
  }
  function col(c, a) { return Qt.rgba(c.r, c.g, c.b, a === undefined ? 1 : a) }

  onPaint: {
    var ctx = getContext("2d")
    ctx.reset()
    if (!theme) return
    var x0 = padL, x1 = width - padR, y0 = padT, y1 = height - padB
    ctx.font = "11px \"" + theme.font + "\""

    // frame + bands
    ctx.fillStyle = col(theme.bgDarker, 0.6)
    ctx.fillRect(x0, y0, x1 - x0, y1 - y0)
    for (var b = 0; b < bands.length; b++) {
      var bd = bands[b]
      var bx0 = Math.max(x0, px(Math.max(bd.from, xMin))), bx1 = Math.min(x1, px(Math.min(bd.to, xMax)))
      if (bx1 <= bx0) continue
      ctx.fillStyle = bd.color
      ctx.fillRect(bx0, y0, bx1 - bx0, y1 - y0)
    }

    // grid
    ctx.lineWidth = 1
    ctx.strokeStyle = col(theme.fg, 0.08)
    ctx.fillStyle = col(theme.fg, 0.45)
    for (var t = 0; t < xTicks.length; t++) {
      var xv = xTicks[t]
      if (xv < xMin || xv > xMax) continue
      var xp = Math.round(px(xv)) + 0.5
      ctx.beginPath(); ctx.moveTo(xp, y0); ctx.lineTo(xp, y1); ctx.stroke()
      var lbl = fmtX(xv)
      ctx.fillText(lbl, xp - ctx.measureText(lbl).width / 2, height - 6)
    }
    var first = Math.ceil(yMin / yStep) * yStep
    for (var yv = first; yv <= yMax + 1e-9; yv += yStep) {
      var yp = Math.round(py(yv)) + 0.5
      ctx.beginPath(); ctx.moveTo(x0, yp); ctx.lineTo(x1, yp); ctx.stroke()
      var yl = String(Math.round(yv * 10) / 10)
      ctx.fillText(yl, x0 - 6 - ctx.measureText(yl).width, yp + 4)
    }
    for (var h = 0; h < hlines.length; h++) {
      var hl = hlines[h]
      ctx.strokeStyle = hl.color
      ctx.setLineDash(hl.dash || [])
      ctx.beginPath(); ctx.moveTo(x0, py(hl.y)); ctx.lineTo(x1, py(hl.y)); ctx.stroke()
    }
    ctx.setLineDash([])

    // series
    ctx.save()
    ctx.beginPath(); ctx.rect(x0, y0, x1 - x0, y1 - y0); ctx.clip()
    for (var s = 0; s < series.length; s++) {
      var ser = series[s]
      if (!ser || !ser.xs || !ser.ys || ser.ys.length === 0) continue
      var off = ser.offset || 0
      var pts = []
      for (var i = 0; i < ser.xs.length && i < ser.ys.length; i++) {
        var xx = ser.xs[i] + off, yy = ser.ys[i]
        if (yy === null || yy === undefined || !isFinite(yy)) continue
        if (xx < xMin * 0.98 || xx > xMax * 1.02) continue
        pts.push([px(xx), py(Math.max(yMin - 5, Math.min(yMax + 5, yy)))])
      }
      if (pts.length < 2) continue
      if (ser.fill) {
        ctx.beginPath()
        ctx.moveTo(pts[0][0], y1)
        for (var p = 0; p < pts.length; p++) ctx.lineTo(pts[p][0], pts[p][1])
        ctx.lineTo(pts[pts.length - 1][0], y1)
        ctx.closePath()
        ctx.fillStyle = col(ser.color, ser.fillAlpha === undefined ? 0.18 : ser.fillAlpha)
        ctx.fill()
      }
      ctx.beginPath()
      ctx.moveTo(pts[0][0], pts[0][1])
      for (var q = 1; q < pts.length; q++) ctx.lineTo(pts[q][0], pts[q][1])
      ctx.strokeStyle = col(ser.color, ser.alpha === undefined ? 1 : ser.alpha)
      ctx.lineWidth = ser.width || 1.5
      ctx.setLineDash(ser.dash || [])
      ctx.stroke()
      ctx.setLineDash([])
    }
    ctx.restore()

    // markers
    for (var m = 0; m < markers.length; m++) {
      var mk = markers[m]
      if (mk.x < xMin || mk.x > xMax) continue
      var mx = Math.round(px(mk.x)) + 0.5
      ctx.strokeStyle = mk.color
      ctx.setLineDash([3, 3])
      ctx.beginPath(); ctx.moveTo(mx, y0); ctx.lineTo(mx, y1); ctx.stroke()
      ctx.setLineDash([])
      if (mk.label) {
        ctx.fillStyle = mk.color
        ctx.fillText(mk.label, Math.min(mx + 4, x1 - ctx.measureText(mk.label).width - 2), y0 + 12)
      }
    }
    // unit
    ctx.fillStyle = col(theme.fg, 0.35)
    ctx.fillText(yUnit, 4, y0 + 10)
  }
}
