# Overdub Cam checklist

One line per item of the brief. Status is one of: todo, done, done with a difference (what), gap (why).
The mockup is overdub-cam.html in this folder. Screenshot pairs are in compare/.

## Rules
- R1 Mockup is the source of truth for visuals and words: done with a difference (see the differences listed under each item)
- R2 Every control drives the real engine through bridge.ts: done: every control calls bridge.ts; controls the engine cannot do are disabled with Not built yet
- R3 No invented visuals; new things reuse paper panel, Label, Btn, Sticker, Slider, select: done
- R4 This checklist exists before code and is kept current: done

Also changed: a new set starts like the mockup, an orange frame 6 thick with corners of 14. Saved sets keep their own look.

## 1. Foundations
- 1.1 Mockup style block ported to styles.css, old styles removed: done: the mockup's style block is verbatim in styles.css, with plain classes in place of its Tailwind ones
- 1.2 Colour tokens: done
- 1.3 Slot colours in order everywhere a slot is shown: done
- 1.4 Fonts bundled offline with licences, no CDN, plain CSS or Tailwind chosen: done: npm @fontsource packages 5.3.0 inlined into app.css as data urls, licences in ui/src/fonts; Permanent Marker is Apache 2.0, the other two are OFL
- 1.5 Btn, Sticker, Label, Slider, TplThumb, Meter, Wave components: done (kit.tsx)
- 1.6 Window 1280 by 800 default, minimum 900 by 620, holds at 2400 by 1600: done in PluginEditor.cpp; the 2400 by 1600 maximum is the resize limit, not tried in Live

## 2. Header
- 2.0 Header padding and gaps, wraps when narrow: done
- 2.1 Logo and OVERDUB CAM sticker: done
- 2.2 Transport group, order, colours, sizes: done with a difference: Stop and Play show Live's state and are disabled (Use Live's transport); Rec arms this track for one take (the next play records it) and is hidden in the master; Loop shows only when Live reports loop points; Count in is hidden because JUCE does not report it
- 2.3 Counter and BPM chip from the host: done
- 2.4 Export MP4 button opens the sheet: done

## 3. Canvas
- 3.1 Canvas fits the aspect ratio like the mockup: done
- 3.2 Frame colour, padding, gap, border, shadow, radii, tape pieces: done with a difference: thickness and radius are measured on a 1280 wide canvas, so the export matches the screen at every canvas size
- 3.3 Real preview with HTML overlays placed from engine cell rectangles: done
- 3.4 Status chips under the canvas: done with a difference: only the latency chip (a take's measured latencyMs) and the record hint; the mockup's INPUT and PLAYBACK chip has no engine state behind it, so it is left out

## 4. Tiles
- 4.1 Densities full, compact, tiny: done
- 4.2 Empty slot: done with a difference: TAP TO CLAIM only on slots no track holds, and never in the master
- 4.3 Live camera with viewfinder and LOW LATENCY PREVIEW: done
- 4.4 Take showing with scanlines: done
- 4.5 Active ring armed and recording: done
- 4.6 Top left number sticker and track name: done
- 4.7 Top right badges, shortened words, previous take muted, slip: done
- 4.8 Waiting overlay with countdown: done with a difference: GET READY with a beat countdown shows only while armed and Live reports a position before the song start (its count in). Whether Live reports that is unverified; armed plus play otherwise records at once
- 4.9 Bottom strip: waveform, recording bar, Keep, Retake, slip group: done with a difference: without a host loop the recording bar fills through each bar
- 4.10 Tile selection by click, Enter, Space; aria label: done

## 5. Takes timeline
- 5.0 Paper panel, rotation, padding: done
- 5.1 Label column: done
- 5.2 Ruler from the host time signature, length rule: done
- 5.3 Loop marker from the host: done
- 5.4 Rows, labels, selection: done
- 5.5 Take blocks placed by startPpq and slip, styles, opacity, tooltip: done
- 5.6 Recording block: done
- 5.7 Playhead: done
- 5.8 Ruler click explained, no seeking: done: no seeking, the ruler says Move the playhead in Live

## 6. Inspector
- 6.0 Column width, scroll, paper, rotation, stacks below 1024 px: done
- 6.1 Tabs Slot, Look, Audio, Setup with roles: done with a difference: a fourth Setup tab (gold) holds what the mockup has no place for

### 6a. Slot tab
- 6a.1 Header: done
- 6a.2 Message wording: done
- 6a.3 Assigned track (slot picker or read only owner): done: a track picks its slot; a held slot says it will swap; the master sees the owner read only
- 6a.4 Camera picker: done
- 6a.5 Keep take and Retake: done
- 6a.6 Trim and slip box: done
- 6a.7 Clear this slot is Reset slot with confirm: done
- 6a.8 Take list and sync chip restyled: done

### 6b. Look tab
- 6b.1 Thirteen templates with thumbnails, engine geometry, confirm when hiding takes: done
- 6b.2 Canvas shape cards with real sizes: done
- 6b.3 Thickness, corner radius, frame colour: done
- 6b.4 Who sits where: done
- 6b.5 Track instance editing rule: done: any instance can change the look, as today

### 6c. Audio tab
- 6c.1 Audio mode buttons: done: DAW routed is on; Internal mixer is disabled, Not built yet
- 6c.2 Text and meters per slot: done with a difference: the text says the meters read where Griddy sits on the track, before the track's fader, because that is what they measure

### 6d. Setup tab
- 6d.1 Master, Slots count, Record when, Session, Takes folder, Every slot: done with a difference: the Slots count stepper is gone, the template sets the count; Record when is gone, see Recording below

## 7. Export sheet
- 7.0 Dialog look, Close, Escape, backdrop: done
- 7.1 Codec, audio and bitrate choices only as supported: done with a difference: H.264 and AAC only; HEVC and PCM are disabled; no bitrate slider because the encoder has no bitrate setting
- 7.2 Summary line: done
- 7.3 Real export states: done

## 8. Toasts, keys, motion, accessibility
- 8.1 Toast look and sentences: done
- 8.2 Keys R and K, Space left to Live: done
- 8.3 Focus outlines and reduced motion: done

## Mapping
- M1 Instance types, claim on empty tile, toast for another track's slot: done
- M2 Transport buttons show host state; Loop and Count in from the host or hidden; Rec is arm: done; Rec arms one take
- M3 GET READY and countdown: done, see 4.8
- M4 Previous take muted: done
- M5 No mixer labels or sends: done
- M6 Status chips only with measured values: done
- M7 Track names from the host: done (from updateTrackProperties; not yet seen in Live)
- M8 Selected slot in the master lives in the UI: done

## Engine work
- E1 Cell rectangles in the status: done
- E2 Thirteen templates in CollageLayout, old layouts mapped, tests: done
- E3 Corner radius in layout, compositor, export, test: done
- E4 startPpq, bpm, time signature per take, record start while recording: done
- E5 Waveform peaks, cached: done
- E6 Levels per slot: done with a difference: the peak decays on the audio side
- E7 Track names: done

## Keep everything that exists today (where each now lives)
- Arm: header Rec and the R key
- Master: Setup tab
- Slot picker: Slot tab, Assigned track
- Camera pads: Slot tab, Camera. Clicking the camera that is on turns it off. No test patterns.
- Cameras blocked banner: under the header
- Record hint: chip under the grid
- Sync chip with every status and the warp warning: Slot tab take list, Setup tab Every slot
- Record when: removed; arm plus play records one take (see Recording below)
- Slots count: replaced by the template (6d.1)
- Layout shape, template, thickness, colour, cell swap: Look tab
- Export: Export MP4 sheet
- Take list with Keep, Retake, Restore, slip: Slot tab
- Reset slot with confirm: Slot tab Clear this slot, Setup tab Every slot
- Session panel with New session and Show folder: Setup tab
- Takes folder with Choose and the pending move note: Setup tab
- Show takes: Setup tab
- Last error: red line under the grid
- Preview one second timeout: kept in Preview.tsx

## Verification
- V1 Mock shows every state with a dev state picker: done: npm run dev, then the Mock state picker bottom left
- V2 Screenshot pairs at 1280 by 800 and 900 by 620: done: compare/ has mockup and Griddy at both sizes, plus recording, master 3x3, export and Look
- V3 VST3 build, GriddyTests, Harness, smoke test, UI typecheck and build: done; every Harness scenario passes except seek (2 checks), which fails the same way on 83b9bd9, before this work
- V4 Install if Live is closed: todo
- V5 What to click in Live, what is unverified: todo

## Recording (after the Overdub Cam work)
- Shipped: version B, one rule. Armed plus play in Live records one take, and the take switches arm off, so the next play plays it back.
- Evidence: on 13 November a Live test with Arrangement Record, punch in and Session Record logged every transport change as "DAW not recording", and the check for a flag shorter than a control tick never fired. The earlier 1640 log lines had the flag 0 times. A plugin cannot see a Live track's arm (JUCE TrackProperties: name and colour; VST3 channel context: no arm key).
- Hints: "Armed. Press play in Live to record slot N.", "Pick a camera for slot N in the Slot tab.", "Griddy's engine is not running yet."; none while recording or not armed.
- Toasts: "Slot N recorded. Keep it or retake it." then "Slot N disarmed. Press Rec to record again."
- Mock states: Armed, Live stopped; Armed, Live playing; Armed, Live recording; Count in; Disarmed after take. Screenshots: compare/record-*.jpg

## Comping
- Takes: a new take over a review take archives it; Retake still discards. The sync recognises the newest 20 takes or 30 minutes per slot; the Slot tab lists five, then Show all; past the cap "Too old to follow comps". Setup shows the session's disk use.
- Comp detection: aligned 100 ms correlation every 25 ms on the sync worker, 1.25 times for two windows, newest wins a tie; moved pieces clear the fingerprint votes; loop passes are candidates.
- Gaps: black after 150 ms of digital silence where no clip plays, back on 25 ms of sound; never for a silent take, an unowned slot, Live stopped, or an armed or recording slot. A take's end while playing is now black.
- What is playing: TAKE N or NO CLIP chip on tiles (with aria labels, not in the export), Playing in the Slot tab, the take number at the playhead in the timeline, a griddy.log line for every change.
- Follow lines: "Follows Live's position. Put Griddy on this track to follow edits and comps." in the master view and the Slot tab; "This take listened to TRACK NAME. It follows edits on that track."
- GRIDDY_NO_COMP switches comp detection and black gaps off (debug only).
- Mock states: Comp playing; No Griddy on this track. Screenshots (1280 by 800 viewport, saved at 800 by 500 like the others): compare/comp-*.jpg, compare/no-griddy-*.jpg
- Harness: comp_aligned, comp_crossfade, comp_loop_passes, comp_moved, comp_gap, comp_end, comp_silent_take, comp_unowned, comp_recording_load, comp_bench
