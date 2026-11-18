# Links Griddy's Record arm to Live: a track's Griddy records only while that track is armed in Live and Live records,
# so every Griddy take has an audio clip in Live to follow. Griddy's own arm follows Live's, and pressing Griddy's REC
# asks Live: it arms that track, switches Live's record button on and starts playback if Live is stopped. Pressing REC
# again while Live records switches Live's record button off.
#
# Live never tells a plugin whether its track is armed or whether Live records, so this runs as a control surface
# script (Settings, Link, Tempo & MIDI, Control Surface: Griddy), which can see both and set Griddy's parameter.
#
# One more thing it does: when a track with Griddy on it is armed and its Monitor is Off, it sets Monitor to Auto.
# With Monitor Off, Live sends the track's devices silence while recording, so Griddy would record no sound.
# It changes Monitor in no other case.
#
# It also copies Live's arm to Griddy's "Track armed in Live" parameter, so Griddy shows the camera on an armed track
# before Live records. That parameter only changes what Griddy shows.
#
# And it tells Griddy what the arrangement holds on every Griddy track: each clip's place, whether it plays (deactivated
# with 0 or at zero gain does not), and which recorded file it plays from where. It writes that to TIMELINE_PATH every
# second and whenever it changes; Griddy reads it to go black exactly where no clip plays and to switch takes on the
# frame of a cut. Without the file, Griddy still works it out from the sound, a little later.

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
        self._set_by_us = {}  # id(parameter) -> the value this script wants it to have; anything else is a press
        self._requests = []   # (track, wants recording) from presses of Griddy's REC
        self._timeline_text = None
        self._timeline_at = 0.0
        self._born = {}       # file path -> when it was created, seconds since the epoch
        self._timeline_failed = False
        with self.component_guard():
            song = self.song()
            self._listen(song, "tracks", self._rebuild)
            self._listen(song, "record_mode", self._request_sync)
            self._listen(song, "session_record", self._request_sync)
            self._rebuild()
        self.log_message("Griddy: linking Griddy's Record arm to Live's arm and record buttons")

    # Listeners ---------------------------------------------------------------------------------------------------

    def _listen(self, subject, name, fn):
        add = getattr(subject, "add_%s_listener" % name)
        remove = getattr(subject, "remove_%s_listener" % name)
        has = getattr(subject, "%s_has_listener" % name)
        if not has(fn):
            add(fn)
            self._listeners.append((subject, remove, has, fn))

    def _forget_all(self, keep_song=False):
        kept = []
        song = self.song()
        for entry in self._listeners:
            subject, remove, has, fn = entry
            if keep_song and subject is song:
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
            self._listen(track, "arm", self._request_sync)
            self._listen(track, "devices", self._rebuild_later)
            for device in track.devices:
                arm = griddy_arm(device)
                if arm is not None:
                    self._listen(arm, "value", self._pressed(track, arm))
        self._request_sync()

    def _pressed(self, track, arm):
        """A listener for one Griddy's Record arm. A change this script did not make is the user pressing REC."""
        def listener():
            if self._set_by_us.get(id(arm)) != arm.value:
                self._requests.append((track, arm.value > (arm.min + arm.max) / 2.0))
            self._request_sync()
        listener.__name__ = "griddy_arm_%d" % id(arm)
        return listener

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

        # Presses of Griddy's REC become Live's: record on this track, or stop recording.
        requests, self._requests = self._requests, []
        for track, wants in requests:
            if wants and not (track.arm and self._recording()):
                if not track.arm:
                    track.arm = True
                if not song.record_mode:
                    song.record_mode = True
                if not song.is_playing:
                    song.start_playing()
                self.log_message("Griddy: REC on %s, so Live arms it and records" % track.name)
            elif not wants and track.arm and self._recording():
                song.record_mode = False
                song.session_record = False
                self.log_message("Griddy: REC off on %s, so Live stops recording" % track.name)

        recording = self._recording()
        for track in self.song().tracks:
            if not getattr(track, "can_be_armed", False):
                continue
            arms = [a for a in (griddy_arm(d) for d in track.devices) if a is not None]
            if not arms:
                continue

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
                self._set_by_us[id(arm)] = want
                if arm.value != want and arm.is_enabled:
                    arm.value = want

    # The arrangement -------------------------------------------------------------------------------------------

    def update_display(self):
        ControlSurface.update_display(self)
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
            clips.append(entry)

        for clip in getattr(track, "arrangement_clips", []):
            add(clip, False)
        for take_lane in getattr(track, "take_lanes", []):
            for clip in getattr(take_lane, "arrangement_clips", []):
                add(clip, True)

        slot = params[SLOT_PARAMETER] if SLOT_PARAMETER in params else None
        return {
            "name": track.name,
            "slot": slot.str_for_value(slot.value) if slot is not None and hasattr(slot, "str_for_value") else str(slot or ""),
            "audible": not (track.mute or getattr(track, "muted_via_solo", False)),
            "arrangement": getattr(track, "playing_slot_index", -1) < 0,
            "clips": clips,
        }

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
            json.dump({"version": 1, "written": now, "tracks": tracks}, f)
        os.replace(temporary, TIMELINE_PATH)

    def disconnect(self):
        self._forget_all()
        try:
            os.remove(TIMELINE_PATH)  # Griddy goes back to following the sound
        except OSError:
            pass
        ControlSurface.disconnect(self)
