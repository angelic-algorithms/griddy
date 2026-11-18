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

Griddy knows the script runs when its arrangement file is fresh (rewritten in the last 3 s). The file also says
which Griddy tracks Live has armed and, while Live records, the beat each one's recording began at. Griddy reads it
every few milliseconds. The arm comes through the file as well as the "Track armed in Live" parameter, because Live
only shows a plugin the parameters it had when the set was saved: in a set saved before that parameter existed, the
script cannot set it.

The script also checks every display tick (about 0.1 s) that every Griddy's Record arm matches Live, so a
notification Live did not send, or one the script missed, cannot leave them out of step.

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

Arming a track turns its camera on, whether Live plays or not. The picture and the overlay both follow these
rules, in order:

1. Recording: the live camera, red ring, "● REC".
2. A take in review: that take, "REVIEW".
3. Armed in Live: the live camera, "ARMED · LIVE", also while Live plays.
4. A kept take: that take, "✓ APPROVED", following the arrangement while Live plays.
5. Otherwise: the live camera if the slot has no take, under the stripes.

A camera that is off keeps the stripes. The master Griddy has no REC or Arm, and its tiles follow the same rules.

## Every case, and what you should see

| Case | Before the press | During | After |
|---|---|---|---|
| Arm a track with no takes | stripes | camera, no stripes | |
| Arm a track with a kept take, Live stopped | kept take | camera | |
| Arm a track with a kept take, Live playing | kept take playing | camera | |
| REC, armed, Live stopped | camera | camera, "● REC" from the first recorded frame | review take when Live stops |
| REC, armed, Live playing (punch in) | camera | camera | review take when recording stops |
| Live's own record button | as for REC | as for REC | as for REC |
| Stop rec from Griddy, Live keeps playing | camera | | review take, playing in time |
| Stop from Live's stop button | camera | | review take |
| Disarm from Griddy or Live | camera | | kept take, or stripes when there is none |
| REC on an unarmed track | unchanged | "Arm the track first" | unchanged |
| Camera off | stripes | stripes, the recording still runs | |
| Two Griddy tracks, one armed | the other track's tile never changes | | |
| Master Griddy | never records; shows every slot by the rules above | | |

At every step the picture changes once when recording starts and once when it stops.

## Every way to record, and what should happen

Each Griddy take starts where Live's recording of that track starts and ends when Live's recording ends: one Live
recording, one Griddy take. Griddy hears that Live records a tenth of a second or so late, so while a track is armed
in Live, Griddy keeps its last second of sound and camera frames, and a take starts back at the beat the script
reports. When the script does not report one (an older script), the take starts when Griddy hears, as before.

| How | Live | Griddy |
|---|---|---|
| Live's Arrangement record, Live stopped, "Start Playback with Record" on | records from the playhead | a take from the same beat |
| The same, with that setting off | record lights; recording starts at play | armed, "Press play" hint; the take starts at play |
| Live's Arrangement record while playing (punch in) | records from the press | a take from the press, from the kept second |
| Punch In switch on | records from the punch in point | armed early; the take waits for the punch in point |
| Count in | counts in, then records from the playhead | the take waits through the count in |
| Arming a track while Live records | that track records from the arm | a take from the arm |
| Live's record off while playing (punch out) | stops recording, plays on | the take ends, review take shows |
| Live's Stop button | stops recording and playing | the take ends, review take shows |
| Griddy's REC, armed | as Live's record button | as Live's record button |
| Griddy's Stop rec | as Live's record off | as Live's record off |
| Start in Live, stop in Griddy, or the reverse | one recording | one take |
| Record again straight after, before or after Keep | a new recording | a new take; the one in review is replaced as before |
| Live's Session record | records a Session clip | a take, from when Griddy hears (Session clips are not in the arrangement file) |

## Filming video for a recording that is already there

For laying down a part one piece at a time: record the kick, then the snare, and so on, and film each one later while
Live plays it back. A film take becomes the picture of that recording, wherever its clips are used. It needs the Live
script.

**Choosing the recording.** Click an audio clip on a Griddy track in Live, on the main lane or a take lane. That
track's Griddy shows "Film for: <track>, bar N" (with "take lane" for a lane clip) and lights its Film button. The
Slot tab also lists every recording on the track, one row per recorded file, marked "video" when a take shows for it,
"filmed" when that take is a film take, and "no video" otherwise, each with its own Film button.

**Filming.** Pressing Film asks Live, through `live-requests.json`, to play from one bar before the clip and to stop
at the clip's end. If Live's record button is on, it is switched off first, and if the track is armed in Live, it is
disarmed, so Live plays the clip and records nothing. Griddy films from when Live plays until Live stops. Pressing
Film again (it reads "■ Stop film" while filming) stops Live early. If Live starts recording during filming, the film
take ends and recording works as above.

**Take lanes.** Live plays a take lane only while it is auditioned (T on the lane), and the script cannot switch that.
For a lane clip, Griddy says "Audition this lane (T) to hear it". The film lines up by position either way.

**Lining up.** The take stores which file it was filmed against (path, creation time, length) and where in that file
it starts (`filmedFor` in take.json). Every clip of that file then shows the film take at the matching place, to the
sample, by path. The output latency the host reports is taken off, since you play along to what comes out of the
speakers. The rest is the slip control, as for any take.

**Several videos for one file.** The newest kept or reviewed film take of a file is its video; older ones stay in
history and can be restored. Where the film does not cover the file (filmed only part of it), the video the file had
before shows, the take recorded with it, or black where there was none.

| Case | Before | During | After |
|---|---|---|---|
| Click a clip on a Griddy track | | "Film for: …", Film lit | |
| Click a clip on another track, or none | Film for the last clip | Film hidden | |
| Film, Live stopped | camera | Live plays from a bar before the clip; camera, red ring, "● FILM" | Live stops at the clip's end; the film take in review |
| Film on an armed track, or with Live's record on | | Live disarms the track and switches record off, then plays | as above |
| Stop film early | | Live stops | the film take in review, covering what was filmed |
| Keep | review | | every clip of the file shows the film take |
| Retake | review | | the file's video before |
| Film the same file again | the kept film take | | the new one in review, then kept; the old one in history |
| Live records during filming | filming | the film take ends; a normal take starts if the track is armed | |
| A lane clip | | "Audition this lane (T) to hear it" | |
| No Live script | | no Film button | |
| Move, cut, duplicate or comp the clip | | | the film take follows, as any take |
| A clip that loops past its loop end | | | placed by sound, as any take |

## Arm out of step and no take, seen in Live on 18 November, and why

From griddy.log, Live's Log.txt and the two shared files, for the set made from the "Griddy 9 Track Master" template:

| Time | Who | Live | Griddy |
|---|---|---|---|
| 00:01:58 | Griddy's Arm | armed at 00:01:59 | kept showing "Arm" |
| 00:02:01 to 00:02:14 | Griddy's Arm, six presses | armed and disarmed each time | kept showing "Arm" |
| 00:02:42 | Live's record button | recorded 1.6 s from bar 1 | Record arm never came on: no take |
| 00:04:18 to 00:04:25 | Live's record button, then Griddy's Arm twice | armed, nothing recorded | kept showing "Arm" |

Two causes:

1. The template was saved on 11 November, and Live keeps the list of a plugin's parameters it shows in the set. It
   had Slot, Master, Cameras, Record arm and Record when, but not Track armed in Live, so the script could not tell
   Griddy the track was armed. Fixed by sending the arm through the arrangement file too.
2. The script dropped its listeners on Live's record buttons right after it started: it kept only those whose subject
   "is" the song, and Live hands out a new song object each time. So Griddy followed Live's record button only when
   something else (an arm change) woke the script. Fixed by keeping them, and by checking every display tick.

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
* With the track armed in Live but not recording, black gaps showed under a "LIVE" overlay. The picture and the
  overlay now use one rule: an armed track shows its camera.
