# Griddy

A Max for Live device that turns an Ableton Live session into a grid of performance videos. Every track you record on gets a tile, and every tile plays the video of you performing that part, locked to Live's playhead.

* Record in Arrangement view and Griddy records your camera at the same time.
* Each video is tied to the audio clip it was recorded with, so moving, trimming or duplicating the clip keeps the video with it.
* The grid builds itself: 1 tile, then 2 side by side, then 2x2, 3x2 and so on.
* Play, stop, loop and scrubbing all stay in sync.
* Cameras: Cam Link and other capture cards, HDMI dongles, webcams, and the iPhone through Continuity Camera.

## Requirements

* Ableton Live 12 Suite (or Standard with the Max for Live add on)
* macOS (paths are currently Mac only)

## Where this repo lives

Live loads Griddy straight from this folder, so the repo is cloned into the User Library:

```
~/Music/Ableton/User Library/Presets/Audio Effects/Max Audio Effect/Griddy
```

`Griddy.amxd` and `griddy.js` must stay next to each other at the top of the repo.

## Everyday workflow

1. Pull the latest changes (VS Code Source Control, or `git pull`).
2. In Live, click **Reload Engine** on the Griddy device. This reloads `griddy.js` without reopening the set.
3. If `Griddy.amxd` itself changed, reopen the set instead.

## Files

| File | What it is |
|---|---|
| `griddy.js` | The engine. All the logic: reading the set, sync, recording, linking takes, the grid. Runs in Max's built in JavaScript engine (older JavaScript: use `var`, no arrow functions or classes). |
| `Griddy.amxd` | The Max patch Live loads: camera, recorder, frame clock, buttons. Edit it in Max (edit button on the device) and save with Cmd+S. This file is the source of truth for the patch. |
| `griddy_trash.js` | Small Node for Max helper that moves videos of deleted tracks to the Trash. |
| `griddy_title.png` | The "Griddy" title on the device panel. Regenerate with `tools/make_title.swift` (font: Sedgwick Ave Display from Google Fonts). |
| `docs/how-it-works.md` | Flowcharts of how the code works. |

## Colors

Click **Colors** on the device to open the swatches. **Background** colors the grid window and the device panel; **Accent** colors the toggles and the Colors button. Click a swatch or type a hex code like `#C2CBD4` and press Return. Your choice is saved in `griddy_data.json` and comes back the next time you open a set.

## Editing

* **Logic:** open this folder in VS Code and edit `griddy.js`. Save, then click Reload Engine in Live.
* **Patch and styling:** click the edit button on the device in Live to open it in Max. Cmd+E toggles edit mode; the presentation view is what shows in Live. Save with Cmd+S.
* Auto reload is off on purpose (`autowatch = 0`). Reloading by itself used to leave an extra grid window behind. Use the Reload Engine button.

## Where Griddy keeps its data

* `~/Music/Ableton/Griddy Takes/griddy_data.json` holds which video belongs to which clip, your camera, and offsets. It is keyed by each clip's audio file, so one file serves every set.
* Take videos are saved into your chosen takes folder, otherwise the project folder, otherwise `~/Music/Ableton/Griddy Takes/`.
* The home folder is set in `griddy.js` (`HOME`). Change it before using Griddy on another computer.

## What shows in the grid

* A track's tile hides while the track is muted, silenced by another track's solo, or has its volume fader all the way down. It comes back when you can hear the track again, and the other tiles resize to fill the space.
* Deleting a track removes its tile right away. Its takes stay linked until you next start recording, so Cmd+Z brings everything back. When recording starts, Griddy forgets the deleted track's takes and moves their videos to the Trash (Put Back works). Clips you moved or copied to other tracks keep their videos. The first time, macOS asks to let Live control Finder; click OK.

## Known issues

* Loop recording links the video to only one of the loop passes. Record one pass at a time for now.
* Tiles are black while the transport is stopped; they show once you press play.
* Continuity Camera is not officially enabled in Live or Max, so the iPhone may not send video on some setups. Camo or OBS Virtual Camera work as a fallback.
* Consolidating a clip creates new audio, so its video link is lost.
