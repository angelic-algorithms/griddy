# Recording from Griddy: the flow, and what each tile shows

This is the reference for how Griddy's REC and Arm buttons work with Live, and what the picture and the tile
overlay show in every case. Change it before changing the behaviour.

## Who owns what

* **Live owns arming and recording.** With the Live script running, Griddy never decides to record on its own:
  its Record arm parameter is set by the script to "this track is armed in Live and Live records".
* **Griddy's buttons ask Live.** REC and Arm write a request to `live-requests.json` next to the arrangement file.
  The script reads it every display tick (about 0.1 s) and does what Live's own button would do. A button press
  never changes a parameter, so nothing a press does can be echoed back as another press, or written as automation.
* **Without the script** (Control Surface set to None, or an older Live), REC toggles Griddy's Record arm as it did
  before the script existed: armed and playing records one take. The Arm button is hidden, since only the script can
  arm a track.

Griddy knows the script runs when its arrangement file is fresh (rewritten in the last 3 s).

## The buttons

| State of this track | Arm button | REC button | Pressing REC |
|---|---|---|---|
| Not armed in Live | "Arm" | dimmed | says "Arm the track first", changes nothing |
| Armed, Live not recording | "Armed", lit | "● Rec" | Live's record on, and play if stopped |
| Armed, Live recording | "Armed", lit | "● Stop rec", pulsing | Live's record off; playback goes on, as with Live's own button |
| No Live script | hidden | "● Rec", as before | toggles Griddy's arm, as before |

Pressing Arm arms or disarms only this track, as Cmd clicking Live's Arm button would. While a request waits for
Live, the button shows the state asked for; if Live has not followed after 1.5 s it goes back and says so.

## What the tile shows

Live's auto monitoring is the model: an armed track you hear through is the input, a track playing its clips is
the clips. So the picture and the overlay both follow these rules, in order:

1. Recording: the live camera, red ring, "● REC".
2. A take in review: that take, "REVIEW".
3. Armed in Live and Live stopped (or Griddy armed by Live recording): the live camera, "ARMED · LIVE".
4. A kept take: that take, "✓ APPROVED", following the arrangement while Live plays.
5. Otherwise: the live camera if the slot has no take, under the stripes unless its track is armed.

A camera that is off keeps the stripes. The master Griddy has no REC or Arm, and its tiles follow the same rules.

## Every case, and what you should see

| Case | Before the press | During | After |
|---|---|---|---|
| Arm a track with no takes | stripes | camera, no stripes | |
| Arm a track with a kept take, Live stopped | kept take | camera | |
| Arm a track with a kept take, Live playing | kept take playing | kept take playing | camera when Live stops |
| REC, armed, Live stopped | camera | camera, "● REC" from the first recorded frame | review take when Live stops |
| REC, armed, Live playing (punch in) | kept take playing | camera from the press | review take when recording stops |
| Live's own record button | as for REC | as for REC | as for REC |
| Stop rec from Griddy, Live keeps playing | camera | | review take, playing in time |
| Stop from Live's stop button | camera | | review take |
| Disarm from Griddy or Live, Live stopped | camera | | kept take, or stripes when there is none |
| REC on an unarmed track | unchanged | "Arm the track first" | unchanged |
| Camera off | stripes | stripes, the recording still runs | |
| Two Griddy tracks, one armed | the other track's tile never changes | | |
| Master Griddy | never records; shows every slot by the rules above | | |

At every step the picture changes once when recording starts and once when it stops.

## The flashing seen in Live on 17 November, and why

Griddy's log showed takes 1 to 36 started and ended about every 0.1 s after one press of REC, and Live's log
alternated "REC on" and "REC off" at the same rate. Each cycle the tile went camera, then the tiny take just
finished (or black, where the arrangement had no clip yet), then camera again.

The cause was one parameter with three writers:

1. The REC button wrote Record arm.
2. The script wrote Record arm to match Live, and read any change it had not made as a press of REC, which it passed
   on to Live's record button.
3. Griddy wrote Record arm off after every take ("one take per arm"), which the script read as a press of REC off.

Any change arriving out of order (Griddy's disarm landing after the script's arm, or a value coming back late from
the plugin) turned into a press, which changed Live's record, which changed Record arm again. Fixed by:

* presses going to the script through `live-requests.json`, never through a parameter;
* Griddy not disarming itself after a take while the script runs, since Live ends the recording itself;
* the picture and the overlay using the same rule for the live camera, so arming no longer shows "LIVE" over a
  take, and starting to record no longer jumps from that take to the camera.

Two smaller ones:

* The "Armed. Press play in Live" hint showed for one status update between Live's record and play, moving the
  timeline down and up. It now shows only after it has held for half a second.
* With the track armed in Live but not recording, black gaps showed under a "LIVE" overlay. The overlay now says
  "LIVE" only where the picture is the camera; while Live plays, an armed track shows its takes and gaps.
