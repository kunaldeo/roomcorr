import QtQuick
import Quickshell
import Quickshell.Io

// Connection to the roomcorr engine: state, meters and live spectra.
Item {
  id: d
  visible: false

  property bool connected: false
  property var state: null
  property var status: null
  property var freqs: []
  // Display spectra (dBFS per 1/6-octave band), with fast attack and a
  // slow fall so the analyzer reads like a hardware RTA.
  property var spec: ({ "in": [], "left": [], "right": [], "sub": [] })
  property var hold: ({ "in": [], "left": [], "right": [], "sub": [] })
  property int intervalMs: 50

  readonly property var cfg: state && state.config ? state.config : null

  Socket {
    id: sock
    path: Quickshell.env("ROOMCORR_SOCKET") || ((Quickshell.env("XDG_RUNTIME_DIR") || "/tmp") + "/roomcorr.sock")
    connected: true
    parser: SplitParser { onRead: function(line) { d.handle(line) } }
    onConnectionStateChanged: {
      d.connected = sock.connected
      if (sock.connected) d.send({ cmd: "subscribe", interval_ms: d.intervalMs, spectrum: true })
    }
  }

  // Reconnect when the engine restarts.
  Timer {
    interval: 2000
    running: !sock.connected
    repeat: true
    onTriggered: { sock.connected = false; sock.connected = true }
  }

  function send(obj) {
    if (!sock.connected) return
    sock.write(JSON.stringify(obj) + "\n")
    sock.flush()
  }
  function set(key, value) {
    var v = {}
    v[key] = value
    send({ cmd: "set", values: v })
  }
  function setMany(values) { send({ cmd: "set", values: values }) }
  function save() { send({ cmd: "save" }) }
  function reload() { send({ cmd: "reload" }) }

  function handle(line) {
    var m
    try { m = JSON.parse(line) } catch (e) { return }
    if (m.type === "state") {
      d.state = m
      if (m.spectrum_freqs) d.freqs = m.spectrum_freqs
    } else if (m.type === "status") {
      d.status = m
      if (m.spectrum) smooth(m.spectrum)
    }
  }

  function smooth(sp) {
    var out = {}, hl = {}
    var keys = ["in", "left", "right", "sub"]
    for (var k = 0; k < keys.length; k++) {
      var key = keys[k]
      var nv = sp[key] || [], ov = d.spec[key] || [], oh = d.hold[key] || []
      var r = [], h = []
      for (var i = 0; i < nv.length; i++) {
        var o = i < ov.length ? ov[i] : -100
        r.push(nv[i] > o ? nv[i] : Math.max(nv[i], o - 1.5))
        var p = i < oh.length ? oh[i] : -100
        h.push(nv[i] > p ? nv[i] : Math.max(nv[i], p - 0.25))
      }
      out[key] = r
      hl[key] = h
    }
    d.spec = out
    d.hold = hl
  }
}
