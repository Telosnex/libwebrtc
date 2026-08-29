#!/usr/bin/env python3
"""Generate a tiny tap-v2 bundle and verify replay produces a valid WAV."""

import math
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import wave

RATE = 48_000
BLOCK = RATE // 100
FRAMES = 200


def pcm_frame(frame: int) -> list[int]:
    gain = 0.15 + 0.1 * (0.5 + 0.5 * math.sin(frame * 0.11))
    return [
        round(
            32767
            * gain
            * math.sin(2 * math.pi * (330 + 17 * (frame % 13)) * (frame * BLOCK + i) / RATE)
        )
        for i in range(BLOCK)
    ]


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: smoke_replay.py <tsnx_replay>")
    replay = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="tsnx_replay_smoke_") as tmp:
        bundle = pathlib.Path(tmp)
        bundle.joinpath("manifest.json").write_text(
            """{
  "version": 2,
  "scope": "recorder_lifetime",
  "format": "s16le",
  "render": {"rate": 48000, "channels": 2},
  "capture_raw": {"rate": 48000, "channels": 2},
  "capture_apm": {"rate": 48000, "channels": 1},
  "drift_seed_ppm": -1700.0,
  "pacing_log": "<stream>.log lines: t_us frames"
}
""",
            encoding="utf-8",
        )
        render_pcm = bytearray()
        capture_pcm = bytearray()
        render_log: list[str] = []
        capture_log: list[str] = []
        base_us = 10_000_000
        for frame in range(FRAMES):
            mono = pcm_frame(frame)
            stereo = [sample for value in mono for sample in (value, value)]
            packed = struct.pack(f"<{len(stereo)}h", *stereo)
            render_pcm.extend(packed)
            # A deterministic attenuated echo-only capture.
            capture_pcm.extend(
                struct.pack(f"<{len(stereo)}h", *(sample // 5 for sample in stereo))
            )
            render_log.append(f"{base_us + frame * 10_000} {BLOCK}\n")
            capture_log.append(f"{base_us + frame * 10_000 + 5_000} {BLOCK}\n")
        bundle.joinpath("render.pcm").write_bytes(render_pcm)
        bundle.joinpath("capture_raw.pcm").write_bytes(capture_pcm)
        bundle.joinpath("render.log").write_text("".join(render_log), encoding="utf-8")
        bundle.joinpath("capture_raw.log").write_text(
            "".join(capture_log), encoding="utf-8"
        )
        output = bundle / "after.wav"
        env = os.environ.copy()
        env["TSNX_REPLAY_FULL"] = str(bundle / "full_tap")
        result = subprocess.run(
            [str(replay), str(bundle), "--output", str(output)],
            check=True,
            capture_output=True,
            text=True,
            env=env,
        )
        expected_header = (
            "t_s,mode,render_active,erl_db,erle_db,servo_engaged,"
            "servo_measured_ppm,servo_applied_ppm,servo_windows,"
            "servo_anomalies\n"
        )
        if not result.stdout.startswith(expected_header):
            raise AssertionError(f"missing CSV header: {result.stdout[:160]!r}")
        with wave.open(str(output), "rb") as wav:
            assert wav.getframerate() == RATE
            assert wav.getnchannels() == 1
            assert wav.getsampwidth() == 2
            assert wav.getnframes() == FRAMES * BLOCK
        print("tap-v2 replay smoke passed")


if __name__ == "__main__":
    main()
