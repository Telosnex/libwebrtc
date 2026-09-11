# External recording ownership

`external_recording_demand.patch` arbitrates app capture and WebRTC senders.
The pinned core is `webrtc-sdk/webrtc` at
`b1800a61db8320af5c14456c13622d8b85b1ed39`.

## Contract

- The ADM tracks external demand and WebRTC demand independently.
- The final sender clears only WebRTC demand.
- `AudioState::SetRecording(false)` preserves recording when external demand remains.
- App acquire initializes and starts capture on the owning worker thread.
- App release preserves recording when a WebRTC sender still needs it.
- Failed initialization does not create demand.
- Failed physical stop restores external demand for a retry.

The flags are atomic. The complete lifecycle operation is not lock-free.
Call acquire and release on the ADM worker thread.
External demand represents one app owner, not a reference count for multiple clients.
The app must serialize its ownership requests.

This patch does not register `AudioTransport` or create an APM.
Sender-free processed PCM still needs the app's media-engine anchor.
This patch does not include selective AEC recovery.

## Platform entry points

| Platform | Acquire | Release |
| --- | --- | --- |
| Desktop wrapper | `RTCAudioDeviceImpl` worker-thread helper | Native `ReleaseExternalRecording()` |
| Android | `PeerConnectionFactory.acquireAudioRecording()` | `releaseAudioRecording()` |
| Apple | `acquireExternalRecordingWithAudioProcessingOptions:` | `releaseExternalRecording` |

Android's factory retains the ADM and dispatches both operations to its worker.
`getAudioRecordingState()` returns one worker-thread snapshot.
Java-only `AudioRecord` helpers are not an ownership substitute.

Apple's AudioEngine acquire keeps processing options and engine start in one worker transaction.
Other Apple ADM types use the common acquire helper.
The Apple release method uses the common sender-aware helper.
Low-level `setExternalRecordingDemand:`, `startRecording`, and `stopRecording` remain available.
The app-owned capture bridge must not clear the flag and then call `stopRecording` directly.
That sequence can stop an active sender.

## Apply and build

Use a disposable checkout. Apply the patches in the recipe's order.
The hardware-clock API patch precedes the ownership patch.
Desktop builds also apply `custom_audio_source_m144.patch` before it.
Android applies both patches through its recipe.
Apple builds the Objective-C SDK rather than the desktop wrapper.

```sh
git apply --check /path/to/libwebrtc/patches/external_recording_demand.patch
git apply /path/to/libwebrtc/patches/external_recording_demand.patch
```

Build headers, implementation, and tests from the same source snapshot.
Do not enable a bridge against older binaries that lack its API.

## Test coverage

`external_recording_demand_unittests` covers the common ownership policy.
Linux and Windows x64 recipes run the full suite without a test filter.
ARM64 recipes build the same suite and retain it in a separate test bundle.
A compiled ARM64 test has not passed until it runs on a compatible host.

`external_recording_demand_objc_unittests` calls the real Objective-C wrapper with a mock ADM.
The Apple recipe runs it on the macOS host before the multi-slice build.
It checks worker dispatch, peerless capture, sender coexistence, stop retry,
initialization failure, and options-aware demand rollback.
It does not open a microphone or prove AudioEngine device behavior.

Physical-device acceptance remains separate for every platform:
peerless PCM, final-peer removal, app release during send, route changes,
failed lifecycle recovery, and disposal with queued work.

## Historical evidence

Older `.01` and `.02` releases tested the final-sender guard only.
Earlier macOS capture-continuity observations apply to those exact payloads.
They do not qualify the newer acquire/release API or current device deployment.
Use the release receipt, artifact SHA-256, and app payload identity for current evidence.

## Linux backend selection

`LibWebRTC::CreateRTCPeerConnectionFactory(RTCAudioBackend)` selects the backend
before ADM initialization. Platform default, ALSA, and PulseAudio are explicit choices.
The no-argument overload keeps platform-default behavior for existing callers.
