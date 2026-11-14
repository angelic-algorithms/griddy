// A fake backend so the UI runs in a normal browser while you style it.
// It pretends Live is playing at 120 BPM, keeps a small set of slots and takes, and draws a framed stand in
// for the composite. A dev only picker (mockStates.tsx) switches it between every state the UI can show.
import { TEMPLATES } from "./kit";

type Listener = (payload: unknown) => void;
type Take = { id: number; state: "kept" | "review" | "archived"; seconds: number; slip: number; hasMov: boolean; startPpq: number; bpm: number; timeSig: string; fps: number; latencyMs: number; recognised?: boolean; heard?: string };
type MockSlot = { claimed: boolean; name: string; armed: boolean; recording: boolean; recordStartPpq: number; takes: Take[]; playingTake?: number; gap?: boolean };
type ExportState = "idle" | "waiting" | "recording" | "writing" | "done" | "failed";
type Store<T> = { get: () => T; set: (v: T) => void; subscribe: (fn: () => void) => () => void };

const SIZES = ["1280×720", "1920×1080", "1080×1080", "1080×1920"];
const DIMS = [[1280, 720], [1920, 1080], [1080, 1080], [1080, 1920]];
const NAMES = ["Vox", "Keys", "Bass", "Drums", "Guitar", "Synth", "Perc", "Strings", "FX"];

/** The engine's framed cells (CollageLayout::framedCellArea): gap and radius are measured on a 1280 wide canvas. */
function framedCells(preset: number, canvas: number, gap: number, order: number[]) {
  const t = TEMPLATES[preset];
  const [w, h] = DIMS[canvas];
  const g = (gap * w) / 1280;
  const cw = (w - g) / t.cols, ch = (h - g) / t.rows;
  return t.cells.map(([c, r, cs, rs], i) => ({
    slot: order[i] ?? i,
    x: (g + (c - 1) * cw) / w,
    y: (g + (r - 1) * ch) / h,
    w: (cs * cw - g) / w,
    h: (rs * ch - g) / h,
  }));
}

const take = (id: number, state: Take["state"], startPpq: number, seconds = 8, slip = 0): Take => ({
  id, state, seconds, slip, hasMov: state !== "review", startPpq, bpm: 120, timeSig: "4/4", fps: 30, latencyMs: 42.5,
});

export type MockScenario = { id: string; label: string };
export const MOCK_SCENARIOS: MockScenario[] = [
  { id: "mixed", label: "Four slots, mixed" },
  { id: "fresh", label: "Fresh session, stopped" },
  { id: "armed-stopped", label: "Armed, Live stopped" },
  { id: "armed-playing", label: "Armed, Live playing" },
  { id: "armed-recording", label: "Armed, Live recording" },
  { id: "countin", label: "Count in" },
  { id: "disarmed", label: "Disarmed after take" },
  { id: "recording", label: "Recording" },
  { id: "review", label: "Take in review" },
  { id: "comp", label: "Comp playing" },
  { id: "no-griddy", label: "No Griddy on this track" },
  { id: "master9", label: "Master, 3×3" },
  { id: "one", label: "One slot" },
  { id: "square", label: "Square, 1+5" },
  { id: "vertical", label: "9:16, stack 3" },
  { id: "loop", label: "Host loop on" },
  { id: "blocked", label: "Cameras blocked" },
  { id: "error", label: "Error line" },
  { id: "starting", label: "Engine starting" },
  { id: "export-waiting", label: "Export: waiting" },
  { id: "export-recording", label: "Export: recording" },
  { id: "export-writing", label: "Export: writing" },
  { id: "export-done", label: "Export: done" },
  { id: "export-failed", label: "Export: failed" },
];

export function createMock() {
  const listeners = new Map<string, Set<Listener>>();
  const emit = (name: string, payload: unknown) => listeners.get(name)?.forEach((fn) => fn(payload));

  const devices = [
    { id: "av:mock-facetime", name: "FaceTime HD Camera (mock)", kind: "camera" },
    { id: "av:mock-iphone", name: "iPhone Camera (mock)", kind: "camera" },
  ];
  let sources = ["av:mock-facetime", "av:mock-iphone", "av:mock-facetime", "av:mock-facetime", "", "", "", "", ""];
  let outputFolder = "/Users/you/Movies/Griddy";
  let serial = 0;
  let start = performance.now();
  const canvas = typeof document !== "undefined" ? document.createElement("canvas") : null;

  function store<T>(initial: T): Store<T> {
    let value = initial;
    const subs = new Set<() => void>();
    return {
      get: () => value,
      set: (v: T) => {
        value = v;
        subs.forEach((fn) => fn());
      },
      subscribe: (fn: () => void) => {
        subs.add(fn);
        return () => subs.delete(fn);
      },
    };
  }

  // Parameter stores, created by bridge.ts in its order: slot, numSlots for ints; master, recordArm for bools.
  const ints: Store<number>[] = [];
  const bools: Store<boolean>[] = [];
  const slotParam = () => ints[0];
  const masterParam = () => bools[0];
  const armParam = () => bools[1];

  // ---- the mock world ----
  let scenario = "mixed";
  let session = "b541fe4e-dbc2-452e-98c5-b4bd80d9dc93";
  let preset = 5;
  let layout = { canvas: 0, gap: 6, radius: 14, background: "ff6a1a", order: [0, 1, 2, 3, 4, 5, 6, 7, 8] };
  let slots: MockSlot[] = [];
  let playing = true;
  let hostLoop = false;
  // Live's record flag. Live never sends it to a plugin, so Griddy ignores it; the mock keeps it to show that.
  let liveRecording = false;
  let countInBeats = 0;
  let exportState: ExportState = "idle";
  let blocked = false;
  let lastError = "";
  let starting = false;
  let nextId = 20;

  const emptySlot = (): MockSlot => ({ claimed: false, name: "", armed: false, recording: false, recordStartPpq: 0, takes: [] });
  const ppqNow = () => (playing ? ((performance.now() - start) / 1000) * 2 - countInBeats : 0);
  const own = () => (masterParam()?.get() ? -1 : (slotParam()?.get() ?? 1) - 1);
  const panelsOf = () => TEMPLATES[preset].cells.length;

  function applyScenario(id: string) {
    scenario = id;
    slots = Array.from({ length: 9 }, emptySlot);
    const claim = (i: number, extra: Partial<MockSlot> = {}) => Object.assign(slots[i], { claimed: true, name: NAMES[i] }, extra);
    preset = 5;
    layout = { canvas: 0, gap: 6, radius: 14, background: "ff6a1a", order: [0, 1, 2, 3, 4, 5, 6, 7, 8] };
    playing = true;
    hostLoop = false;
    liveRecording = false;
    countInBeats = 0;
    exportState = "idle";
    blocked = false;
    lastError = "";
    starting = false;
    masterParam()?.set(false);
    slotParam()?.set(1);
    armParam()?.set(false);
    start = performance.now() - 4000;

    switch (id) {
      case "fresh":
        claim(0);
        playing = false;
        start = performance.now();
        break;
      case "armed-stopped":
        claim(0, { takes: [take(1, "kept", 0)] });
        claim(1, { takes: [take(2, "kept", 0)] });
        armParam()?.set(true);
        playing = false;
        start = performance.now();
        break;
      case "armed-playing":
      case "armed-recording":
        claim(0, { takes: [take(1, "kept", 0)] });
        claim(1, { takes: [take(2, "kept", 0)] });
        armParam()?.set(true);
        liveRecording = id === "armed-recording";
        start = performance.now();
        break;
      case "countin":
        claim(0, { takes: [take(1, "kept", 0)] });
        claim(1, { takes: [take(2, "kept", 0)] });
        armParam()?.set(true);
        countInBeats = 8;
        start = performance.now();
        break;
      case "disarmed":
        claim(0, { takes: [take(3, "review", 0, 6), take(2, "kept", 0)] });
        claim(1, { takes: [take(4, "kept", 0)] });
        break;
      case "recording":
        claim(0, { recording: true, recordStartPpq: 0, takes: [take(1, "kept", 0)] });
        claim(1, { takes: [take(2, "kept", 0)] });
        claim(2, { armed: true });
        armParam()?.set(true);
        break;
      case "review":
        claim(0, { takes: [take(3, "review", 0, 8), take(2, "kept", 0, 8, 0.02), take(1, "archived", 0)] });
        claim(1, { takes: [take(4, "kept", 0)] });
        break;
      case "comp":
        // Slot 1 plays a comp: an archived take is on screen now. Slot 2 is in a gap between clips. Slot 1 has 23 takes.
        claim(0, {
          playingTake: 21,
          takes: [take(23, "kept", 0), take(22, "archived", 0), { ...take(21, "archived", 0), heard: "VOX DOUBLE" },
            ...Array.from({ length: 20 }, (_, i) => ({ ...take(20 - i, "archived" as const, 0), recognised: i < 17 }))],
        });
        claim(1, { gap: true, takes: [take(30, "kept", 0), take(29, "archived", 0)] });
        claim(2, { playingTake: 31, takes: [take(31, "kept", 0)] });
        break;
      case "no-griddy":
        // The master Griddy shows two slots whose tracks have no Griddy: their takes follow Live's position only.
        preset = 8;
        for (let i = 0; i < 4; i++) claim(i, { takes: [take(60 + i, "kept", 0)] });
        Object.assign(slots[2], { claimed: false, name: "" });
        Object.assign(slots[3], { claimed: false, name: "" });
        masterParam()?.set(true);
        break;
      case "master9":
        preset = 12;
        for (let i = 0; i < 9; i++) claim(i, { takes: i % 3 === 2 ? [] : [take(30 + i, i % 4 === 1 ? "review" : "kept", 0)], armed: i === 2, recording: i === 5 });
        masterParam()?.set(true);
        break;
      case "one":
        preset = 0;
        claim(0, { takes: [take(1, "kept", 0)] });
        break;
      case "square":
        preset = 8;
        layout.canvas = 2;
        layout.background = "ff3d7f";
        for (let i = 0; i < 4; i++) claim(i, { takes: [take(40 + i, "kept", 0)] });
        break;
      case "vertical":
        preset = 4;
        layout.canvas = 3;
        claim(0, { takes: [take(1, "kept", 0)] });
        claim(1, { armed: true });
        break;
      case "loop":
        hostLoop = true;
        claim(0, { recording: true, recordStartPpq: 0, takes: [take(1, "kept", 0)] });
        claim(1, { takes: [take(2, "kept", 0)] });
        armParam()?.set(true);
        break;
      case "blocked":
        claim(0);
        blocked = true;
        break;
      case "error":
        claim(0, { takes: [take(1, "kept", 0)] });
        lastError = "Could not open the camera: it is in use by another app.";
        break;
      case "starting":
        starting = true;
        break;
      default:
        if (id.startsWith("export-")) {
          for (let i = 0; i < 4; i++) claim(i, { takes: [take(50 + i, "kept", 0)] });
          masterParam()?.set(true);
          exportState = id.slice(7) as ExportState;
        } else {
          // mixed: my slot has a take in review over a kept one, slot 2 kept, slot 3 armed and live, slot 4 free.
          claim(0, { takes: [take(3, "review", 0, 8), take(2, "kept", 0, 8, 0.02), take(1, "archived", 0)] });
          claim(1, { takes: [take(4, "kept", 0)] });
          claim(2, { armed: true });
        }
    }
    sources = slots.map((s, i) => (s.claimed ? (i === 1 ? "av:mock-iphone" : "av:mock-facetime") : ""));
    emit("status", status());
  }

  const mock = {
    scenarios: MOCK_SCENARIOS,
    scenario: () => scenario,
    setScenario: applyScenario,

    on(name: string, fn: Listener) {
      if (!listeners.has(name)) listeners.set(name, new Set());
      listeners.get(name)!.add(fn);
      return () => listeners.get(name)!.delete(fn);
    },

    async call(name: string, args: unknown[]): Promise<unknown> {
      const s = (i: number) => slots[i] ?? emptySlot();
      const result = (() => {
        switch (name) {
          case "getDevices":
            return devices;
          case "getSlots":
            return sources;
          case "setSlotSource": {
            const [slot, id] = args as [number, string];
            sources = sources.map((v, i) => (i === slot ? id : v));
            return { ok: true, error: "", slots: sources };
          }
          case "getStatus":
            return status();
          case "keepTake":
          case "restoreTake": {
            const [slot, id] = args as [number, number];
            s(slot).takes = s(slot).takes.map((t) => (t.id === id ? { ...t, state: "kept" } : t.state === "kept" ? { ...t, state: "archived" } : t));
            return true;
          }
          case "retakeTake": {
            const [slot, id] = args as [number, number];
            s(slot).takes = s(slot).takes.map((t) => (t.id === id ? { ...t, state: "archived" } : t));
            return true;
          }
          case "setSlip": {
            const [slot, id, seconds] = args as [number, number, number];
            s(slot).takes = s(slot).takes.map((t) => (t.id === id ? { ...t, slip: seconds } : t));
            return true;
          }
          case "takePeaks": {
            const [slot, id] = args as [number, number];
            return Array.from({ length: 64 }, (_, i) => Math.abs(Math.sin(i * 0.37 + id + slot)) * (0.35 + 0.65 * Math.abs(Math.sin(i * 0.11 + slot))));
          }
          case "resetSlot": {
            const [slot] = args as [number];
            Object.assign(s(slot), { armed: false, recording: false, takes: s(slot).takes.map((t) => ({ ...t, state: "archived" as const })) });
            if (slot === own()) armParam()?.set(false);
            return true;
          }
          case "newSession":
            session = `mock-${Math.random().toString(16).slice(2, 10)}`;
            slots.forEach((x) => (x.takes = []));
            return true;
          case "setLayout": {
            const [c, gap, background, radius] = args as [number, number, string, number];
            layout = { ...layout, canvas: c, gap, background, radius };
            return layoutVar();
          }
          case "choosePreset": {
            const [index] = args as [number];
            const panels = TEMPLATES[index]?.cells.length ?? 0;
            if (slots.some((x, i) => x.claimed && i >= panels)) return false;
            preset = index;
            return true;
          }
          case "swapCells": {
            const [a, b] = args as [number, number];
            const order = [...layout.order];
            [order[a], order[b]] = [order[b], order[a]];
            layout = { ...layout, order };
            return true;
          }
          case "startExport":
            exportState = playing ? "recording" : "waiting";
            return true;
          case "stopExport":
            exportState = exportState === "waiting" ? "idle" : "done";
            return true;
          case "chooseOutputFolder":
            outputFolder = "/Users/you/Desktop/Griddy takes";
            return outputFolder;
          default:
            return true;
        }
      })();
      queueMicrotask(() => emit("status", status()));
      return result;
    },

    previewUrl: () => canvas?.toDataURL("image/jpeg", 0.8) ?? "",

    choice: (choices: string[], initial: number) => {
      const st = store(initial);
      return { ...st, choices: () => choices };
    },
    int: (min: number, max: number, initial: number) => {
      const st = store(initial);
      ints.push(st);
      return { ...st, range: () => ({ min, max }) };
    },
    bool: () => {
      const st = store(false);
      bools.push(st);
      return st;
    },
  };

  function layoutVar() {
    const [width, height] = DIMS[layout.canvas];
    return {
      ...layout, width, height, preset, presetId: TEMPLATES[preset].id, sizes: SIZES,
      cells: framedCells(preset, layout.canvas, layout.gap, layout.order),
    };
  }

  function finishTake(slot: number) {
    const x = slots[slot];
    x.recording = false;
    // A take still in review is archived, not dropped: Live keeps the replaced recording, so its video stays.
    x.takes = [take(nextId++, "review", x.recordStartPpq, Math.max(1, (ppqNow() - x.recordStartPpq) / 2)),
      ...x.takes.map((t) => (t.state === "review" ? { ...t, state: "archived" as const } : t))];
  }

  function engineStatus() {
    const me = own();
    const panels = panelsOf();
    // The track's own arm lives in the recordArm parameter; R and the Rec button move it.
    if (me >= 0 && slots[me]) {
      // The one rule, as in the hub: armed plus play records one take, and the take ends the arm.
      if (slots[me].recording && (!armParam()?.get() || !playing)) {
        finishTake(me);
        armParam()?.set(false);
      }
      const armed = !!armParam()?.get();
      if (armed && playing && !slots[me].recording && ppqNow() >= 0) {
        slots[me].recording = true;
        slots[me].recordStartPpq = Math.max(0, ppqNow());
      }
      slots[me].armed = armed;
      slots[me].claimed = true;
      slots[me].name ||= NAMES[me];
    }
    const recording = me >= 0 && !!slots[me]?.recording;
    return {
      session, sessionFolder: `${outputFolder}/Sessions/${session}`, takesBytes: 412_000_000, slot: Math.max(0, me), instances: slots.filter((x) => x.claimed).length + (me < 0 ? 1 : 0),
      recording, armed: me >= 0 && !!slots[me]?.armed, master: me < 0, error: "",
      recordHint: me >= 0 && slots[me]?.armed && !slots[me]?.recording && !playing ? `Armed. Press play in Live to record slot ${me + 1}.` : "",
      claimedMask: slots.reduce((m, x, i) => (x.claimed ? m | (1 << i) : m), 0),
      slots: slots.slice(0, panels).map((x, slot) => ({
        slot, claimed: x.claimed, recording: x.recording, status: !x.takes.some((t) => t.state === "kept") ? "noTake" : x.claimed ? "locked" : "prior",
        source: !x.takes.length ? "none" : x.claimed ? "fingerprint" : "hostPosition", takeId: x.playingTake ?? x.takes.find((t) => t.state === "kept")?.id ?? -1, gap: !!x.gap, warp: false, lockMs: 180,
        recordStartPpq: x.recordStartPpq, trackName: x.name, armed: x.armed, review: x.takes.some((t) => t.state === "review"),
      })),
      takes: slots.slice(0, panels).map((x) => x.takes),
      layout: layoutVar(),
      export: { available: true, state: exportState, seconds: 12, file: "/Users/you/Movies/Griddy/Exports/mock.mp4", error: exportState === "failed" ? "The disk is full." : "" },
    };
  }

  function status() {
    return {
      engine: starting ? undefined : engineStatus(),
      outputFolder,
      lastTake: "",
      lastError,
      fps: 30,
      camerasAllowed: !blocked,
      cameraBlockedReason: blocked ? "Griddy can't use the camera. Allow it in System Settings, Privacy and Security, Camera, then reopen Live." : "",
      outputSize: SIZES[layout.canvas],
    };
  }

  // A framed stand in for the composite: the frame colour behind rounded cells, one colour per slot.
  function drawPreview(seconds: number) {
    const g = canvas?.getContext("2d");
    if (!g || !canvas) return;
    const [w, h] = DIMS[layout.canvas];
    const scale = 640 / Math.max(w, h);
    canvas.width = Math.round(w * scale);
    canvas.height = Math.round(h * scale);
    g.fillStyle = `#${layout.background}`;
    g.fillRect(0, 0, canvas.width, canvas.height);
    const beat = Math.floor(seconds * 2);
    const r = (layout.radius * w * scale) / 1280;
    framedCells(preset, layout.canvas, layout.gap, layout.order).forEach((c, i) => {
      const x = c.x * canvas.width, y = c.y * canvas.height, cw = c.w * canvas.width, ch = c.h * canvas.height;
      const sl = slots[c.slot];
      const has = sl && (sl.takes.some((t) => t.state !== "archived") || sl.armed || sl.recording);
      const hue = (sl?.playingTake ?? c.slot) * 47 + 20;
      g.fillStyle = sl?.gap && playing ? "#000" : has ? `hsl(${hue % 360} 45% ${beat % panelsOf() === i ? 46 : 34}%)` : "#141418";
      g.beginPath();
      g.roundRect(x, y, cw, ch, Math.min(r, cw / 2, ch / 2));
      g.fill();
    });
  }

  if (typeof window !== "undefined") {
    queueMicrotask(() => applyScenario("mixed"));
    setInterval(() => {
      const seconds = (performance.now() - start) / 1000;
      const ppq = playing ? seconds * 2 - countInBeats : 0;
      const loopLen = 16;
      const shown = hostLoop ? ppq % loopLen : ppq;
      emit("transport", {
        bpm: 120, ppq: shown, bar: Math.floor(shown / 4) + 1, beat: Math.floor(shown % 4) + 1,
        timeSig: "4/4", playing, recording: liveRecording, hostSync: true,
        looping: hostLoop, hasLoop: hostLoop, loopStart: 0, loopEnd: loopLen,
        levels: slots.slice(0, panelsOf()).map((x, i) => (x.claimed && playing ? 0.25 + 0.5 * Math.abs(Math.sin(seconds * (1.3 + i * 0.4))) : 0)),
      });
      drawPreview(seconds);
      emit("preview", ++serial);
    }, 1000 / 15);
    setInterval(() => emit("status", status()), 500);
  }

  return mock;
}
