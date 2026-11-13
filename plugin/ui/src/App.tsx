// Griddy's window, laid out like the Overdub Cam design (docs/design/overdub-cam.html): header with the transport,
// the grid with its takes timeline on the left, the taped inspector on the right.
import { useEffect, useRef, useState } from "react";
import { api, Status, Transport, params } from "./bridge";
import { AudioTab } from "./AudioTab";
import { Canvas } from "./Canvas";
import { ExportSheet } from "./ExportSheet";
import { useBackendEvent, useParam } from "./hooks";
import { barBeat, Btn, C, Sticker } from "./kit";
import { LookTab } from "./LookTab";
import { slotViews } from "./model";
import { SetupTab } from "./SetupTab";
import { SlotTab } from "./SlotTab";
import { Timeline } from "./Timeline";
import { DevStatePicker } from "./mockStates";

const initialTransport: Transport = {
  bpm: 120, ppq: 0, bar: 1, beat: 1, timeSig: "4/4", playing: false, recording: false, hostSync: false,
};

const initialStatus: Status = {
  outputFolder: "", lastTake: "", lastError: "", fps: 0,
  camerasAllowed: true, cameraBlockedReason: "", outputSize: "",
};

type Tab = "slot" | "look" | "audio" | "setup";
const TABS: [Tab, string, string][] = [["slot", "Slot", C.hot], ["look", "Look", C.sky], ["audio", "Audio", C.pink], ["setup", "Setup", C.gold]];

export function App() {
  const transport = useBackendEvent<Transport>("transport", initialTransport);
  const status = useBackendEvent<Status>("status", initialStatus);
  const engine = status.engine;
  const armed = useParam(params.recordArm);
  const master = !!engine?.master;
  const own = engine && !master ? engine.slot : -1;

  const [selected, setSelected] = useState(0);
  const [tab, setTab] = useState<Tab>("slot");
  const [toast, setToast] = useState<string | null>(null);
  const [exportOpen, setExportOpen] = useState(false);
  const toastTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);

  const say = (text: string) => {
    setToast(text);
    clearTimeout(toastTimer.current);
    toastTimer.current = setTimeout(() => setToast(null), 2200);
  };

  const views = slotViews(engine, transport);
  const sel = Math.min(selected, Math.max(0, views.length - 1));
  const view = views[sel];

  // A track instance looks at its own slot, and follows it when the slot changes.
  useEffect(() => {
    if (own >= 0) setSelected(own);
  }, [own]);

  // A new take waiting in review: say so once.
  const seenReview = useRef(new Map<number, number>());
  useEffect(() => {
    for (const v of views) {
      const id = v.review?.id ?? -1;
      const before = seenReview.current.get(v.slot);
      if (before !== undefined && id >= 0 && id !== before) say(`Slot ${v.slot + 1} recorded. Keep it or retake it.`);
      seenReview.current.set(v.slot, id);
    }
  });

  const keep = async (slot: number) => {
    const t = views[slot]?.review;
    if (t && (await api.keepTake(slot, t.id))) say(`Slot ${slot + 1} approved`);
  };
  const retake = async (slot: number) => {
    const t = views[slot]?.review;
    if (t && (await api.retakeTake(slot, t.id))) say(`Take discarded. Slot ${slot + 1} is live again.`);
  };
  const nudge = (slot: number, ms: number) => {
    const t = views[slot]?.review ?? views[slot]?.shown;
    if (t) void api.setSlip(slot, t.id, Math.max(-0.5, Math.min(0.5, Math.round((t.slip * 1000 + ms) * 10) / 10000)));
  };

  const select = (slot: number) => {
    const v = views[slot];
    if (!v) return;
    if (!master && !v.mine) {
      if (!v.claimed) {
        if (engine?.recording) {
          say("Stop recording before you pick another slot");
          return;
        }
        params.slot.set(slot + 1); // claim it for this track
      } else {
        say(`Slot ${slot + 1} belongs to ${v.name}`);
      }
    }
    if (slot !== sel) setTab("slot");
    setSelected(slot);
  };

  // R arms this track's slot, K keeps the take in review. Space stays with Live.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const tag = (e.target as HTMLElement | null)?.tagName;
      if (tag === "INPUT" || tag === "SELECT" || tag === "TEXTAREA" || e.metaKey || e.ctrlKey || e.altKey) return;
      if ((e.key === "r" || e.key === "R") && !master && engine) params.recordArm.set(!params.recordArm.get());
      if ((e.key === "k" || e.key === "K") && view?.review) void keep(sel);
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  });

  const ownView = own >= 0 ? views[own] : undefined;
  const isRec = !!ownView && (ownView.recording || ownView.waiting);
  const latency = view?.shown?.latencyMs;
  const showLoop = !!transport.hasLoop;

  return (
    <div className="app">
      <header className="top">
        <div className="logo">
          <h1>Griddy</h1>
          <span className="logo-sticker"><Sticker color={C.gold} rotate={5} size={11}>OVERDUB CAM</Sticker></span>
        </div>

        <div className="transport" role="group" aria-label="Transport, from Live">
          <Btn color="ink" disabled label="Stop" title="Use Live's transport">■</Btn>
          <Btn color={transport.playing ? "lime" : "bone"} disabled pressed={transport.playing} label="Playing in Live" title="Use Live's transport">
            {transport.playing ? "❚❚" : "▶"}
          </Btn>
          {!master && engine && (
            <Btn color="blood" onClick={() => params.recordArm.set(!armed)} pressed={armed} className={isRec ? "rec-pulse" : ""}
              title="Arm this track's slot. Griddy records when Live does (R)">
              {isRec ? "● Stop rec" : "● Rec"}
            </Btn>
          )}
          {showLoop && (
            <Btn small color={transport.looping ? "hot" : "ink"} pressed={!!transport.looping} disabled title="Live's loop. Use Live's transport">Loop</Btn>
          )}
        </div>

        <div className="counter">
          <div className="counter-num" aria-label="Bar and beat">{barBeat(transport.ppq, transport.timeSig)}</div>
          <div style={{ paddingBottom: 4 }}>
            <span className="counter-chip">{Math.round(transport.bpm)} BPM · {transport.timeSig}</span>
          </div>
        </div>

        <div className="top-right">
          <Btn color="gold" onClick={() => setExportOpen(true)} disabled={!engine}>Export MP4</Btn>
        </div>
      </header>

      {!status.camerasAllowed && status.cameraBlockedReason && <div className="banner">{status.cameraBlockedReason}</div>}

      <main className="board">
        <section className="stage">
          {engine && engine.layout?.cells ? (
            <Canvas layout={engine.layout} views={views} selected={sel} transport={transport} master={master}
              onSelect={select} onKeep={keep} onRetake={retake} onNudge={nudge} />
          ) : (
            <div className="stage-area" style={{ display: "flex", alignItems: "center", justifyContent: "center" }}>
              <Sticker color={C.paper} rotate={-1} size={16}>Starting Griddy…</Sticker>
            </div>
          )}
          <div className="chips-row">
            {!!engine?.recordHint && <span className="chip-dark" role="status">{engine.recordHint}</span>}
            {latency !== undefined && Math.round(latency) !== 0 && (
              <span className="chip-dark" title="The camera's delay Griddy took off this take, before your slip">
                Latency comp auto · {latency < 0 ? "−" : "+"}{Math.abs(latency).toFixed(1)} ms
              </span>
            )}
          </div>
          {(status.lastError || engine?.error) && <p className="error-line">{status.lastError || engine?.error}</p>}
          {engine && <Timeline views={views} transport={transport} selected={sel} onSelect={select} />}
        </section>

        <aside className="inspector">
          <div className="paper tape inspector-card">
            <div role="tablist" aria-label="Inspector" className="tabs">
              {TABS.map(([id, text, col]) => (
                <button
                  key={id}
                  id={`tab-${id}`}
                  role="tab"
                  aria-selected={tab === id}
                  aria-controls="panel"
                  type="button"
                  onClick={() => setTab(id)}
                  className="btn tab"
                  style={{ background: tab === id ? col : "#e7e2d3", transform: tab === id ? "skewX(-8deg) translateY(-2px)" : undefined, boxShadow: tab === id ? "4px 4px 0 #000" : "2px 2px 0 #000" }}
                >
                  <span>{text}</span>
                </button>
              ))}
            </div>
            <div id="panel" role="tabpanel" aria-labelledby={`tab-${tab}`}>
              {!engine && <p className="msg">Waiting for Griddy's engine.</p>}
              {engine && tab === "slot" && (
                <SlotTab engine={engine} views={views} slot={sel} camerasAllowed={status.camerasAllowed}
                  onKeep={keep} onRetake={retake} onReset={(s) => say(`Slot ${s + 1} reset`)} />
              )}
              {engine && tab === "look" && <LookTab engine={engine} say={say} />}
              {engine && tab === "audio" && <AudioTab views={views} transport={transport} />}
              {engine && tab === "setup" && <SetupTab engine={engine} status={status} views={views} say={say} />}
            </div>
          </div>
        </aside>
      </main>

      {toast && (
        <div className="toast" role="status" style={{ position: "fixed", left: "50%", bottom: 24, transform: "translateX(-50%)", zIndex: 40 }}>
          <Sticker color={C.paper} rotate={-1} size={16} style={{ padding: "8px 12px" }}>{toast}</Sticker>
        </div>
      )}
      {exportOpen && engine && <ExportSheet engine={engine} onClose={() => setExportOpen(false)} />}
      <DevStatePicker />
    </div>
  );
}
