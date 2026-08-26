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

Published desktop wrapper artifacts (build validated, not audio-device
qualified):

```
https://github.com/Telosnex/libwebrtc/releases/tag/libwebrtc.m144.7559.09-telosnex.01
Linux x64:   f0156804d153c82e3b454ddbb1180d920e31f25d9a3ed69ef46be6dcfcc26ef7
Linux arm64: ea47d62ad084110b1552db2d6a1f665c6244eca38d90e976a2b6b4975310fb2d
Windows x64: f7d8f87fc309d982b747c24fbda3207636bc1e1916ab1a6857140d99c5dea4da
Windows arm64: 6ea130d19d7fc65639df12250f26df056c11f371972500f473373b007fb1b126
```

CI run `32924934052` compiled all four artifacts. Linux/Windows x64 executed
three focused wrapper tests and the core final-sender external-demand test;
arm64 compiled the same tests. The release and hashes are pinned in
flutter_webrtc and the desktop bridge is built without a rollout flag. Desktop
C1–C4 and route tests remain release-qualification gates.

Published Apple XCFramework (build validated, not physical-device qualified):

```
https://github.com/Telosnex/libwebrtc/releases/tag/libwebrtc.m144.7559.09-telosnex.02
SHA-256 9ba5491b7b3e754c30c4bbad307162d855f809455649e0d2506ca0eaea5219e4
```

CI run `32930758729` built iOS device/simulator, macOS, Catalyst, tvOS, and
visionOS slices. The iOS/macOS headers and binaries contain the Objective-C
external-demand API and final-sender guard. The SHA-pinned CocoaPods spec is in
`https://github.com/Telosnex/cocoapods-specs.git` at version
`144.7559.09-telosnex.02`.

## Headless Linux audio backend

Linux continues to use WebRTC's platform-default audio layer unless
`LIBWEBRTC_AUDIO_BACKEND=alsa` is present in the process environment. The
explicit override is intended for headless systems that expose ALSA devices but
do not run a PulseAudio server. It must be set before the peer-connection
factory is initialized.
