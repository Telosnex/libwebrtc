# External recording demand patch

`external_recording_demand.patch` preserves app-owned ADM capture when WebRTC's
last audio send stream is removed.

Pinned Darwin source used for the qualified artifact:

```
webrtc-sdk/webrtc b1800a61db8320af5c14456c13622d8b85b1ed39
WebRTC-SDK 144.7559.09
```

Apply from the WebRTC source root:

```bash
git checkout --detach b1800a61db8320af5c14456c13622d8b85b1ed39
git apply /path/to/libwebrtc/patches/external_recording_demand.patch
```

The patch adds a thread-safe external demand flag to `AudioDeviceModule`,
exposes it through the Darwin Objective-C ADM wrapper, and gates only
`AudioState::RemoveSendingStream()`'s final-sender `StopRecording()` call. It
also includes a focused `AudioStateTest`.

The same core guard is required by the m144 Windows/Linux wrapper capture API.
For desktop prebuilts, apply this patch after
`custom_audio_source_m144.patch`, then compile the wrapper source and exported
headers from the same checkout. The Flutter desktop bridge must not be enabled
against the stock `libwebrtc.m144.7559.09` archives because those archives lack
the new wrapper symbols.

This patch does not register `AudioTransport`. Sender-free post-APM capture
still needs the app's bare media-engine anchor to outlive all real peers.

Qualified macOS arm64 artifact:

```
build/out-release/WebRTC-m144.7559.09-external-demand-macos-arm64.xcframework.zip
SHA-256 4fb59bccf933fc88db1c1a973b7e75b497a9215cdbb93acbfc44224dcb771d06
```

Runtime qualification is recorded in Telosnex ADR 004 and
`test-results/log_5.txt`: two Live teardowns retained the same capture
generation with 31–33 ms post-teardown PCM gaps and no ADM restart.

Locally build-validated Linux x64 wrapper artifact (not published or
audio-device qualified):

```
build/libwebrtc-linux-x64-release-external-demand.zip
SHA-256 c59e331ca2a30333de4b15c48e1628186ee0f8fd45ac970e41a0b810888712cd
```

Its three focused wrapper tests pass in an amd64 Ubuntu container. This hash is
for build provenance only; do not pin it for release until Linux C1–C4 and
route tests pass and the artifact is published under an immutable version.
