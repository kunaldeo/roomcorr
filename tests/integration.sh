#!/usr/bin/env bash
# Silent end-to-end test: runs the daemon against a 6-channel null sink with
# an isolated config, plays a 40 Hz tone on the left and 1 kHz on the right,
# and checks where the energy lands. Nothing reaches the real speakers.
set -euo pipefail
BIN=${BIN:-$(dirname "$0")/../build/roomcorr}
T=$(mktemp -d)
trap '[ -n "${SHOWLOG:-}" ] && cat "$T/daemon.log"; kill $DPID 2>/dev/null || true; pactl unload-module $MOD 2>/dev/null || true; [ -n "${KEEP:-}" ] && cp "$T/rec.wav" "$KEEP"; rm -rf "$T"' EXIT

mkdir -p "$T/cfg/roomcorr"
echo "{\"output_device\":\"rc_test_out\",\"sub_outputs\":[\"FC\",\"LFE\"]${EXTRA_CONFIG:-}}" > "$T/cfg/roomcorr/config.json"
MOD=$(pactl load-module module-null-sink sink_name=rc_test_out channels=6 \
  channel_map=front-left,front-right,front-center,lfe,rear-left,rear-right sink_properties=device.description=rc_test_out)
XDG_CONFIG_HOME=$T/cfg XDG_DATA_HOME=$T/data ROOMCORR_INSTANCE=test ROOMCORR_SOCKET=$T/rc.sock "$BIN" daemon >"$T/daemon.log" 2>&1 &
DPID=$!
sleep 1.5

python3 - "$T/test.wav" <<'PY'
import wave, struct, math, sys
fs=48000; n=fs*3
with wave.open(sys.argv[1],'wb') as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(fs)
    w.writeframes(b''.join(struct.pack('<hh', int(3276*math.sin(2*math.pi*40*i/fs)), int(3276*math.sin(2*math.pi*1000*i/fs))) for i in range(n)))
PY

pw-record --target rc_test_out -P stream.capture.sink=true --channels 6 --format f32 --rate 48000 "$T/rec.wav" &
RPID=$!
sleep 0.5
pw-play --target roomcorr_sink_test "$T/test.wav"
sleep 0.3
kill $RPID; wait $RPID 2>/dev/null || true

python3 - "$T/rec.wav" <<'PY'
import struct, array, math, sys
b=open(sys.argv[1],'rb').read(); i=12
while b[i:i+4]!=b'data': i+=8+struct.unpack('<I',b[i+4:i+8])[0]
a=array.array('f'); d=b[i+8:]; a.frombytes(d[:len(d)//24*24]); ch=6; n=len(a)//ch
act=[k for k in range(0,n,480) if max(abs(a[k*ch+c]) for c in range(ch))>1e-4]
if not act: sys.exit("FAIL: recording is silent")
s0,s1=act[0]+24000,act[-1]-24000
lv={}
for c,name in enumerate(["FL","FR","FC","LFE","RL","RR"]):
    s=a[c+s0*ch:c+s1*ch:ch]; lv[name]=20*math.log10(math.sqrt(sum(x*x for x in s)/len(s))+1e-12)
    def tone(f):
        import cmath
        w=2*math.pi*f/48000; acc=sum(x*cmath.exp(-1j*w*k) for k,x in enumerate(s[:48000]))
        return 20*math.log10(abs(acc)/48000*2/math.sqrt(2)+1e-12)
    print(f"  {name:4s} {lv[name]:6.1f} dBFS   40 Hz {tone(40):6.1f}   1 kHz {tone(1000):6.1f}")
# input tones are -23 dBFS RMS each
ok = lv["FC"] > -30 and abs(lv["FC"]-lv["LFE"]) < 0.1 and lv["FL"] < lv["FC"]-15 and lv["RL"] < -100 and lv["FR"] > -30
print("PASS" if ok else "FAIL"); sys.exit(0 if ok else 1)
PY
