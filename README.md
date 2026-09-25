# roomcorr

Room correction and AV-receiver-style bass management for a PipeWire desktop,
built for a 2.1 system:

```
apps ─► "Room Correction" sink ─► roomcorr engine ─► Sound Blaster X4 (5.1 profile)
                                                        ├─ Front  ─► Marantz PM6003 ─► Boston A26 L/R
                                                        └─ C/Sub (ring = LFE) ─► Polk HTS 10
```

Measured with a miniDSP UMIK-1 (90° calibration file, mic pointing at the ceiling).

## Pieces

| | |
|---|---|
| `roomcorr daemon` | Realtime C++ engine (PipeWire). 5.1/stereo in → tone → Linkwitz-Riley crossover (L+R and LFE to the sub) → 262 144-tap FIR room correction per channel → trims, delays, sub polarity → level-matched bypass → safety limiter. ~6% of one core. |
| `roomcorr calibrate` | Measurement wizard (terminal, or JSON-driven by the Studio). |
| `roomcorr studio` | **Room Correction Studio**: live spectrum analyzer and signal flow, response graphs, target-curve editor, time/phase alignment views, and the graphical calibration. |
| `kunal.roomcorr` | Omarchy bar widget: meters, volume, sub level, crossover, tone, switches, response graph, and Studio launch buttons. |

## Install

```sh
./install.sh          # build, test, install engine + service + Studio + plugin + window rule
roomcorr setup        # once: X4 to 5.1, Room Correction becomes the default output
roomcorr studio calibrate
```

Settings live in `~/.config/roomcorr/config.json`. Filters, measurements and the
response report live in `~/.local/share/roomcorr/`. Impulse responses and filters
are 32-bit float WAVs, so REW can open them.

## How calibration works

1. **Levels.** Pink-noise bursts raise the test level until the mic reads 75 dB SPL
   (or 45 dB over the room noise). The wizard then finds which wire of the C/Sub jack
   the sub is on, after a few seconds of bass to wake the Polk from auto-standby.
2. **Measurement.** Log sweeps play left, right, then sub, back to back in one run at
   each mic position. Keeping all three in one run keeps their relative timing exact.
3. **Analysis.** Deconvolution, frequency-dependent windowing (15 cycles),
   1/24-octave smoothing in the bass easing to 1/3 octave in the treble, and power
   averaging across positions.
4. **Target.** The **Harman in-room curve**: a +6.5 dB bass shelf at 105 Hz and a
   −2.5 dB treble shelf. It rolls off below the system's natural limit, so no boost is
   wasted asking the sub for bass it can't play.
5. **Correction strength by frequency.** Following Toole, full-resolution correction
   runs up to the transition frequency (250 Hz). Above twice that, only a broad
   (octave-smoothed, ±2 dB) tonal correction is applied. Narrow EQ from a single mic
   above ~300 Hz chases reflections the ear ignores and makes speakers sound hollow.
   (Dirac's own ART limits itself to 20–150 Hz for the same reason.)
6. **Dips.** Boost is capped at 4 dB and only follows the surrounding third-octave.
   With several positions, only dips that stay put are boosted: a null that moves with
   the mic is fixed at one seat and made worse at the next.
7. **Mains.** Level-matched and time-aligned. An off-center mic is detected and its
   effect capped at ±2 dB and 1 ms, so the stereo image isn't pulled sideways.
8. **Sub.** The trim matches it to the target. The delay (±20 ms) and polarity are
   chosen for the best summation through the crossover over all positions, and the
   crossover itself (60–120 Hz) is picked for the flattest predicted bass, biased to 80 Hz.
9. **Joint bass correction.** After per-channel correction, the predicted *combined*
   L+R+sub response gets one common correction below the transition frequency.
   Because the same filter on all three scales their sum exactly and leaves alignment
   untouched, this flattens what you actually hear at the seat (Dirac Bass
   Control's idea).
10. **Filters.** Minimum-phase FIR (homomorphic), 262 144 taps (0.18 Hz resolution),
    with automatic headroom.
11. **Verify.** Sweeps played through the corrected system are shown against the
    prediction and the target on the Studio's Response tab.

For the most robust result, calibrate with **5+ positions** around your head.

## Keep in mind

- **Don't move the Marantz volume knob after calibrating.** It changes the mains
  but not the sub. Use the Room Correction volume (keyboard keys, bar, Studio).
- Polk HTS 10: set the low-pass knob to maximum (or use LFE in) and phase to 0°.
  roomcorr does the crossover and polarity. If the calibration says the sub is
  much louder than the mains, turn its volume knob down a little and recalibrate.
- Changing the house curve (bass shelf etc.) doesn't need new measurements: use
  **Rebuild filters** in the Studio, or `roomcorr design`.

## Control protocol

Newline-delimited JSON on `$XDG_RUNTIME_DIR/roomcorr.sock`:
`get`, `status` (`"spectrum": true` for the analyzer), `subscribe`, `set`
(`{"values": {"sub_gain_db": 2, "target.bass_boost_db": 8}}`), `save`, `reload`,
`quiet` (self-expiring mute used while measuring). `roomcorr ctl …` wraps it.

## Development

```sh
cmake -S . -B build && cmake --build build -j
build/roomcorr_tests          # DSP, crossover, LFE, FIR, deconvolution, synthetic-room calibration
tests/integration.sh          # silent end-to-end test through a null sink (own node names)
```
