# Telosnex AEC guarantees and replay

This directory owns the release-gated checks for Telosnex split-clock AEC
hardening.

## Automated guarantees

`aec_guarantees.test.cc` uses generated fixtures to check:

- ALSA hardware-position regression takes control from 1 ms-quantized counters
  in about 5.5 seconds;
- counter generations invalidate windows across XRUN/recovery;
- observe mode cannot change audio correction;
- control mode engages large drift without a seed and never engages in-spec
  hardware;
- a startup seed is superseded by a confident hardware estimate;
- callback-estimator engagement and correction rails remain available as a
  fallback;
- self-echo gate close/open/double-talk behavior;
- tap-v3 atomically records all audio formats, immutable seed, PCM, pacing, and
  both hardware-clock streams.

The Linux release build compiles the guarantees and replay tool for x64 and
ARM64, executes the guarantees on x64, and runs `smoke_replay.py`. The smoke
test generates a complete tap-v3 bundle, obtains hardware-servo control through
the real estimator, runs the real APM, and validates the output WAV. No customer
audio is stored in git.

## ALSA hardware observations

ALSA keeps `hw_ptr` opaque in its public API. The ADM derives an equivalent,
unwrapped position from an atomic `snd_pcm_status` snapshot:

```text
playout hardware position = successful written frames - status delay
capture hardware position = successful read frames + status delay
```

Each position is paired with `CLOCK_MONOTONIC`. A generation changes after
prepare/XRUN recovery, preventing a fit from crossing a counter discontinuity.
The estimator separately regresses normalized capture and playout rates against
the same clock and passes their ratio to the existing bounded resampler.

Runtime modes:

```text
TSNX_HW_CLOCK_SERVO=observe   # estimate/log only; cannot alter correction
TSNX_HW_CLOCK_SERVO=control   # estimator may own bounded correction
TSNX_DRIFT_PPM=-1700          # optional startup fallback until hardware control
```

## Silent hardware probe

`tsnx_alsa_hw_clock_probe` opens a selected ALSA capture/playout pair, renders
zeros, discards captured PCM, and prints the production estimator result. It is
for development hardware validation and makes no correction:

```bash
tsnx_alsa_hw_clock_probe --list
tsnx_alsa_hw_clock_probe --playout 2 --capture 1 --seconds 12
```

Reference Pi 5 validation (2026-08-29), using HDMI-1 `plughw` plus the USB mic
`plughw`, produced `-1781.96 +/- 30.19 ppm` after 20 seconds with zero resets or
rejections. A forced 2-second process stall caused an ALSA recovery generation;
the estimator discarded the old window, reacquired near `-1700 ppm` by 5.5
seconds, and finished with one reset and zero rejected observations.

## Deterministic field replay

A tap-v3 bundle contains:

```text
manifest.json
render.pcm             render.log
capture_raw.pcm        capture_raw.log
capture_apm.pcm        capture_apm.log
playout_hw.log         capture_hw.log
```

PCM is headerless signed 16-bit little-endian. Audio logs contain `t_us frames`.
Hardware logs contain `t_ns position_frames rate generation`. One directory
covers one recorder/transport lifetime; timing gaps preserve call boundaries.

Replay through the real APM:

```bash
tsnx_replay <bundle>                         # stock path
tsnx_replay <bundle> --servo                 # unseeded callback fallback
tsnx_replay <bundle> --servo --seed -1700    # callback path with seed
tsnx_replay <bundle> --hw-servo              # recorded hardware positions
tsnx_replay <bundle> --hw-servo --seed -1700 # production startup fallback
tsnx_replay <bundle> --ratio -1700           # independent fixed control
tsnx_replay <bundle> --output /tmp/after.wav
```

CSV output includes APM statistics, correction state, hardware estimate and
uncertainty, fit span, estimate count, counter resets, and rejected data.
Tap-v2 and original canonical-WAV/tap-v1 evidence remain readable, but only
v3 bundles contain the observations required by `--hw-servo`.

`TSNX_REPLAY_FULL=1` additionally routes replay through the observer gate and a
fresh RT-safe tap, useful under ASAN.

## Privacy

`TSNX_TAP_DIR` records raw microphone PCM. It must be unset in normal
production. Arm it only for an explicitly approved diagnostic recording and
disarm it immediately afterward.
