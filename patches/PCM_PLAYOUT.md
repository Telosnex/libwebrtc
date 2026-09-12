# External PCM playout

Apply external_pcm_playout.patch after custom_audio_source_m144.patch and
external_recording_demand.patch on core b1800a61. Desktop build scripts include
it. The wrapper base is .08 / e02f1ff8, not M150/main.

AudioState owns external mixer sources. It starts ADM for a first app source,
retains output when receivers leave, and releases only when neither owner needs
it. Explicit global SetPlayout(false) suspends PCM sources so null polling cannot
consume their queues; existing receivers still get their null poller. The
transport factory receives worker-thread AudioState lifetime notifications.

PcmPlayoutSource: 5s fixed-capacity PCM16LE mono/24kHz queue, whole-write reject,
generation + epoch validation, render-clock PushResampler, clearing of filter
tail, quiescent failed-stop retry. Real receiver mix goes through APM then ADM;
no render-preprocessing mutation, private device, capture injection or loopback.

Focused targets: pcm_playout_unittests, external_recording_demand_unittests.
Manual pcm_playout_smoke [output-index] plays 200ms of SILENCE through a real
output with capture off. Do not auto-run the hardware probe in CI. Tests and
smoke were run on pi5.local with the same compiled library as the deployed app.

Artifact receipt: build/pcm-arm64/artifact/pcm-build-receipt.json; experimental
matching header/library ZIP alongside. No published artifact or tag replaced.
