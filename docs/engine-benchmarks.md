# Griddy engine benchmarks

Measured with `plugin/tests/Harness.cpp` (`GriddyHarness`), Linux, 2 vCPU cloud box, Release build, no GPU, no real camera, no DAW.
Everything here is the CPU reference path with JPEG take files. Numbers on a Mac with hardware decode and a GPU compositor will differ; they are not measured.

Run it: `GriddyHarness` (all scenarios and the bench), `GriddyHarness soak --soak-seconds 600 [--soak-720] [--soak-slots N]`.
Pictures are coded (take label and frame number as pixel squares) so the harness reads what the engine shows and compares it with where the audio really is.

## Sync scenarios (all pass)

| Scenario | Result |
|---|---|
| Play, pause, resume | start lock 25 ms; error p99 19 to 32 ms; paused picture holds, status stopped |
| Loop wrap x3 | max error under 11 ms after each wrap |
| Seek (in, out of, back into clip) | max error 16 to 20 ms; outside the clip holds a frame and reports it |
| Tempo change | unwarped clip unaffected |
| Take 2 over take 1 | take 2 video plays; an old copy of take 1 elsewhere plays take 1 video, recognised 0.69 s after its audio starts; take 1 archived on disk |
| Retake | discarded take deleted, previous take stays active |
| One clip duplicated 8 times | each repeat starts within 25 ms, 0.2 percent of samples wrong |
| Trimmed and moved clip | locks in 0.22 s, error p99 19 ms, last frame held after the end |
| Split and moved clip | moved half locks in 0.32 s |
| Clip starting with silence | host position prior is right during silence; moved copy locks 0.02 s after sound begins |
| Two takes, identical audio | picture never flips, newest take shown |
| Warped clip (1.12x) | reported as lost sync after 1.6 s, never shown as locked |
| Embedded sync (Griddy clip trimmed, moved, duplicated) | locks in 0.16 to 0.22 s, source reported as embedded 100 percent of the time |

Missed target: the brief asked for about 100 ms lock for embedded sync. Measured 155 to 223 ms (the 400 baud frame needs its full length before the CRC is checked). Fingerprint lock is 0.2 to 0.7 s.

## Soak: 9 slots looping

| Run | Late render frames | Skipped video frames | Drift p99 (worst slot) / max | Memory growth | CPU |
|---|---|---|---|---|---|
| 1080p canvas, 180 s | 0 of 5595 | 0 | 50 / 60 ms | 0.00 percent | 105 percent of one core |
| 1080p canvas, 600 s (first run, labels bug in harness) | 18 of 18203 | 15 | 52 / 65 ms | 0.45 percent | 106 percent |
| 720p canvas, 600 s | 1 of 18200 | 10 | 31 / 130 ms | 0.66 percent | 73 percent |

RSS is flat at about 197 MB for 9 slots. The 720p run showed every slot on its own take for the whole run.

Not met: "zero dropped frames over 10 minutes with 9 slots" and "video within 1 frame of audio always". Both 10 minute runs had a handful of late or skipped frames and drift spikes up to 130 ms. The box has 2 cores and is running the harness, fake audio, 9 slot threads, the sync worker and the render thread together, so I read these as CPU starvation, not an engine defect, but I have not proven that. Re-run on target hardware. The 30 minute memory run was not done; growth was under 1 percent at 10 minutes.

## Microbenchmarks (single thread)

| Item | Cost |
|---|---|
| CPU composite 1280x720, 1 / 4 / 9 slots | 6.9 / 6.8 / 7.2 ms per frame |
| CPU composite 1920x1080, 1 / 4 / 9 slots | 19.0 / 18.6 / 20.7 ms |
| CPU composite 1080x1080, 9 slots | 9.2 ms |
| CPU composite 1080x1920, 9 slots | 16.5 ms |
| JPEG decode 640x360 / 1280x720 / 1920x1080 | 1.0 / 4.7 / 10.5 ms |
| Fingerprint index build | 1.4 ms per second of audio, 2.1 MB per minute |
| Fingerprint live analysis and matching | 10.3 ms per second of audio per slot |
| Embedded decoder | 7.4 ms per second of audio per slot |

The 1080p CPU composite is about two thirds of a 30 fps frame budget on this machine. That is why the GPU compositor matters; it is not written. JPEG sizes in the harness are unrealistically small (flat synthetic pictures), so disk and bandwidth numbers for real camera footage are not measured. Live fingerprint analysis for 9 slots is about 93 ms of CPU per second, which is high for an audio plugin and a candidate for optimisation.

## Cannot be verified here

- macOS code (`.mm`, AVFoundation camera, AU), Windows, any real DAW including Ableton Live 12.
- VideoToolbox / Media Foundation decode, GPU compositing, `.mov` take files (takes are JPEG folders, see engine-design.md).
- Audibility of the 18 kHz embedded carrier, and fingerprint hit rates on dense real mixes or through Live's own processing.
- Real camera latency measurement and the audio to video offset on real hardware.
