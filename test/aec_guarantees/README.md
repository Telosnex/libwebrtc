# Telosnex AEC guarantees and replay

This directory owns the release-gated checks for the Telosnex split-clock AEC
hardening.

## Automated guarantees

`aec_guarantees.test.cc` generates deterministic PCM fixtures in memory and
checks:

- an in-spec clock never engages the resampler;
- a large clock mismatch engages while correction remains clamped;
- seed acceptance/rejection rails;
- pure self-echo closes the observer gate and render silence reopens it;
- near-end-only audio never closes the gate and input audio is not mutated;
- strong double-talk reopens a previously closed gate;
- tap-v2 writes all three stream formats, the immutable seed, pacing data, and
  the expected PCM byte counts.

The Linux release build compiles this target for x64 and ARM64 and executes it
on x64. It also compiles `tsnx_replay` on both architectures. On x64,
`smoke_replay.py` creates a complete tap-v2 bundle using only generated audio,
runs the real APM replay, and validates the emitted WAV.

The generated fixtures enforce structural behavior without storing customer
microphone recordings in git. Historical field recordings remain private
postmortem evidence; they are not CI dependencies.

## Deterministic field replay

A tap-v2 bundle contains:

```text
manifest.json
render.pcm             render.log
capture_raw.pcm        capture_raw.log
capture_apm.pcm        capture_apm.log
```

PCM is headerless signed 16-bit little-endian. Logs contain `t_us frames`.
One directory covers one recorder/transport lifetime; timing gaps in the logs
preserve boundaries between calls. Restart the process before recording when a
single-call directory is desired.

Replay through the real APM:

```bash
tsnx_replay <bundle>                         # stock path
tsnx_replay <bundle> --servo                 # estimator-driven servo
tsnx_replay <bundle> --ratio -1700           # fixed-ratio comparison
tsnx_replay <bundle> --output /tmp/after.wav
```

The tool emits per-second APM statistics on stdout and always writes a
listenable post-AEC WAV. It accepts both tap-v2 bundles and the original
canonical-WAV/tap-v1 evidence format. `TSNX_REPLAY_FULL=1` additionally routes
the replay through the observer gate and a fresh RT-safe tap, useful under
ASAN.

## Privacy

`TSNX_TAP_DIR` records raw microphone PCM. It must be unset in normal
production. Arm it only for an explicitly approved diagnostic recording and
disarm it immediately afterward.
