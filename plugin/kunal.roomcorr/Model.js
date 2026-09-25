// Pure helpers for the Room Correction panel. No QML dependencies so they
// can be checked with plain node.

// dBFS -> 0..1 for a meter spanning `floorDb`..0 dBFS.
function meterFraction(db, floorDb) {
  var f = floorDb || -60
  if (db === null || db === undefined || !isFinite(db)) return 0
  return Math.max(0, Math.min(1, (db - f) / -f))
}

function fmtDb(v, digits) {
  if (v === null || v === undefined || !isFinite(v)) return "—"
  var d = digits === undefined ? 1 : digits
  var s = Number(v).toFixed(d)
  return (v > 0 ? "+" : "") + s + " dB"
}

function fmtHz(v) {
  if (v === null || v === undefined || !isFinite(v)) return "—"
  return v >= 1000 ? (v / 1000).toFixed(v >= 10000 ? 0 : 1) + " kHz" : Math.round(v) + " Hz"
}

// Linear volume (0..1.5) as a percentage string.
function fmtPercent(v) {
  return Math.round((v || 0) * 100) + "%"
}

function statusLine(connected, state) {
  if (!connected) return "ENGINE OFFLINE"
  if (!state || !state.config) return "CONNECTING"
  var c = state.config
  if (c.mute) return "MUTED"
  if (!c.enabled) return "BYPASSED"
  var parts = []
  parts.push(c.room_eq ? (state.filters === "loaded" ? "EQ ON" : "EQ " + String(state.filters).toUpperCase()) : "EQ OFF")
  parts.push(c.bass_management ? "SUB " + Math.round(c.crossover_hz) + " HZ" : "SUB OFF")
  return parts.join(" · ")
}

// Picks the series to draw for a graph view from response.json.
// view: "bass" | "left" | "right" | "sub"
// Returns { fmin, fmax, series: [{ name, freqs, values, style }] }
// style: "target" | "before" | "after" | "measured"
function graphSeries(report, view) {
  var out = { fmin: 20, fmax: 20000, series: [] }
  if (!report || !report.freqs) return out
  var v = report.verified || null

  if (view === "bass") {
    out.fmin = 15
    out.fmax = 500
    out.series.push({ name: "Target", freqs: report.freqs, values: report.target, style: "target" })
    if (report.system) {
      out.series.push({ name: "Before", freqs: report.system.freqs, values: report.system.before, style: "before" })
      out.series.push({ name: "Predicted", freqs: report.system.freqs, values: report.system.after, style: "after" })
    }
    if (v && v.both) out.series.push({ name: "Measured", freqs: v.freqs, values: v.both, style: "measured" })
    return out
  }

  var ch = report.channels ? report.channels[view] : null
  if (view === "sub") {
    out.fmin = 15
    out.fmax = 500
  }
  out.series.push({ name: "Target", freqs: report.freqs, values: report.target, style: "target" })
  if (ch) {
    out.series.push({ name: "Before", freqs: report.freqs, values: ch.measured, style: "before" })
    out.series.push({ name: "Predicted", freqs: report.freqs, values: ch.corrected, style: "after" })
  }
  if (v && v[view]) out.series.push({ name: "Measured", freqs: v.freqs, values: v[view], style: "measured" })
  return out
}

// Vertical range that keeps the target in view with some room around it.
function graphRange(graph) {
  var lo = Infinity, hi = -Infinity
  for (var s = 0; s < graph.series.length; s++) {
    var ser = graph.series[s]
    if (ser.style !== "target") continue
    for (var i = 0; i < ser.freqs.length; i++) {
      if (ser.freqs[i] < graph.fmin || ser.freqs[i] > graph.fmax) continue
      lo = Math.min(lo, ser.values[i])
      hi = Math.max(hi, ser.values[i])
    }
  }
  if (!isFinite(lo)) return { lo: 50, hi: 90 }
  return { lo: Math.floor((lo - 20) / 5) * 5, hi: Math.ceil((hi + 12) / 5) * 5 }
}

function relativeAge(epochSeconds) {
  if (!epochSeconds) return ""
  var s = Math.max(0, Date.now() / 1000 - epochSeconds)
  if (s < 90) return "just now"
  if (s < 3600) return Math.round(s / 60) + " min ago"
  if (s < 86400 * 2) return Math.round(s / 3600) + " h ago"
  return Math.round(s / 86400) + " days ago"
}
