// A fake backend so the UI runs in a normal browser while you style it.
// It pretends a DAW is playing at 120 BPM and draws a stand in preview.

type Listener = (payload: unknown) => void;

export function createMock() {
  const listeners = new Map<string, Set<Listener>>();
  const emit = (name: string, payload: unknown) => listeners.get(name)?.forEach((fn) => fn(payload));

  const devices = [
    { id: "test:1", name: "Test pattern 1", kind: "test" },
    { id: "test:2", name: "Test pattern 2", kind: "test" },
    { id: "test:3", name: "Test pattern 3", kind: "test" },
    { id: "test:4", name: "Test pattern 4", kind: "test" },
    { id: "av:mock-facetime", name: "FaceTime HD Camera (mock)", kind: "camera" },
    { id: "av:mock-iphone", name: "iPhone Camera (mock)", kind: "camera" },
  ];
  let slots = ["test:1", "test:2", "test:3", "test:4", "", "", "", "", ""];
  let outputFolder = "/Users/you/Movies/Griddy";
  let serial = 0;
  const start = performance.now();
  const canvas = typeof document !== "undefined" ? document.createElement("canvas") : null;
  if (canvas) {
    canvas.width = 640;
    canvas.height = 360;
  }

  // Simple param stores with subscribers, shaped like the real ones in bridge.ts.
  function store<T>(initial: T) {
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

  const mock = {
    on(name: string, fn: Listener) {
      if (!listeners.has(name)) listeners.set(name, new Set());
      listeners.get(name)!.add(fn);
      return () => listeners.get(name)!.delete(fn);
    },

    async call(name: string, args: unknown[]): Promise<unknown> {
      switch (name) {
        case "getDevices":
          return devices;
        case "getSlots":
          return slots;
        case "setSlotSource": {
          const [slot, id] = args as [number, string];
          slots = slots.map((s, i) => (i === slot ? id : s));
          return { ok: true, error: "", slots };
        }
        case "getStatus":
          return status();
        case "chooseOutputFolder":
          outputFolder = "/Users/you/Desktop/Griddy takes";
          return outputFolder;
        default:
          return true;
      }
    },

    previewUrl: () => canvas?.toDataURL("image/jpeg", 0.7) ?? "",

    choice: (choices: string[], initial: number) => {
      const s = store(initial);
      return { ...s, choices: () => choices };
    },
    int: (min: number, max: number, initial: number) => {
      const s = store(initial);
      return { ...s, range: () => ({ min, max }) };
    },
    bool: () => store(false),
  };

  function status() {
    return {
      outputFolder,
      lastTake: "",
      lastError: "",
      recorderFormat: "H.264 QuickTime",
      recording: false,
      fps: 30,
      camerasAllowed: true,
      cameraBlockedReason: "",
      outputSize: "1280x720",
    };
  }

  function drawPreview(seconds: number) {
    const g = canvas?.getContext("2d");
    if (!g || !canvas) return;
    const beat = Math.floor(seconds * 2); // 120 BPM
    g.fillStyle = "#0b0b0f";
    g.fillRect(0, 0, canvas.width, canvas.height);
    for (let i = 0; i < 4; i++) {
      const x = (i % 2) * 320, y = Math.floor(i / 2) * 180;
      g.fillStyle = `hsl(${(i * 97 + 40) % 360} 55% ${beat % 4 === i ? 55 : 35}%)`;
      g.fillRect(x + 2, y + 2, 316, 176);
      g.fillStyle = "rgba(0,0,0,.6)";
      g.font = "bold 64px system-ui";
      g.textAlign = "center";
      g.fillText(String(i + 1), x + 160, y + 112);
    }
  }

  if (typeof window !== "undefined") {
    setInterval(() => {
      const seconds = (performance.now() - start) / 1000;
      const ppq = seconds * 2;
      emit("transport", {
        bpm: 120, ppq, bar: Math.floor(ppq / 4) + 1, beat: Math.floor(ppq % 4) + 1,
        timeSig: "4/4", playing: true, recording: false, hostSync: true,
      });
      drawPreview(seconds);
      emit("preview", ++serial);
    }, 1000 / 15);
    setInterval(() => emit("status", status()), 500);
  }

  return mock;
}
