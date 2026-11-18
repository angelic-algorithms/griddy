# Links Griddy's Record arm to Live: a track's Griddy records only while that track is armed in Live and Live records,
# so every Griddy take has an audio clip in Live to follow. Griddy's own arm button follows Live's.
#
# Live never tells a plugin whether its track is armed or whether Live records, so this runs as a control surface
# script (Settings, Link, Tempo & MIDI, Control Surface: Griddy), which can see both and set Griddy's parameter.
#
# One more thing it does: when a track with Griddy on it is armed and its Monitor is Off, it sets Monitor to Auto.
# With Monitor Off, Live sends the track's devices silence while recording, so Griddy would record no sound.
# It changes Monitor in no other case.

from _Framework.ControlSurface import ControlSurface

ARM_PARAMETER = "Record arm"
GRIDDY_PARAMETERS = ("Record arm", "Cameras")  # a plugin with both is Griddy, whatever the device is called
MONITOR_AUTO = 1
MONITOR_OFF = 2


def griddy_arm(device):
    """Griddy's Record arm parameter on this device, or None if the device is not Griddy."""
    if device.class_name not in ("PluginDevice", "AuPluginDevice"):
        return None
    names = dict((p.name, p) for p in device.parameters)
    if all(n in names for n in GRIDDY_PARAMETERS):
        return names[ARM_PARAMETER]
    return None


class Griddy(ControlSurface):
    def __init__(self, c_instance):
        ControlSurface.__init__(self, c_instance)
        self._listeners = []  # (remove function, listener) pairs to undo on disconnect
        self._pending = False
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
            self._listeners.append((remove, has, fn))

    def _forget_all(self, keep_song=False):
        kept = []
        for remove, has, fn in self._listeners:
            if keep_song and fn in (self._rebuild, self._request_sync):
                kept.append((remove, has, fn))
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
                    self._listen(arm, "value", self._request_sync)
        self._request_sync()

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

            on = track.arm and recording
            for arm in arms:
                want = arm.max if on else arm.min
                if arm.value != want and arm.is_enabled:
                    arm.value = want

    def disconnect(self):
        self._forget_all()
        ControlSurface.disconnect(self)
