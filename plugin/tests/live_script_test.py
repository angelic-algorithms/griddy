# Drives the Griddy control surface script (live/Griddy) with a fake Live: python3 -B tests/live_script_test.py
# It checks the link's rules; whether Live's real objects behave like these fakes is only known by trying it in Live.

import os
import sys
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


class Song(Listenable):
    def __init__(self, tracks):
        Listenable.__init__(self)
        self.tracks, self.record_mode, self.session_record = tracks, False, False


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

griddy = lambda: Device("Griddy", "PluginDevice", ["Slot", "Cameras", "Record arm", "Master"])
vocals = Track("Vocals", [griddy()], monitor=2)
guitar = Track("Guitar", [Device("EQ Eight", "Eq8Device", ["Gain"])], monitor=2)
song = Song([vocals, guitar])
arm = vocals.devices[0].parameters[2]

surface = script.create_instance(song)
run_scheduled()
check(arm.value == 0.0, "nothing armed, Live not recording: Griddy is not armed")
check(vocals.current_monitoring_state == 2, "Monitor is left alone while the track is not armed")

vocals.set("arm", True)
run_scheduled()
check(arm.value == 0.0, "track armed but Live not recording: Griddy is not armed")
check(vocals.current_monitoring_state == 1, "arming a Griddy track with Monitor Off sets it to Auto")
check(guitar.current_monitoring_state == 2, "a track without Griddy keeps Monitor Off")

song.set("record_mode", True)
run_scheduled()
check(arm.value == 1.0, "track armed and Live records: Griddy is armed")

arm.value = 0.0  # the user clicks Griddy's own arm button off, or the plugin turns it off after a take
run_scheduled()
check(arm.value == 1.0, "Griddy's own arm follows Live's while Live records")

song.set("record_mode", False)
run_scheduled()
check(arm.value == 0.0, "Live stops recording: Griddy is disarmed")

arm.value = 1.0  # the user clicks Griddy's arm on while Live is not recording
run_scheduled()
check(arm.value == 0.0, "Griddy cannot be armed on its own")

song.set("session_record", True)
run_scheduled()
check(arm.value == 1.0, "session recording counts as Live recording")
vocals.set("arm", False)
run_scheduled()
check(arm.value == 0.0, "disarming the track in Live disarms Griddy")

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

surface.disconnect()
check(not any(song._listeners.values()) and not any(vocals._listeners.values()), "disconnect removes every listener")

print("LIVE SCRIPT TESTS PASSED" if failures == 0 else "LIVE SCRIPT TESTS FAILED (%d checks)" % failures)
sys.exit(1 if failures else 0)
