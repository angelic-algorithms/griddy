# Links Griddy to Live's arm and record buttons, so every Griddy take has an audio clip in Live to follow.
#
# Live owns arming and recording. A track's Griddy records only while that track is armed in Live and Live records:
# this script sets Griddy's Record arm parameter to exactly that, and nothing else writes it while the script runs.
# Griddy's REC and Arm buttons ask Live through a file, live-requests.json, which this script reads every display
# tick: REC on an armed track switches Live's record on (and plays if Live is stopped), REC again switches it off,
# and Arm arms or disarms that one track. A press never goes through a parameter, so it cannot come back as another
# press or be written as automation. (It used to: the script read any Record arm change it had not made as a press,
# and Griddy's own disarm after a take then started and stopped Live's recording every tenth of a second.)
#
# Live never tells a plugin whether its track is armed or whether Live records, so this runs as a control surface
# script (Settings, Link, Tempo & MIDI, Control Surface: Griddy), which can see both and set Griddy's parameter.
#
# One more thing it does: when a track with Griddy on it is armed and its Monitor is Off, it sets Monitor to Auto.
# With Monitor Off, Live sends the track's devices silence while recording, so Griddy would record no sound.
# It changes Monitor in no other case.
#
# It also copies Live's arm to Griddy's "Track armed in Live" parameter, so Griddy shows the camera on an armed track
# before Live records, and knows whether its REC can record.
#
# And it tells Griddy what the arrangement holds on every Griddy track: each clip's place, whether it plays (deactivated
# with 0 or at zero gain does not), and which recorded file it plays from where. It writes that to TIMELINE_PATH every
# second and whenever it changes; Griddy reads it to go black exactly where no clip plays and to switch takes on the
# frame of a cut, and takes it as the sign that this script runs. Without the file, Griddy still works it out from the
# sound, a little later, and its REC arms Griddy itself.
#
# Film: Griddy can film a video for a recording that is already there. The file says which recorded file each clip
# plays (by path) and which clip you clicked in Live. Griddy's Film button asks, through the requests file, to play from
# a bar before that clip to its end without recording: the script switches Live's record off and disarms the track
# first, so Live plays the clip and records nothing, and stops Live at the clip's end.

import json
import os
import time

from _Framework.ControlSurface import ControlSurface

ARM_PARAMETER = "Record arm"
TRACK_ARMED_PARAMETER = "Track armed in Live"
GRIDDY_PARAMETERS = ("Record arm", "Cameras")  # a plugin with both is Griddy, whatever the device is called
SLOT_PARAMETER = "Slot"
MASTER_PARAMETER = "Master"
MONITOR_AUTO = 1
MONITOR_OFF = 2
TIMELINE_PATH = os.path.expanduser("~/Library/Application Support/Griddy/live-timeline.json")
TIMELINE_EVERY = 1.0  # seconds: rewritten this often even unchanged, so Griddy can tell the script still runs
REQUESTS_PATH = os.path.join(os.path.dirname(TIMELINE_PATH), "live-requests.json")
REQUEST_MAX_AGE = 3.0  # seconds: an older press is dropped, so starting Live never replays one


def same(a, b):
    """Whether two of Live's objects are one: Live hands out a new Python object each time, so "is" never says."""
    if a is None or b is None:
        return False
    if a is b:
        return True
    pa, pb = getattr(a, "_live_ptr", None), getattr(b, "_live_ptr", None)
    if pa is not None and pb is not None:
        return pa == pb
    try:
        return a == b
    except Exception:
        return False


def griddy_arm(device):
    """Griddy's Record arm parameter on this device, or None if the device is not Griddy."""
    if device.class_name not in ("PluginDevice", "AuPluginDevice"):
        return None
    names = dict((p.name, p) for p in device.parameters)
    if all(n in names for n in GRIDDY_PARAMETERS):
        return names[ARM_PARAMETER]
    return None


def griddy_track_armed(device):
    """Griddy's Track armed in Live parameter on this device, or None (not Griddy, or a Griddy built before it)."""
    if griddy_arm(device) is None:
        return None
    for p in device.parameters:
        if p.name == TRACK_ARMED_PARAMETER:
            return p
    return None


def griddy_parameters(device):
    """All of a Griddy device's parameters by name, or None if the device is not Griddy."""
    if griddy_arm(device) is None:
        return None
    return dict((p.name, p) for p in device.parameters)


def is_on(parameter):
    return parameter.value > (parameter.min + parameter.max) / 2.0


def clip_origin(clip, seconds_per_beat):
    """Host seconds where the clip's file would start, or None when it cannot be said (a clip that loops)."""
    try:
        warped = clip.warping
        marker = clip.start_marker  # beats for a warped clip, seconds for an unwarped one
        if clip.looping:
            length = clip.end_time - clip.start_time
            if not warped:
                length *= seconds_per_beat
            if length > clip.loop_end - marker + 1e-6:
                return None  # it plays past its loop end and repeats
        if warped:
            return (clip.start_time - marker) * seconds_per_beat
        return clip.start_time * seconds_per_beat - marker
    except Exception:
        return None


class Griddy(ControlSurface):
    def __init__(self, c_instance):
        ControlSurface.__init__(self, c_instance)
        self._listeners = []  # (remove function, listener) pairs to undo on disconnect
        self._pending = False
        self._requests_at = None  # the requests file's modification time when last read
        self._request_id = None   # the newest request done; None until the file is first read, which does none
        self._timeline_text = None
        self._timeline_at = 0.0
        self._born = {}       # file path -> when it was created, seconds since the epoch
        self._timeline_failed = False
        self._record_from = None  # beat where Live's arrangement recording began, while it records
        self._armed_from = {}     # track name -> beat where it was armed while Live recorded
        self._was_recording = False
        self._last_arm = {}       # track name -> arm last seen, to log changes made in Live
        self._film_until = None   # while Griddy films: the beat where Live stops, the end of the clip filmed
        self._film_started = False
        with self.component_guard():
            song = self.song()
            # Kept for the script's whole life. (They used to be dropped at every rebuild: Live hands out a new song
            # object each time, so "is it the song" was never true, and Griddy stopped following Live's record button.)
            self._listen(song, "tracks", self._rebuild, keep=True)
            self._listen(song, "record_mode", self._record_changed, keep=True)
            self._listen(song, "session_record", self._record_changed, keep=True)
            self._listen(song, "is_playing", self._playing_changed, keep=True)
            self._rebuild()
        self.log_message("Griddy: linking Griddy's Record arm to Live's arm and record buttons")

    # Listeners ---------------------------------------------------------------------------------------------------

    def _listen(self, subject, name, fn, keep=False):
        add = getattr(subject, "add_%s_listener" % name)
        remove = getattr(subject, "remove_%s_listener" % name)
        has = getattr(subject, "%s_has_listener" % name)
        if not has(fn):
            add(fn)
            self._listeners.append((keep, remove, has, fn))

    def _forget_all(self, keep_song=False):
        kept = []
        for entry in self._listeners:
            keep, remove, has, fn = entry
            if keep_song and keep:
                kept.append(entry)
                continue
            try:
                if has(fn):
                    remove(fn)
            except Exception:
                pass  # the track or device is already gone
        self._listeners = kept

    def _rebuild(self):
        """Tracks or devices changed: listen to every track that can be armed and every Griddy on it."""
        self._forget_all(keep_song=True)
        for track in self.song().tracks:
            if not getattr(track, "can_be_armed", False):
                continue
            self._listen(track, "arm", self._arm_changed(track))
            self._listen(track, "devices", self._rebuild_later)
            for device in track.devices:
                arm = griddy_arm(device)
                if arm is not None:
                    self._listen(arm, "value", self._request_sync)  # anything else writing it is put back
        self._request_sync()

    def _arm_changed(self, track):
        """A listener for one track's arm: notes where a track armed during a recording starts recording."""
        def listener():
            song = self.song()
            if track.arm and song.record_mode and song.is_playing:
                self._armed_from[track.name] = song.current_song_time
            elif not track.arm:
                self._armed_from.pop(track.name, None)
            self._request_sync()
        listener.__name__ = "griddy_track_arm_%d" % id(listener)
        return listener

    def _record_changed(self):
        song = self.song()
        if song.record_mode and self._record_from is None:
            self._record_from = self._recording_starts(song)
            self.log_message("Griddy: Live's record on, recording from beat %.3f" % self._record_from)
        elif not song.record_mode and self._record_from is not None:
            self._record_from = None
            self._armed_from = {}
            self.log_message("Griddy: Live's record off")
        self._request_sync()

    def _playing_changed(self):
        song = self.song()
        if song.is_playing and song.record_mode:
            # Record was on with Live stopped: the recording starts where playback does.
            self._record_from = self._recording_starts(song)
            self._armed_from = {}
        self._request_sync()

    def _recording_starts(self, song):
        """The beat Live's arrangement recording starts at: now, or the punch in point if it is later."""
        start = song.current_song_time
        if getattr(song, "punch_in", False) and getattr(song, "loop_start", start) > start:
            start = song.loop_start
        return start

    def _rebuild_later(self):
        # Live does not allow changes from inside a notification; rebuilding listeners is safe a tick later too.
        self.schedule_message(1, self._rebuild)

    def _request_sync(self):
        if not self._pending:
            self._pending = True
            self.schedule_message(1, self._sync)

    # The link ----------------------------------------------------------------------------------------------------

    def _recording(self):
        song = self.song()
        return bool(song.record_mode or song.session_record)

    def _sync(self):
        self._pending = False
        song = self.song()

        recording = self._recording()
        if recording != self._was_recording:
            self._was_recording = recording
            self.log_message("Griddy: Live %s" % ("records" if recording else "stops recording"))
        for track in self.song().tracks:
            if not getattr(track, "can_be_armed", False):
                continue
            arms = [a for a in (griddy_arm(d) for d in track.devices) if a is not None]
            if not arms:
                continue

            if self._last_arm.get(track.name) != bool(track.arm):
                self._last_arm[track.name] = bool(track.arm)
                self.log_message("Griddy: %s %s in Live" % (track.name, "armed" if track.arm else "disarmed"))

            if track.arm and track.current_monitoring_state == MONITOR_OFF:
                track.current_monitoring_state = MONITOR_AUTO
                self.log_message("Griddy: %s Monitor set from Off to Auto, so Griddy hears what it records" % track.name)

            for shown in (griddy_track_armed(d) for d in track.devices):
                if shown is not None:
                    want = shown.max if track.arm else shown.min
                    if shown.value != want and shown.is_enabled:
                        shown.value = want

            on = track.arm and recording
            for arm in arms:
                want = arm.max if on else arm.min
                if arm.value != want and arm.is_enabled:
                    arm.value = want

    # Presses of Griddy's REC and Arm ---------------------------------------------------------------------------

    def _read_requests(self):
        """New requests from live-requests.json, oldest first. The first read only notes where the file is."""
        try:
            at = os.path.getmtime(REQUESTS_PATH)
        except OSError:
            if self._request_id is None:
                self._request_id = 0
            return []
        if at == self._requests_at:
            return []
        self._requests_at = at
        try:
            with open(REQUESTS_PATH) as f:
                listed = json.load(f).get("requests", [])
        except (IOError, OSError, ValueError):
            return []
        newest = max([r.get("id", 0) for r in listed] + [0])
        if self._request_id is None:
            self._request_id = newest
            return []
        fresh = [r for r in listed if r.get("id", 0) > self._request_id]
        self._request_id = max(self._request_id, newest)
        now = time.time()
        return sorted([r for r in fresh if now - r.get("at", 0) <= REQUEST_MAX_AGE], key=lambda r: r.get("id", 0))

    def _griddy_track(self, request):
        """The track whose Griddy asked: by its Slot, else by the track's name."""
        named = None
        for track in self.song().tracks:
            if not getattr(track, "can_be_armed", False):
                continue
            for device in track.devices:
                params = griddy_parameters(device)
                if params is None or (MASTER_PARAMETER in params and is_on(params[MASTER_PARAMETER])):
                    continue
                slot = params.get(SLOT_PARAMETER)
                if slot is not None and hasattr(slot, "str_for_value") and slot.str_for_value(slot.value).strip() == str(request.get("slot")):
                    return track
                if named is None and track.name == request.get("track"):
                    named = track
        return named

    def _do_requests(self):
        song = self.song()
        for request in self._read_requests():
            action = request.get("action")
            track = self._griddy_track(request)
            if track is None:
                self.log_message("Griddy: a request to %s found no Griddy track for slot %s" % (action, request.get("slot")))
                continue
            if action == "arm" and not track.arm:
                track.arm = True
                self.log_message("Griddy: Arm on %s's Griddy, so Live arms it" % track.name)
            elif action == "disarm" and track.arm:
                track.arm = False
                self.log_message("Griddy: Arm off on %s's Griddy, so Live disarms it" % track.name)
            elif action == "record":
                if not track.arm:
                    self.log_message("Griddy: REC on %s, which is not armed, so Live does nothing" % track.name)
                    continue
                if not song.record_mode:
                    song.record_mode = True
                if not song.is_playing:
                    song.start_playing()
                self.log_message("Griddy: REC on %s, so Live records" % track.name)
            elif action == "stop" and self._recording():
                song.record_mode = False
                song.session_record = False
                self.log_message("Griddy: REC off on %s, so Live stops recording" % track.name)
            elif action == "film":
                self._film(song, track, request)
            elif action == "filmstop" and self._film_until is not None:
                self._film_until = None
                song.stop_playing()
                self.log_message("Griddy: Stop film on %s, so Live stops" % track.name)

    def _film(self, song, track, request):
        """Play the clip to film, from a bar before it to its end, recording nothing."""
        seconds_per_beat = 60.0 / song.tempo
        start = float(request.get("from", 0.0)) / seconds_per_beat
        end = float(request.get("to", 0.0)) / seconds_per_beat
        if end <= start:
            self.log_message("Griddy: Film on %s asked for no clip" % track.name)
            return
        if song.record_mode or song.session_record:
            song.record_mode = False
            song.session_record = False
            self.log_message("Griddy: Film on %s, so Live's record is switched off first" % track.name)
        if track.arm:
            track.arm = False
            self.log_message("Griddy: Film on %s, so Live disarms it and plays its clips" % track.name)
        beats_per_bar = 4.0 * getattr(song, "signature_numerator", 4) / float(getattr(song, "signature_denominator", 4))
        song.current_song_time = max(0.0, start - beats_per_bar)  # playing, this jumps there
        if not song.is_playing:
            song.continue_playing()  # from the playhead just set; start_playing would go from the start marker
        self._film_until = end
        self._film_started = False
        self.log_message("Griddy: Film on %s, so Live plays beats %.3f to %.3f" % (track.name, song.current_song_time, end))

    def _check_film(self):
        """While filming: stop Live at the end of the clip. Stopping Live in any other way ends filming too."""
        if self._film_until is None:
            return
        song = self.song()
        if song.is_playing:
            self._film_started = True
            if song.current_song_time >= self._film_until:
                self._film_until = None
                song.stop_playing()
                self.log_message("Griddy: the clip filmed has ended, so Live stops")
        elif self._film_started:
            self._film_until = None

    # The arrangement -------------------------------------------------------------------------------------------

    def update_display(self):
        ControlSurface.update_display(self)
        try:
            self._do_requests()
        except Exception as e:
            self.log_message("Griddy: could not do a request from Griddy: %s" % e)
        try:
            self._check_film()
        except Exception as e:
            self.log_message("Griddy: could not stop Live after filming: %s" % e)
        try:
            # Every tick, not only on Live's notifications: a missed one can no longer leave Griddy out of step.
            self._sync()
        except Exception as e:
            self.log_message("Griddy: could not set Griddy's arm: %s" % e)
        try:
            self._write_timeline()
        except Exception as e:
            if not self._timeline_failed:
                self._timeline_failed = True
                self.log_message("Griddy: could not write the arrangement for Griddy: %s" % e)

    def _file_born(self, path):
        if path not in self._born:
            try:
                st = os.stat(path)
                self._born[path] = float(getattr(st, "st_birthtime", 0.0))
            except OSError:
                self._born[path] = 0.0
        return self._born[path]

    def _track_timeline(self, track, params, seconds_per_beat):
        files = {}
        clips = []
        song = self.song()
        view = getattr(song, "view", None)
        chosen = getattr(view, "detail_clip", None) if view is not None else None
        beats_per_bar = 4.0 * getattr(song, "signature_numerator", 4) / float(getattr(song, "signature_denominator", 4))

        def add(clip, lane):
            if not getattr(clip, "is_audio_clip", False):
                return
            entry = {
                "start": clip.start_time * seconds_per_beat,
                "end": clip.end_time * seconds_per_beat,
                "active": (not clip.muted) and getattr(clip, "gain", 1.0) > 0.0,
            }
            path = getattr(clip, "file_path", "") or ""
            if path:
                entry["file"] = files.setdefault(path, len(files))
                entry["born"] = self._file_born(path)
            origin = clip_origin(clip, seconds_per_beat)
            if origin is not None:
                entry["origin"] = origin
            if lane:
                entry["lane"] = True
            if same(clip, chosen):
                entry["selected"] = True  # the clip you clicked in Live: Griddy's Film films for its recording
                entry["bar"] = int(clip.start_time // beats_per_bar) + 1
            clips.append(entry)

        for clip in getattr(track, "arrangement_clips", []):
            add(clip, False)
        for take_lane in getattr(track, "take_lanes", []):
            for clip in getattr(take_lane, "arrangement_clips", []):
                add(clip, True)

        slot = params[SLOT_PARAMETER] if SLOT_PARAMETER in params else None
        entry = {
            "name": track.name,
            "armed": bool(track.arm),
            "slot": slot.str_for_value(slot.value) if slot is not None and hasattr(slot, "str_for_value") else str(slot or ""),
            "audible": not (track.mute or getattr(track, "muted_via_solo", False)),
            "arrangement": getattr(track, "playing_slot_index", -1) < 0,
            "clips": clips,
            "files": [p for p, _ in sorted(files.items(), key=lambda kv: kv[1])],  # each file number's path
        }
        if track.arm and song.record_mode and self._record_from is not None:
            # Where this track's recording begins, so Griddy's take starts there too, not when Griddy hears of it.
            entry["recordFrom"] = max(self._record_from, self._armed_from.get(track.name, self._record_from))
        return entry

    def _write_timeline(self):
        song = self.song()
        seconds_per_beat = 60.0 / song.tempo
        tracks = []
        for track in song.tracks:
            if not getattr(track, "can_be_armed", False):
                continue
            for device in track.devices:
                params = griddy_parameters(device)
                if params is None or (MASTER_PARAMETER in params and is_on(params[MASTER_PARAMETER])):
                    continue
                tracks.append(self._track_timeline(track, params, seconds_per_beat))
                break

        text = json.dumps(tracks, sort_keys=True)
        now = time.time()
        if text == self._timeline_text and now - self._timeline_at < TIMELINE_EVERY:
            return
        self._timeline_text = text
        self._timeline_at = now

        folder = os.path.dirname(TIMELINE_PATH)
        if not os.path.isdir(folder):
            os.makedirs(folder)
        temporary = TIMELINE_PATH + ".tmp"
        with open(temporary, "w") as f:
            json.dump({"version": 1, "written": now, "recording": self._recording(), "tracks": tracks}, f)
        os.replace(temporary, TIMELINE_PATH)

    def disconnect(self):
        self._forget_all()
        try:
            os.remove(TIMELINE_PATH)  # Griddy goes back to following the sound
        except OSError:
            pass
        ControlSurface.disconnect(self)
