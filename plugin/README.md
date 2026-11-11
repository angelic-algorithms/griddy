# Griddy (plugin edition)

Griddy is an overdub video recorder that lives inside your DAW, in the style of the Acapella app: a grid of slots, one per track, where every slot holds the video of a take you performed. This branch is being rebuilt around that idea (see `docs/engine-design.md` once it lands). The code you see here is the transitional state after the cleanup commit: the old composite recorder, Cut and Spotlight layouts and switch rate are gone, and the take recording and playback engine is being added next.

One C++ codebase (JUCE 9) builds **VST3, AU and a Standalone app**, with the whole interface written in **React and TypeScript** and shown in a WebView. CLAP was removed to keep the matrix small.

Status: it builds and passes its tests on Linux with test pattern "cameras". The macOS camera code is written but has not been compiled yet; the GitHub Actions workflow (`.github/workflows/plugin.yml`) compiles and validates it on a Mac runner on every push.

## How it works

```
 DAW audio thread          capture threads             engine thread (30 fps)                message thread
 ────────────────          ───────────────             ──────────────────────                ──────────────
 processBlock()            AVFoundation camera ─┐      read transport                        WebView (React UI)
   read play head          AVFoundation camera ─┼─▶    BeatClock: where are we musically?    ◀── events: transport,
   write TransportState    test pattern ────────┘      CollageLayout: grid cells                   preview, status
   (lock free) ───────────────────────────────────▶    Compositor: draw 1280x720 frame       ──▶ native functions:
                           FrameStore (newest                                                     setSlotSource, ...
                           frame per camera)            Preview: JPEG for the UI ──────────▶ /preview.jpg
```

The ideas, from first principles:

1. **The audio thread only reports the clock.** A plugin can't own the DAW's transport, but every audio block the host tells us the tempo, the song position in quarter notes (PPQ), the time signature, and whether it's playing and recording. `processBlock` copies that into a `TransportState` using a sequence lock, which never blocks, so the audio thread can't be slowed down by video.
2. **Video runs on its own clock.** The `VideoEngine` thread ticks at 30 fps. Audio blocks arrive every few milliseconds, video frames every 33 ms, so `BeatClock` extrapolates from the last block at the current tempo. When the DAW stops, it keeps free running so the preview stays alive.
3. **Layout is a pure function.** `computeLayout(slots)` decides the grid cells; one slot fills the whole frame (1x1).
4. **One compositor, one truth.** The C++ compositor draws the grid once per frame and the UI preview is a downscaled JPEG of that image.
5. **Cameras are mailboxes, not queues.** Each camera drops its newest frame into the `FrameStore`, replacing the old one. If the machine is busy we skip frames rather than fall behind, so latency stays flat.
6. **The UI is a web app talking to C++.** JUCE's WebView bridge gives three channels: parameter relays (slots, arm, record mode stay in sync with DAW automation both ways), native functions (async calls like `setSlotSource`), and events pushed from C++ (`transport`, `preview`, `status`). The preview image is served by a resource provider at `/preview.jpg` instead of being base64 encoded into JSON.

### Parameters (automatable in the DAW)

| Parameter | Values |
| --- | --- |
| Cameras (slots) | 1 to 9 |
| Record arm | on, off |
| Record when | DAW records, DAW plays, Armed |

Which camera sits in which slot and the takes folder are saved with your DAW project.

## Formats and hosts

| Format | Hosts | Notes |
| --- | --- | --- |
| VST3 | Ableton Live, Cubase, Studio One, Reaper, Bitwig, FL Studio | Main target |
| AU | Logic Pro, GarageBand, Live on Mac | macOS only |
| Standalone | none needed | Its own app with its own camera permission; the fallback when a DAW blocks cameras |
| AAX (Pro Tools) | Pro Tools | Not built. Needs Avid's AAX SDK, an Avid developer account and PACE signing |

## The real limits (read this before testing in Ableton)

**Camera permission belongs to the DAW, not the plugin.** A plugin is a library inside the DAW's process, so macOS checks the DAW's `Info.plist` and entitlements. If the DAW never declared `NSCameraUsageDescription`, or it uses the hardened runtime without the camera entitlement, macOS **kills the whole DAW** the moment anything in it opens a camera ([this happened to the SPARTA plugins in Reaper](https://github.com/leomccormack/SPARTA/issues/82)). Griddy checks this before touching AVFoundation (`source/capture/HostPermissions_mac.mm`) and, if the host can't use cameras, shows a banner and offers test patterns only. It never crashes your session.

**Ableton Live 12 Suite passes.** Its `Info.plist` declares `NSCameraUsageDescription` ("A plug-in or Max for Live device requires access to the camera.") and its signature carries `com.apple.security.device.camera`, so Griddy can open cameras inside Live. The first time it does, macOS asks once whether Live may use the camera. To check another DAW:

```sh
plutil -extract NSCameraUsageDescription raw "/Applications/<DAW>.app/Contents/Info.plist"
codesign -d --entitlements - "/Applications/<DAW>.app" 2>/dev/null | grep -i camera
```

If the first prints text and the second prints `com.apple.security.device.camera` (or nothing, when the DAW isn't hardened), cameras work inside it. If not, the fallback is in "Next steps": a small **Griddy Capture** helper app owns the cameras and shares frames with the plugin.

Some hosts run plugins in a separate helper process. When that happens the check looks at that helper instead, and the banner says what it found.

**Other limits**

* Camera capture is macOS only for now. Windows (Media Foundation) and Linux (V4L2) get test patterns until those backends are written.
* Compositing is on the CPU (JUCE's software renderer). Fine for 4 cameras at 720p; the GPU path (Metal or Core Image) is on the list for 1080p and more cameras.
* The AU and VST3 must be code signed and notarized before you share them with anyone else, or macOS Gatekeeper blocks them. Building for yourself doesn't need that.
* Licensing: JUCE is AGPLv3 or the JUCE licence. The free Starter tier covers up to $20K of revenue or funding in the last 12 months; past that you need Indie or Pro, or release the source under AGPL. Check the [JUCE 9 licence](https://juce.com/legal/juce-9-licence/) before selling it.

## Build it on your Mac

You need Xcode (or its command line tools), CMake 3.22+, Ninja and Node 20+.

```sh
brew install cmake ninja node
git clone https://github.com/angelic-algorithms/griddy.git && cd griddy
git checkout claude/griddy-vst-plugin && cd plugin

# 1. The UI (React, bundled into three files the plugin embeds)
cd ui && npm ci && npm run build && cd ..

# 2. Configure and build. The first run downloads JUCE 9.0.5.
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# 3. Tests
./build/GriddyTests_artefacts/Release/GriddyTests
./build/GriddyHostSmokeTest_artefacts/Release/GriddyHostSmokeTest build/Griddy_artefacts/Release/VST3/Griddy.vst3
```

Install for your DAWs:

```sh
cp -R build/Griddy_artefacts/Release/VST3/Griddy.vst3 ~/Library/Audio/Plug-Ins/VST3/
cp -R build/Griddy_artefacts/Release/AU/Griddy.component ~/Library/Audio/Plug-Ins/Components/
auval -v aufx Grdy Pkag   # Apple's AU check; should end with "AU VALIDATION SUCCEEDED"
```

Prefer Xcode? `cmake -S . -B xcode -G Xcode` and open `xcode/Griddy.xcodeproj`.

### First test

1. Run the Standalone app first (`build/Griddy_artefacts/Release/Standalone/Griddy.app`). It has its own camera permission, so macOS asks once. Pick your FaceTime camera, Continuity Camera (iPhone) and capture card in the slots and confirm the preview.
2. In Live, rescan plugins, drop Griddy on any audio track. If the banner says cameras are blocked, that's the permission limit above; the test patterns still show you sync and recording working.
3. Press play and watch the transport readout follow the DAW. Take recording returns with the engine milestone.

## UI mockups

The street style look is in `docs/mockups/4-street.png`. The interactive prototype is the reference for the new UI.

## Working on the UI

The UI is a normal React app in `ui/src`. `bridge.ts` is the only file that talks to C++; components use hooks from `hooks.ts`.

```sh
cd ui && npm run dev       # esbuild watch + server on http://localhost:5173
```

Open that address in a browser and the UI runs against a mock backend (`mock.ts`) that fakes a playing DAW, so you can style without rebuilding. To see changes inside the real plugin, configure once with `-DGRIDDY_UI_DEV_SERVER=ON`; the plugin then loads the dev server instead of its embedded copy, and reopening the plugin window picks up your edits.

## What's tested

On Linux in this repo's first pass:

* `GriddyTests`: layout math, beat clock (tempo extrapolation, stopped transport, 7/8, float edge cases), the lock free transport snapshot under a racing writer, the compositor's pixels, the frame store and test pattern threads.
* `GriddyHostSmokeTest`: a fake DAW loads the built VST3, checks the parameters, plays at 100 BPM from bar 3, confirms audio passes through untouched, opens and closes the WebView editor, and restores state into a new instance.
* The Standalone app renders the React UI with live test pattern previews.

Not yet compiled or run anywhere: `AVFoundationCapture.mm`, `HostPermissions_mac.mm`, the AU inside a real DAW. The macOS job in `.github/workflows/plugin.yml` compiles all of it, runs both test programs and runs `auval`.

## Next steps

1. **Mac bring up:** build, run the tests, try real cameras in the Standalone, then in Live. Fix whatever the first Mac compile finds in the two `.mm` files.
2. **Griddy Capture helper (only for DAWs that block cameras; Live doesn't):** a tiny menu bar app that owns the cameras and publishes frames through shared GPU memory (IOSurface, the same trick Syphon uses). The plugin swaps its `CaptureSource` for an IOSurface reader; everything else stays the same.
3. **Playback in sync:** load a take and play it back locked to the DAW's PPQ, which is what the browser version does today.
4. **GPU compositing:** Core Image or Metal, for 1080p output and 9 cameras.
5. **Per camera ISO recording:** also save each camera's raw feed, for re editing later.
6. **Audio in the file:** write the track's audio into the .mov so takes are self contained.
7. **Windows cameras:** Media Foundation backend behind the same `CaptureSource` interface.

## Project layout

```
CMakeLists.txt                  JUCE 9 targets, UI embedding, tests
source/
  PluginProcessor.*             parameters, transport capture, saved state
  PluginEditor.*                WebView, native functions, events, preview resource
  UiResources.*                 serves the embedded ui/dist files
  engine/
    TransportState.h            lock free host transport snapshot
    BeatClock.*                 musical time for the video thread
    CollageLayout.*             grid cells as a pure function
    Compositor.*                draws the collage frame
    FrameStore.*                newest frame per camera
    VideoEngine.*               the 30 fps loop and previews
  capture/
    CaptureSource.h             camera interface and device listing
    TestPatternSource.*         fake camera for tests and blocked hosts
    AVFoundationCapture.mm      webcams, Continuity Camera, capture cards
    HostPermissions_mac.mm      the "will macOS kill this DAW?" check
ui/                             React + TypeScript, built with esbuild
tests/                          unit tests and the fake DAW smoke test
```
