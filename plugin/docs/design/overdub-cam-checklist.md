# Overdub Cam checklist

One line per item of the brief. Status is one of: todo, done, done with a difference (what), gap (why).
The mockup is overdub-cam.html in this folder. Screenshot pairs are in compare/.

## Rules
- R1 Mockup is the source of truth for visuals and words: todo
- R2 Every control drives the real engine through bridge.ts: todo
- R3 No invented visuals; new things reuse paper panel, Label, Btn, Sticker, Slider, select: todo
- R4 This checklist exists before code and is kept current: done

## 1. Foundations
- 1.1 Mockup style block ported to styles.css, old styles removed: todo
- 1.2 Colour tokens: todo
- 1.3 Slot colours in order everywhere a slot is shown: todo
- 1.4 Fonts bundled offline with licences, no CDN, plain CSS or Tailwind chosen: todo
- 1.5 Btn, Sticker, Label, Slider, TplThumb, Meter, Wave components: todo
- 1.6 Window 1280 by 800 default, minimum 900 by 620, holds at 2400 by 1600: todo

## 2. Header
- 2.0 Header padding and gaps, wraps when narrow: todo
- 2.1 Logo and OVERDUB CAM sticker: todo
- 2.2 Transport group, order, colours, sizes: todo
- 2.3 Counter and BPM chip from the host: todo
- 2.4 Export MP4 button opens the sheet: todo

## 3. Canvas
- 3.1 Canvas fits the aspect ratio like the mockup: todo
- 3.2 Frame colour, padding, gap, border, shadow, radii, tape pieces: todo
- 3.3 Real preview with HTML overlays placed from engine cell rectangles: todo
- 3.4 Status chips under the canvas: todo

## 4. Tiles
- 4.1 Densities full, compact, tiny: todo
- 4.2 Empty slot: todo
- 4.3 Live camera with viewfinder and LOW LATENCY PREVIEW: todo
- 4.4 Take showing with scanlines: todo
- 4.5 Active ring armed and recording: todo
- 4.6 Top left number sticker and track name: todo
- 4.7 Top right badges, shortened words, previous take muted, slip: todo
- 4.8 Waiting overlay with countdown: todo
- 4.9 Bottom strip: waveform, recording bar, Keep, Retake, slip group: todo
- 4.10 Tile selection by click, Enter, Space; aria label: todo

## 5. Takes timeline
- 5.0 Paper panel, rotation, padding: todo
- 5.1 Label column: todo
- 5.2 Ruler from the host time signature, length rule: todo
- 5.3 Loop marker from the host: todo
- 5.4 Rows, labels, selection: todo
- 5.5 Take blocks placed by startPpq and slip, styles, opacity, tooltip: todo
- 5.6 Recording block: todo
- 5.7 Playhead: todo
- 5.8 Ruler click explained, no seeking: todo

## 6. Inspector
- 6.0 Column width, scroll, paper, rotation, stacks below 1024 px: todo
- 6.1 Tabs Slot, Look, Audio, Setup with roles: todo

### 6a. Slot tab
- 6a.1 Header: todo
- 6a.2 Message wording: todo
- 6a.3 Assigned track (slot picker or read only owner): todo
- 6a.4 Camera picker: todo
- 6a.5 Keep take and Retake: todo
- 6a.6 Trim and slip box: todo
- 6a.7 Clear this slot is Reset slot with confirm: todo
- 6a.8 Take list and sync chip restyled: todo

### 6b. Look tab
- 6b.1 Thirteen templates with thumbnails, engine geometry, confirm when hiding takes: todo
- 6b.2 Canvas shape cards with real sizes: todo
- 6b.3 Thickness, corner radius, frame colour: todo
- 6b.4 Who sits where: todo
- 6b.5 Track instance editing rule: todo

### 6c. Audio tab
- 6c.1 Audio mode buttons: todo
- 6c.2 Text and meters per slot: todo

### 6d. Setup tab
- 6d.1 Master, Slots count, Record when, Session, Takes folder, Every slot: todo

## 7. Export sheet
- 7.0 Dialog look, Close, Escape, backdrop: todo
- 7.1 Codec, audio and bitrate choices only as supported: todo
- 7.2 Summary line: todo
- 7.3 Real export states: todo

## 8. Toasts, keys, motion, accessibility
- 8.1 Toast look and sentences: todo
- 8.2 Keys R and K, Space left to Live: todo
- 8.3 Focus outlines and reduced motion: todo

## Mapping
- M1 Instance types, claim on empty tile, toast for another track's slot: todo
- M2 Transport buttons show host state; Loop and Count in from the host or hidden; Rec is arm: todo
- M3 GET READY and countdown: todo
- M4 Previous take muted: todo
- M5 No mixer labels or sends: todo
- M6 Status chips only with measured values: todo
- M7 Track names from the host: todo
- M8 Selected slot in the master lives in the UI: todo

## Engine work
- E1 Cell rectangles in the status: todo
- E2 Thirteen templates in CollageLayout, old layouts mapped, tests: todo
- E3 Corner radius in layout, compositor, export, test: todo
- E4 startPpq, bpm, time signature per take, record start while recording: todo
- E5 Waveform peaks, cached: todo
- E6 Levels per slot: todo
- E7 Track names: todo

## Keep everything that exists today (where each now lives)
- Arm: todo
- Master: todo
- Slot picker: todo
- Camera pads and test patterns: todo
- Cameras blocked banner: todo
- Record hint: todo
- Sync chip with every status and the warp warning: todo
- Record when: todo
- Slots count: todo
- Layout shape, template, thickness, colour, cell swap: todo
- Export: todo
- Take list with Keep, Retake, Restore, slip: todo
- Reset slot with confirm: todo
- Session panel with New session and Show folder: todo
- Takes folder with Choose and the pending move note: todo
- Show takes: todo
- Last error: todo
- Preview one second timeout: todo

## Verification
- V1 Mock shows every state with a dev state picker: todo
- V2 Screenshot pairs at 1280 by 800 and 900 by 620: todo
- V3 VST3 build, GriddyTests, Harness, smoke test, UI typecheck and build: todo
- V4 Install if Live is closed: todo
- V5 What to click in Live, what is unverified: todo
