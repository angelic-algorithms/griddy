// The one place the UI talks to C++. Everything else imports from here, so the
// React components never touch window.__JUCE__ directly.
//
// Inside the plugin, these call into GriddyEditor (source/PluginEditor.cpp).
// In a normal browser (npm run dev, then open localhost:5173) there is no
// backend, so we fall back to a small mock that fakes a playing DAW. That
// lets you work on the UI without rebuilding the plugin.
import {
  getNativeFunction,
  getComboBoxState,
  getSliderState,
  getToggleState,
  getBackendResourceAddress,
} from "@juce-framework/webview";
import { createMock } from "./mock";

export type Device = { id: string; name: string; kind: "camera" | "test" | string };

export type Transport = {
  bpm: number;
  ppq: number;
  bar: number;
  beat: number;
  timeSig: string;
  playing: boolean;
  recording: boolean;
  hostSync: boolean;
  looping?: boolean;
  hasLoop?: boolean;
  loopStart?: number;
  loopEnd?: number;
  levels?: number[];
};

export type Status = {
  outputFolder: string;
  lastTake: string;
  lastError: string;
  fps: number;
  camerasAllowed: boolean;
  cameraBlockedReason: string;
  outputSize: string;
  engine?: EngineStatus;
};

export type TakeInfo = {
  id: number;
  state: "kept" | "review" | "archived";
  seconds: number;
  slip: number;
  hasMov: boolean;
  startPpq?: number;
  bpm?: number;
  timeSig?: string;
  fps?: number;
  latencyMs?: number;
  /** False past the recognition cap: the take is kept and restorable, but its video no longer follows comps. */
  recognised?: boolean;
  /** The track whose audio the take listened to, when the host named it. */
  heard?: string;
  /** The take recorded no sound (Live's Monitor was off), so it cannot be told apart in a comp. */
  noSound?: boolean;
  /** A film take: the recording it was filmed for, by file name. */
  filmFor?: string;
};

/** One recorded file on this track's arrangement, for Film. */
export type FilmRecording = {
  file: number;
  name: string;
  /** Only on a take lane. */
  lane: boolean;
  start?: number;
  /** Its clip loops past its loop end: Griddy cannot tell where the recording starts, so it cannot be filmed. */
  loops: boolean;
  /** "filmed": a film take shows for it; "video": the take recorded with it; "none": no video. */
  video: "filmed" | "video" | "none";
  bar?: number;
};

/** Film: a video for a recording already in Live, filmed while Live plays it. Needs the Live script. */
export type FilmStatus = {
  available: boolean;
  /** Film was pressed: Griddy waits for Live to play the clip, then films it. */
  armed: boolean;
  filming: boolean;
  /** What Film films for, while armed or filming. */
  name?: string;
  /** The clip clicked in Live, when it is on this track. */
  selected?: FilmRecording;
  recordings?: FilmRecording[];
};

export type SlotInfo = {
  slot: number;
  claimed: boolean;
  recording: boolean;
  status: "noTake" | "stopped" | "prior" | "locked" | "held" | "lost";
  source: "none" | "hostPosition" | "fingerprint" | "embedded" | "timeline";
  takeId: number;
  warp: boolean;
  lockMs: number;
  recordStartPpq?: number;
  trackName?: string;
  armed?: boolean;
  /** Live has this track armed (from the Live script), whether or not Live records. */
  liveArmed?: boolean;
  /** A camera is picked for this slot; false once the camera is turned off. */
  camera?: boolean;
  review?: boolean;
  /** No clip plays here: the picture is black. */
  gap?: boolean;
  /** Recording a film take. */
  filming?: boolean;
};

/** One cell of the picture, 0..1 of the canvas, frame included. */
export type CellRect = { slot: number; x: number; y: number; w: number; h: number };

export type Layout = {
  canvas: number;
  width: number;
  height: number;
  preset: number;
  presetId: string;
  gap: number;
  radius: number;
  background: string;
  order: number[];
  cells: CellRect[];
  /** Output size of every canvas, by canvas number, like "1280×720". */
  sizes?: string[];
};

export type ExportStatus = {
  available: boolean;
  state: "idle" | "waiting" | "recording" | "writing" | "done" | "failed";
  seconds: number;
  file: string;
  error: string;
};

export type EngineStatus = {
  session: string;
  sessionFolder?: string;
  /** Disk use of this session's takes in bytes; -1 until measured. */
  takesBytes?: number;
  resetPending?: boolean;
  slot: number;
  instances: number;
  recording: boolean;
  armed: boolean;
  recordHint?: string;
  pendingFolder?: string;
  hostRecording?: boolean;
  /** The Live script runs: Live owns this track's arm, and REC and Arm ask Live through requestInLive. */
  liveScript?: boolean;
  film?: FilmStatus;
  claimedMask?: number;
  master: boolean;
  error: string;
  slots: SlotInfo[];
  takes: TakeInfo[][];
  layout: Layout;
  export: ExportStatus;
  framesWritten?: number;
  framesDropped?: number;
  audioDropped?: number;
};

export type SetSlotResult = { ok: boolean; error: string; slots: string[] };

const juce = (window as any).__JUCE__;
export const isPlugin: boolean =
  !!juce?.initialisationData?.__juce__platform?.length;

// Only used outside the plugin (a plain browser during UI work). Inside the plugin it is never created.
const mock = (isPlugin ? null : createMock()) as ReturnType<typeof createMock>;
/** The mock backend, for the dev only state picker. Undefined inside the plugin. */
export const devMock = isPlugin ? undefined : mock;

// ---------------------------------------------------------------------------
// Events pushed from C++ (see GriddyEditor::timerCallback)
// ---------------------------------------------------------------------------
export function onEvent<T>(name: "transport" | "preview" | "status", fn: (payload: T) => void): () => void {
  if (isPlugin) {
    const token = juce.backend.addEventListener(name, fn);
    return () => juce.backend.removeEventListener(token);
  }
  return mock.on(name, fn as (p: unknown) => void);
}

// ---------------------------------------------------------------------------
// Native functions (async calls into C++)
// ---------------------------------------------------------------------------
function native<T>(name: string): (...args: unknown[]) => Promise<T> {
  if (isPlugin) return getNativeFunction(name) as (...args: unknown[]) => Promise<T>;
  return (...args) => mock.call(name, args) as Promise<T>;
}

export const api = {
  getDevices: native<Device[]>("getDevices"),
  getSlots: native<string[]>("getSlots"),
  setSlotSource: native<SetSlotResult>("setSlotSource") as (slot: number, id: string) => Promise<SetSlotResult>,
  getStatus: native<Status>("getStatus"),
  revealOutputFolder: native<boolean>("revealOutputFolder"),
  chooseOutputFolder: native<string>("chooseOutputFolder"),
  /** A press of REC or Arm, for the Live script to do in Live. False when the script does not run. */
  requestInLive: native<boolean>("requestInLive") as (action: "record" | "stop" | "arm" | "disarm") => Promise<boolean>,
  /** Film for a recording on this track: file is its number, or -1 for the clip clicked in Live. Returns why not, or "". */
  requestFilm: native<string>("requestFilm") as (file: number) => Promise<string>,
  stopFilm: native<boolean>("stopFilm") as () => Promise<boolean>,
  keepTake: native<boolean>("keepTake") as (slot: number, id: number) => Promise<boolean>,
  retakeTake: native<boolean>("retakeTake") as (slot: number, id: number) => Promise<boolean>,
  restoreTake: native<boolean>("restoreTake") as (slot: number, id: number) => Promise<boolean>,
  setSlip: native<boolean>("setSlip") as (slot: number, id: number, seconds: number) => Promise<boolean>,
  setLayout: native<Layout>("setLayout") as (canvas: number, gap: number, background: string, radius: number) => Promise<Layout>,
  choosePreset: native<boolean>("choosePreset") as (index: number) => Promise<boolean>,
  takePeaks: native<number[]>("takePeaks") as (slot: number, id: number) => Promise<number[]>,
  startExport: native<boolean>("startExport"),
  stopExport: native<boolean>("stopExport"),
  swapCells: native<boolean>("swapCells") as (a: number, b: number) => Promise<boolean>,
  resetSlot: native<boolean>("resetSlot") as (slot: number) => Promise<boolean>,
  newSession: native<boolean>("newSession") as () => Promise<boolean>,
  revealSessionFolder: native<boolean>("revealSessionFolder"),
};

/** URL of the latest preview JPEG. The query string only defeats caching; C++ ignores it. */
export function previewUrl(serial: number): string {
  return isPlugin ? getBackendResourceAddress(`preview.jpg?n=${serial}`) : mock.previewUrl();
}

// ---------------------------------------------------------------------------
// Parameters, kept in sync with the DAW through JUCE's relays. The names match
// ParamIDs in source/PluginProcessor.h.
// ---------------------------------------------------------------------------
export type ChoiceParam = {
  get(): number;
  set(index: number): void;
  choices(): string[];
  subscribe(fn: () => void): () => void;
};

export type IntParam = {
  get(): number;
  set(value: number): void;
  range(): { min: number; max: number };
  subscribe(fn: () => void): () => void;
};

export type BoolParam = {
  get(): boolean;
  set(value: boolean): void;
  subscribe(fn: () => void): () => void;
};

function choiceParam(name: string, fallback: string[], initial: number): ChoiceParam {
  if (!isPlugin) return mock.choice(fallback, initial);
  const s = getComboBoxState(name);
  return {
    get: () => s.getChoiceIndex(),
    set: (i) => s.setChoiceIndex(i),
    choices: () => (s.properties.choices.length ? s.properties.choices : fallback),
    subscribe: (fn) => {
      const a = s.valueChangedEvent.addListener(fn);
      const b = s.propertiesChangedEvent.addListener(fn);
      return () => {
        s.valueChangedEvent.removeListener(a);
        s.propertiesChangedEvent.removeListener(b);
      };
    },
  };
}

function intParam(name: string, min: number, max: number, initial: number): IntParam {
  if (!isPlugin) return mock.int(min, max, initial);
  const s = getSliderState(name);
  const range = () => ({ min: s.properties.start, max: s.properties.end });
  return {
    get: () => Math.round(s.getScaledValue()),
    set: (v) => {
      const r = range();
      // Relays take a normalised 0..1 value; an int parameter maps it back to a whole number.
      s.setNormalisedValue((v - r.min) / Math.max(1, r.max - r.min));
    },
    range,
    subscribe: (fn) => {
      const a = s.valueChangedEvent.addListener(fn);
      const b = s.propertiesChangedEvent.addListener(fn);
      return () => {
        s.valueChangedEvent.removeListener(a);
        s.propertiesChangedEvent.removeListener(b);
      };
    },
  };
}

function boolParam(name: string): BoolParam {
  if (!isPlugin) return mock.bool();
  const s = getToggleState(name);
  return {
    get: () => s.getValue(),
    set: (v) => s.setValue(v),
    subscribe: (fn) => {
      const a = s.valueChangedEvent.addListener(fn);
      return () => s.valueChangedEvent.removeListener(a);
    },
  };
}

export const params = {
  slot: intParam("slot", 1, 9, 1),
  master: boolParam("master"),
  numSlots: intParam("numSlots", 1, 9, 4),
  recordArm: boolParam("recordArm"),
};

