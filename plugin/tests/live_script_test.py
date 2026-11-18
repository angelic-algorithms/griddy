# Drives the Griddy control surface script (live/Griddy) with a fake Live: python3 -B tests/live_script_test.py
# It checks the link's rules; whether Live's real objects behave like these fakes is only known by trying it in Live.

import json
import os
import sys
import tempfile
import types

failures = 0


def check(ok, what):
    global failures
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        failures += 1


class Listenable(object):
    def __init__(self):
        self._listeners = {}

    def _add(self, name, fn):
        self._listeners.setdefault(name, []).append(fn)

    def _notify(self, name):
        for fn in list(self._listeners.get(name, [])):
            fn()

    def __getattr__(self, attr):
        for prefix, kind in (("add_", "add"), ("remove_", "remove")):
            if attr.startswith(prefix) and attr.endswith("_listener"):
                name = attr[len(prefix):-len("_listener")]
                if kind == "add":
                    return lambda fn: self._add(name, fn)
                return lambda fn: self._listeners[name].remove(fn)
        if attr.endswith("_has_listener"):
            name = attr[:-len("_has_listener")]
            return lambda fn: fn in self._listeners.get(name, [])
        raise AttributeError(attr)

    def set(self, name, value):
        setattr(self, name, value)
        self._notify(name)


class Param(Listenable):
    def __init__(self, name):
        Listenable.__init__(self)
        self.name, self.value, self.min, self.max, self.is_enabled = name, 0.0, 0.0, 1.0, True

    def str_for_value(self, value):
        return "%d" % value

    def __setattr__(self, k, v):
        object.__setattr__(self, k, v)
        if k == "value" and "_listeners" in self.__dict__:
            self._notify("value")


class Device(Listenable):
    def __init__(self, name, class_name, params):
        Listenable.__init__(self)
        self.name, self.class_name, self.parameters = name, class_name, [Param(p) for p in params]


class Track(Listenable):
    def __init__(self, name, devices, monitor=1):
        Listenable.__init__(self)
        self.name, self.devices, self.arm, self.can_be_armed, self.current_monitoring_state = name, devices, False, True, monitor
        self.arrangement_clips, self.take_lanes, self.mute, self.muted_via_solo, self.playing_slot_index = [], [], False, False, -1

    def __setattr__(self, k, v):
        object.__setattr__(self, k, v)
        if k == "arm" and "_listeners" in self.__dict__:
            self._notify("arm")  # Live tells listeners when a script arms a track, as when you click Arm


class Clip(object):
    def __init__(self, start, end, path, marker=0.0, muted=False, gain=0.5, warping=True, looping=False, loop_end=1000.0):
        self.is_audio_clip, self.start_time, self.end_time, self.file_path = True, start, end, path
        self.start_marker, self.muted, self.gain, self.warping, self.looping, self.loop_end = marker, muted, gain, warping, looping, loop_end


class TakeLane(object):
    def __init__(self, clips):
        self.arrangement_clips = clips


class Song(Listenable):
    def __init__(self, tracks):
        Listenable.__init__(self)
        self.tracks, self.record_mode, self.session_record, self.is_playing, self.tempo = tracks, False, False, False, 120.0

    def __setattr__(self, k, v):
        object.__setattr__(self, k, v)
        if k in ("record_mode", "session_record") and "_listeners" in self.__dict__:
            self._notify(k)

    def start_playing(self):
        self.is_playing = True


scheduled = []


class FakeControlSurface(object):
    def __init__(self, c_instance):
        self._song = c_instance

    def song(self):
        return self._song

    def component_guard(self):
        class Guard(object):
            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False
        return Guard()

    def schedule_message(self, ticks, fn):
        scheduled.append(fn)

    def log_message(self, *a):
        pass

    def update_display(self):
        pass

    def disconnect(self):
        pass


def run_scheduled():
    for _ in range(10):
        if not scheduled:
            return
        pending = scheduled[:]
        del scheduled[:]
        for fn in pending:
            fn()


framework = types.ModuleType("_Framework")
cs = types.ModuleType("_Framework.ControlSurface")
cs.ControlSurface = FakeControlSurface
sys.modules["_Framework"] = framework
sys.modules["_Framework.ControlSurface"] = cs
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "live"))
import Griddy as script  # noqa: E402

link = sys.modules["Griddy.Griddy"]  # the module; the package name Griddy also names the class
link.TIMELINE_PATH = os.path.join(tempfile.mkdtemp(), "Griddy", "live-timeline.json")
link.REQUESTS_PATH = os.path.join(os.path.dirname(link.TIMELINE_PATH), "live-requests.json")
request_id = [1000]
written_requests = []


def press(action, slot=1, track="Vocals", age=0.0):
    """Griddy's REC or Arm, as the plugin writes it; the script does it on its next display tick."""
    import time
    request_id[0] += 1
    written_requests.append({"id": request_id[0], "slot": slot, "track": track, "action": action, "at": time.time() - age})
    folder = os.path.dirname(link.REQUESTS_PATH)
    if not os.path.isdir(folder):
        os.makedirs(folder)
    with open(link.REQUESTS_PATH, "w") as f:
        json.dump({"version": 1, "requests": written_requests[-8:]}, f)
    os.utime(link.REQUESTS_PATH, (request_id[0], request_id[0]))  # a new modification time each press, as on disk
    surface.update_display()
    run_scheduled()

griddy = lambda: Device("Griddy", "PluginDevice", ["Slot", "Cameras", "Record arm", "Master", "Track armed in Live"])
vocals = Track("Vocals", [griddy()], monitor=2)
guitar = Track("Guitar", [Device("EQ Eight", "Eq8Device", ["Gain"])], monitor=2)
song = Song([vocals, guitar])
arm = vocals.devices[0].parameters[2]
shown = vocals.devices[0].parameters[4]
vocals.devices[0].parameters[0].value = 1.0  # Griddy's Slot 1

folder = os.path.dirname(link.TIMELINE_PATH)
os.makedirs(folder)
with open(os.path.join(folder, "live-requests.json"), "w") as f:
    json.dump({"version": 1, "requests": [{"id": 5, "slot": 1, "track": "Vocals", "action": "arm", "at": 0}]}, f)
surface = script.create_instance(song)
run_scheduled()
surface.update_display()
run_scheduled()
check(not vocals.arm, "requests already in the file when the script starts are not done")
check(arm.value == 0.0, "nothing armed, Live not recording: Griddy is not armed")
check(vocals.current_monitoring_state == 2, "Monitor is left alone while the track is not armed")

vocals.set("arm", True)
run_scheduled()
check(arm.value == 0.0, "track armed but Live not recording: Griddy is not armed")
check(shown.value == 1.0, "track armed: Griddy's Track armed in Live is on, so it shows the camera")
check(vocals.current_monitoring_state == 1, "arming a Griddy track with Monitor Off sets it to Auto")
check(guitar.current_monitoring_state == 2, "a track without Griddy keeps Monitor Off")

song.set("record_mode", True)
run_scheduled()
check(arm.value == 1.0, "track armed and Live records: Griddy is armed")

song.set("record_mode", False)
run_scheduled()
check(arm.value == 0.0, "Live stops recording: Griddy is disarmed")

vocals.set("arm", False)
run_scheduled()
song.is_playing = False
arm.value = 1.0  # something other than the script writes Record arm, as Griddy's old disarm after a take did
run_scheduled()
check(arm.value == 0.0 and not song.record_mode and not vocals.arm, "a stray write to Record arm is put back and never reaches Live")

press("record")
check(not vocals.arm and not song.record_mode and not song.is_playing, "REC on a track not armed in Live does nothing in Live")
check(arm.value == 0.0, "and Griddy stays disarmed")

press("arm")
check(vocals.arm and shown.value == 1.0, "Arm on Griddy arms its track in Live, and Track armed in Live follows")
check(not song.record_mode and arm.value == 0.0, "arming does not record")
press("arm", slot=2, track="Guitar")
check(not guitar.arm, "a request for a slot with no Griddy track arms nothing")

press("record")
check(song.record_mode and song.is_playing, "REC on an armed track switches Live's record on and starts playing")
check(arm.value == 1.0, "and Griddy is armed with Live")

for _ in range(5):
    arm.value = 0.0  # Griddy writing its arm off mid take, many times
    run_scheduled()
check(arm.value == 1.0 and song.record_mode, "while Live records, a write of Record arm off is put back and Live keeps recording")

press("stop")
check(not song.record_mode and arm.value == 0.0, "REC again switches Live's record off, and Griddy disarms")
check(vocals.arm and song.is_playing, "the track stays armed and Live keeps playing, as after Live's own record button")
press("stop")
check(not song.record_mode, "a second stop changes nothing")

press("disarm")
check(not vocals.arm and shown.value == 0.0, "Arm off on Griddy disarms the track in Live")
press("arm", age=10.0)
check(not vocals.arm, "a press older than a few seconds is dropped")
press("arm", slot=7, track="Vocals")
check(vocals.arm, "a request whose slot is not found falls back to the track's name")

song.set("session_record", True)
run_scheduled()
check(arm.value == 1.0, "session recording counts as Live recording")
vocals.set("arm", False)
run_scheduled()
check(arm.value == 0.0, "disarming the track in Live disarms Griddy")
check(shown.value == 0.0, "and switches its Track armed in Live off")

vocals.current_monitoring_state = 0
vocals.set("arm", True)
run_scheduled()
check(vocals.current_monitoring_state == 0, "Monitor In is left alone")

late = Track("Keys", [], monitor=1)
song.tracks.append(late)
song._notify("tracks")
run_scheduled()
late.devices.append(griddy())
late._notify("devices")
run_scheduled()
late.set("arm", True)
run_scheduled()
check(late.devices[0].parameters[2].value == 1.0, "a Griddy added later is linked too")

renamed = Device("My camera", "PluginDevice", ["Slot", "Cameras", "Record arm"])
check(link.griddy_arm(renamed) is not None, "a renamed Griddy is still found by its parameters")
check(link.griddy_arm(Device("Other", "PluginDevice", ["Record arm"])) is None, "another plugin with a Record arm is not Griddy")

# The arrangement, as Griddy reads it. At 120 BPM a beat is half a second.
vocals.arrangement_clips = [
    Clip(8.0, 12.0, "/takes/a.wav"),                       # from beat 8, the file from its start: origin 4 s
    Clip(12.0, 14.0, "/takes/b.wav", marker=4.0),          # a piece of b from its beat 4: origin 4 s
    Clip(14.0, 16.0, "/takes/a.wav", marker=6.0, muted=True),
    Clip(16.0, 18.0, "/takes/b.wav", marker=8.0, gain=0.0),
    Clip(18.0, 26.0, "/takes/a.wav", looping=True, loop_end=2.0),
]
vocals.take_lanes = [TakeLane([Clip(8.0, 24.0, "/takes/b.wav")])]
surface.update_display()
with open(link.TIMELINE_PATH) as f:
    written = json.load(f)
tracks = dict((t["name"], t) for t in written["tracks"])
check(written["version"] == 1 and set(tracks) == set(["Vocals", "Keys"]), "the arrangement file lists every Griddy track and no other")
clips = tracks["Vocals"]["clips"]
check([c["start"] for c in clips[:5]] == [4.0, 6.0, 7.0, 8.0, 9.0] and clips[0]["end"] == 6.0, "clip edges are in seconds at the song's tempo")
check(clips[0]["origin"] == 4.0 and clips[1]["origin"] == 4.0, "each clip says where its file starts")
check(clips[0]["file"] == clips[2]["file"] and clips[0]["file"] != clips[1]["file"], "clips of one recording share a file number")
check(clips[0]["active"] and not clips[2]["active"] and not clips[3]["active"], "a clip deactivated with 0 or at zero gain does not play")
check("origin" not in clips[4], "a clip that loops past its loop end has no single file start")
check(clips[5].get("lane") and not clips[0].get("lane"), "take lane clips are marked, to tell which take a file is")
check(tracks["Vocals"]["audible"] and tracks["Vocals"]["arrangement"], "a playing track is audible and plays the arrangement")
vocals.mute = True
surface.update_display()
with open(link.TIMELINE_PATH) as f:
    check(not dict((t["name"], t) for t in json.load(f)["tracks"])["Vocals"]["audible"], "muting the track is written at once")

surface.disconnect()
check(not os.path.exists(link.TIMELINE_PATH), "disconnecting removes the arrangement file, so Griddy follows the sound again")
check(not any(song._listeners.values()) and not any(vocals._listeners.values()), "disconnect removes every listener")

print("LIVE SCRIPT TESTS PASSED" if failures == 0 else "LIVE SCRIPT TESTS FAILED (%d checks)" % failures)
sys.exit(1 if failures else 0)
