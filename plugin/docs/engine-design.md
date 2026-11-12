# Griddy engine design (Milestone 1)

This document is written before the engine code. It says what the engine does, which parts exist, which thread owns what, and what is known to be unprovable without a Mac and a real DAW. Where the code later disagrees with this file, the file gets fixed in the same commit.

## 1. What the engine is for

Griddy is an overdub video recorder inside a DAW. A grid of up to 9 slots, one slot per audio track. Each slot plays the video of the take you performed for that track. The video must behave like the audio it belongs to:

* the DAW plays, the videos play; the DAW stops, the videos hold
* the audio clip loops or is duplicated on the timeline, the video repeats with it
* the audio clip is trimmed, split or moved, the video follows
* a new take in the DAW also records a new video, and that video replaces the previous take's video in the slot

## 2. The hard fact that shapes everything

A plugin cannot see the DAW's arrangement. It receives the transport (tempo, position, play, loop) and the audio that flows through the track it sits on. It does not receive clip start times, clip offsets, warp markers or which take is current. Ableton Live has no ARA support, so that door is closed there. Everything else in this design follows from that.

So the engine has to work out which moment of which take is currently audible. That is the job of a **ClipSource**: it answers "for this slot, which take, and what position inside that take, right now, and how sure are we".

## 3. ClipSource: three ways to know where the clip is

Ranked by how trustworthy they are. The SyncController uses the best one that is currently locked.

1. **EmbeddedSyncClipSource.** For takes Griddy wrote itself (a `.gtake` or a `.mov` that Griddy produced). Griddy mixes a faint, near inaudible differential BPSK carrier (about 18 kHz, about 100 baud) into the audio of a take it records, carrying a sync word, a take id, a position counter and a CRC. Any copy of that audio, duplicated, looped, trimmed or moved, tells the plugin exactly which take and which sample it is playing. Limits: lossy codecs, warping, effects placed before the plugin, and bounces can destroy it. Realistic lock time is 0.4 to 1 second (one frame of the code is about 80 to 100 ms; several are needed to be sure). The first prompt asked for 100 ms. That is not met by this design and the measured number is reported in `engine-benchmarks.md`.
2. **FingerprintClipSource.** For takes recorded by the DAW itself (the common Live case: the DAW writes the wav, Griddy has no say over it). Griddy keeps its own copy of the take's audio, captured through the plugin while the DAW recorded. At playback it fingerprints the incoming audio and matches it against the take audio: resample to 8 kHz, FFT 512 with hop 64, pick spectral peaks, hash peak pairs into landmarks, vote on the time offset between the live stream and the take, then refine with a raw correlation over a short window. A host position prior (see 3) gives an instant first guess so the picture is never blind. When a loop wraps, the source predicts the wrap at the take end instead of waiting to re match.
3. **HostPositionClipSource.** The fallback. It assumes the clip starts where the take was recorded (the host position stored with the take) and maps host position to take position, honouring the host loop range. It is exactly right for an unmoved clip and wrong the moment the clip is moved. It is used alone only when nothing better is locked, and as the prior for the fingerprint matcher.

Optional later ClipSources that fit the same interface: an ARA2 document controller for hosts that support it (not Live), and a Max for Live companion that reads the arrangement through the Live Object Model. Neither is built in this milestone. The Max for Live files in the repo are not touched.

A ClipSource never runs on the audio thread except for `feed()`, which only copies samples into a lock free ring. Matching and decoding run on worker threads.

## 4. Components

All under `plugin/source/engine`, namespace `griddy`. The core uses plain C++ and the existing `TransportState`. JUCE appears only in the glue (image codecs, files, the editor).

| Component | Role |
| --- | --- |
| `ClipSource` (interface) | `feed(slot audio block, host transport)` from the audio thread, `query()` from the sync thread. Returns a `ClipFix`: take id, position in the take in seconds, confidence, kind, locked flag. |
| `EmbeddedSyncClipSource`, `FingerprintClipSource`, `HostPositionClipSource` | The three sources above. |
| `SyncController` | Per slot. Holds the best locked fix, predicts forward with the host clock between fixes, rejects jumps that disagree with the prediction until they repeat, and publishes one `SlotTarget` (take id, position, rate, playing) as a seqlock snapshot for the player. Also applies the drift rule: while the shown frame is within the drift threshold of the target, do nothing; beyond it, resync. The old Max device used 80 ms. |
| `TakeRecorder` | Writes a take per slot while recording: the camera frames (intraframe only) with capture timestamps, plus the audio copy used for fingerprinting, plus the embedded sync mix. One writer thread per take, fed by lock free queues. |
| `TakeHistory` | Per slot, the last 5 takes, each with its audio and video files and the host position it was recorded at. The newest take is the active one. Replacing is just promoting a newer take: the video follows the audio that plays. Keep, retake and slip (shift the video against the audio by ms or frames) are edits on the take's metadata, never on the media files. |
| `VideoDecoder` (interface) | `open(take)`, `decode(frameIndex) -> Frame`. Implementations: the portable one used by tests and Linux (JPEG intraframe take format `.gtake`), and per platform hardware backends (VideoToolbox on Mac, Media Foundation on Windows) that are written to the same interface but cannot be verified here. |
| `FrameQueue` | Single producer, single consumer ring of decoded frames per slot, with frame index tags. Frames are pre decoded a few frames ahead of the target and, near the end of a take, from the start of the take (loop pre roll) so a loop wrap shows no hitch. |
| `SlotPlayer` | One decode thread per slot (or a pool). Reads the `SlotTarget`, decodes ahead into the FrameQueue, holds the last frame when the take ends and the DAW is not looping it, shows the placeholder when there is no take. |
| `Compositor` (CPU reference) | The existing cover crop compositor, grid only. The reference the GPU path is compared against. |
| `GpuCompositor` | Metal on Mac, D3D11 or OpenGL elsewhere, textures per slot, one draw per cell. Written against an interface, the CPU compositor is the fallback and the test oracle. Not verifiable here. |
| `Harness` (console app) | Feeds N synthetic slot audio streams and a fake transport, runs the whole engine without a DAW or GPU, prints measurements. |

## 5. Take file format

`.gtake` is a folder: `video/000000.jpg ...` (every frame is an intraframe, so any frame decodes alone and seeking is free), `audio.wav` (the plugin's copy of the track audio, mono 24 bit at the DAW rate), `take.json` (take id, slot, frame rate, size, start host position in quarter notes and in samples, tempo, time signature, per frame capture time, slip). A folder of JPEGs is not a good final format but it is portable, fully testable on Linux and random access by construction. The Mac backend writes ProRes Proxy or all intraframe H.264 `.mov` through AVFoundation, behind the same `TakeWriter` interface. That backend is written but unverified.

Why intraframe: the player must jump to any frame at any time (clip trimmed, looped, moved). Long GOP codecs make that expensive and are the main risk for 9 streams.

## 6. Thread map

| Thread | Does | Must never |
| --- | --- | --- |
| Audio (DAW) | Publishes `TransportState`; calls `ClipSource::feed` which copies samples into a lock free ring; calls `TakeRecorder::feed` the same way. | Allocate, lock, touch the disk, decode, call into JUCE message or GUI code. |
| Camera callback | Pushes the newest frame into `FrameStore`, and into the TakeRecorder queue while recording. | Block. |
| Take writer (one per active recording) | Drains queues, encodes JPEG, writes files, finalises `take.json`. | Run on audio or message thread. |
| Sync worker (one for all slots) | Runs fingerprint and embedded sync decoders, updates each `SyncController`, publishes `SlotTarget`. | Block the decoders on the writer. |
| Decode (one per slot) | Reads `SlotTarget`, decodes ahead, fills `FrameQueue`. | Wait on anything the audio thread holds. |
| Render (30 fps, existing engine thread) | Reads the newest frame of each slot, composites, makes the preview JPEG, publishes it. | Decode. |
| Message | UI calls, take history edits, file choosers. | Anything long; those jump to a worker. |

All hand offs between threads are single writer seqlocks (small structs) or single producer single consumer rings (audio, frames). The only mutex users are the message thread and cold paths.

## 7. Embedded sync details

* Carrier: 18 kHz, differential BPSK, about 100 baud, shaped so it stays well under the audible threshold at roughly minus 60 dBFS relative to program (to be tuned by ear on real material, not provable here).
* Frame: sync word (16 bits), take id (16 bits), position counter in frames (24 bits), CRC (8 bits). One frame carries the position of its start, so any decoded frame gives an absolute take position plus the sample offset within the frame found by correlation.
* Detector: bandpass, demodulate, correlate against the sync word, confirm with the CRC. Two consecutive valid frames with consistent counters give lock.
* Consequences stated plainly: survives gain changes, duplication, trim, loop and move; may survive mild MP3; does not survive time stretch, heavy EQ above 16 kHz, a lowpass on the track before Griddy, or a bounce that drops the band.

## 8. Take workflow

1. DAW record starts (or Griddy is armed). TakeRecorder opens a take for the armed slot, stamps it with the host position, and begins writing camera frames and the audio copy.
2. DAW record stops. The take goes to review: Keep, Retake, or slip the video against the audio. Retake discards it (still kept in history for the last 5 takes).
3. The next time the user records over the same slot, the new take becomes the slot's active take and the previous take's video stops being shown. The previous take stays in `TakeHistory` for undo.
4. The audio the DAW plays is whatever the user left on the timeline. The video follows the audio that the ClipSource identifies. If the user moves the old audio clip and mutes the new one, the video of the old take plays, because that is what the audio is.

## 9. Frame timing and drift

* Frame pacing for playback comes from the sync target, not from a free running 30 fps clock. The renderer picks the frame nearest to `targetPosition` of each slot.
* Drift threshold 80 ms (to be tuned from benchmarks): inside it keep playing; outside it, jump to the target frame.
* End of take while the DAW still plays and the clip does not loop: hold the last frame. Loop: the pre rolled frames from the take start are already in the queue.
* Latency: the video shown is the newest frame decoded for a target that is already a few frames old; the harness measures the end to end offset between audio position and shown frame.

## 10. Budgets and measured targets

Targets for 9 slots at 720p (preview) and 1080p (output), 30 fps, a loop several minutes long: decode plus composite inside 33 ms per frame, no unbounded memory growth over a long loop, CPU use per core reported. These are measured in the headless harness on this Linux machine (2 cores). The harness numbers are CPU only and are not a prediction for Apple Silicon with hardware decode. The GPU path and hardware decode are not measured at all here.

## 11. What cannot be verified in this environment

* any `.mm` file (AVFoundation capture, writer, VideoToolbox decode, Metal compositor, host permission check)
* the AU wrapper and `auval`
* anything inside real Ableton Live 12: loop wrap behaviour, the transport values it really reports during loop and punch, clip move and trim reactions, record in Session View versus Arrangement
* hardware decode throughput and GPU compositing cost
* the audibility of the 18 kHz carrier on real material and the real world lock rate of the fingerprint on dense mixes

The macOS job in `.github/workflows/plugin.yml` compiles the Mac files and runs the same tests; it is the first check for those. The manual Ableton Live 12 test plan at the end of Milestone 1 covers the rest.

## 12. Non goals for this milestone

Max for Live files, ARA2, final UI wiring for every control, Windows capture, the GPU compositor implementation beyond its interface.

## As built (corrections to the design above)

- Take format is a `.gtake` folder: `take.json`, `audio.wav`, `frames/NNNNNN.jpg` (full size) and `proxy/NNNNNN.jpg` (tile size). Not a `.mov`. It is crash safe and intraframe by construction. Griddy `.mov` export for the Arrangement View is not written.
- No FFmpeg. Decode and encode use JUCE's JPEG codec behind `VideoDecoder`. Hardware decoders (VideoToolbox, Media Foundation) are not written.
- The engine uses `juce_core`, `juce_graphics` and `juce_events` only.
- Proxy or full frames is chosen automatically: tiles of 800 px or less use the proxy.
- Compositing is the CPU compositor only. The GPU compositor is not written.
- Capture takes one frame stream and records latency fields in the take: `cameraLatencySeconds`, `deliveryDelaySeconds`, `audioLatencySeconds`, `slipSeconds`. Offset is `audioLatency - cameraLatency + slip`.
- Embedded sync locks in about 0.16 to 0.22 s, not 0.1 s.
- The fingerprint source resets its votes when the host audio jumps (seek or loop wrap), and accepts a best match at least 1.25x the runner up, because the correlation check verifies it. Repetitive music otherwise stayed ambiguous.
- Measured numbers: `docs/engine-benchmarks.md`. Headless harness: `plugin/tests/Harness.cpp`.

### Later additions

- take.mov: written after a take stops (FFmpeg on Linux, AVAssetWriter on macOS, JPEG passthrough plus PCM audio). The take folder stays the source of truth.
- Decoders: JPEG frames (default), FFmpeg (Linux, optional), AVFoundation (macOS). The macOS reader stamps a black placeholder before the range start, which the decoder skips.
- Compositors: CPU reference and an OpenGL path. The GPU path matches the CPU picture but is only measured on software renderers, so CPU stays the default.
- Embedded sync runs at 800 baud (48 bit frame, about 60 ms). Fingerprint matching resets on host audio discontinuities.
- The engine is not yet wired into the plugin processor and editor. The plugin still uses the earlier camera code.
- Every finished take.mov is also copied to `<takes folder>/Takes/Slot N Take M.mov`, one flat folder for Finder. Take numbers count per session, so a repeated name gets a " (2)" suffix instead of overwriting.
- Record when: DAW plays and Armed record on every play while armed, so a take there is one shot. When it ends the hub switches Record arm off, and nothing records until arm is switched on again. Before this, pressing play again recorded a new take, which showed the live camera instead of the take and deleted the take still in review. DAW records keeps recording on every DAW record pass, as the DAW's own record button is the gate.
- A takes folder chosen while a take or export runs is kept as pending and applied as soon as they end. Before, the UI showed the new folder while takes kept going to the old one.
- Saved state carries `stateVersion` (now 2). A state without it that has Record when on DAW records is loaded as DAW plays, once. Saving again keeps whatever is chosen.
- `GRIDDY_TAKES_FOLDER` overrides the default takes folder. The host smoke test uses it so it never deletes real takes in `~/Movies/Griddy`.
