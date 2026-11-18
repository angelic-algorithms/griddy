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
  const [toasts, setToasts] = useState<{ id: number; text: string }[]>([]);
  const toastId = useRef(0);
  const [exportOpen, setExportOpen] = useState(false);
  const head = toasts[0];
  const toast = head?.text ?? null;

  // Toasts queue, so "recorded" and "disarmed" after one take are both read, each for 2.2 s.
  const say = (text: string) => setToasts((q) => (q[q.length - 1]?.text === text ? q : [...q, { id: ++toastId.current, text }]));
  useEffect(() => {
    if (!head) return;
    const t = setTimeout(() => setToasts((q) => q.slice(1)), 2200);
    return () => clearTimeout(t);
  }, [head?.id]);

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

  // A take ends the arm (one take per arm). Say so, so the next play playing back is no surprise.
  const wasRecording = useRef(false);
  // A film take arms nothing, so it has no arm to end.
  const ownRecording = own >= 0 && !!engine?.recording && !engine?.film?.filming;
  useEffect(() => {
    if (wasRecording.current && !ownRecording) {
      const t = setTimeout(() => {
        if (!params.recordArm.get()) say(`Slot ${own + 1} disarmed. Press Rec to record again.`);
      }, 600);
      wasRecording.current = false;
      return () => clearTimeout(t);
    }
    wasRecording.current = ownRecording;
  }, [ownRecording, own]);

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
      if ((e.key === "r" || e.key === "R") && !master && engine) pressRec();
      if ((e.key === "k" || e.key === "K") && view?.review) void keep(sel);
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  });

  const ownView = own >= 0 ? views[own] : undefined;
  // A film take is not a recording in Live: REC keeps saying what Live's record does, and Film shows the filming.
  const isRec = !!ownView && (ownView.recording || ownView.waiting) && !ownView.info?.filming;
  const liveScript = !!engine?.liveScript;
  const liveArmed = !!ownView?.info?.liveArmed;

  // With the Live script, REC and Arm ask Live, which answers a tenth of a second or so later. Until it does, each
  // button shows what was asked for; if Live has not followed after 1.5 s, it goes back and says so.
  const [asked, setAsked] = useState<{ arm?: boolean; rec?: boolean; at: number }>({ at: 0 });
  const askedArm = asked.arm !== undefined && asked.arm !== liveArmed ? asked.arm : undefined;
  const askedRec = asked.rec !== undefined && asked.rec !== (isRec || armed) ? asked.rec : undefined;
  // Once Live has followed, forget the press, so a later change made in Live itself shows at once.
  useEffect(() => {
    if (asked.at && askedArm === undefined && askedRec === undefined) setAsked({ at: 0 });
  }, [asked.at, askedArm, askedRec]);
  useEffect(() => {
    if (askedArm === undefined && askedRec === undefined) return;
    const t = setTimeout(() => {
      setAsked({ at: 0 });
      say(askedArm !== undefined
        ? `Live didn't ${askedArm ? "arm" : "disarm"} the track. Check that Griddy is still the Control Surface in Live's settings.`
        : `Live didn't ${askedRec ? "start" : "stop"} recording.`);
    }, 1500);
    return () => clearTimeout(t);
  }, [asked.at, askedArm, askedRec]);

  const ask = async (action: "record" | "stop" | "arm" | "disarm") => {
    if (!(await api.requestInLive(action))) {
      say("Griddy can't reach Live. Set Control Surface to Griddy in Live's settings.");
      return;
    }
    const at = Date.now();
    setAsked(action === "arm" || action === "disarm" ? { arm: action === "arm", at } : { rec: action === "record", at });
  };

  const recDown = askedRec ?? (isRec || (liveScript && armed));
  const armDown = askedArm ?? liveArmed;
  const pressRec = () => {
    if (!liveScript) params.recordArm.set(!params.recordArm.get());
    else if (recDown) void ask("stop");
    else if (!armDown) say("Arm the track first: press Arm, or arm it in Live.");
    else void ask("record");
  };
  const pressArm = () => void ask(armDown ? "disarm" : "arm");

  // Film: a video for a recording already in Live. Live plays it from a bar before and records nothing.
  const film = engine?.film;
  const filmOn = !!film?.armed || !!film?.filming;
  const pressFilm = async (file = -1) => {
    if (filmOn) {
      await api.stopFilm();
      return;
    }
    const why = await api.requestFilm(file);
    if (why) say(why);
  };
  const filmFor = film?.selected;

  // The hint can be true for one status update between Live's record and play; show it only once it holds.
  const hint = engine?.recordHint ?? "";
  const [shownHint, setShownHint] = useState("");
  useEffect(() => {
    if (!hint) {
      setShownHint("");
      return;
    }
    const t = setTimeout(() => setShownHint(hint), 500);
    return () => clearTimeout(t);
  }, [hint]);
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
          {!master && engine && liveScript && (
            <Btn color={armDown ? "hot" : "bone"} onClick={pressArm} pressed={armDown}
              title={armDown ? `Disarm ${ownView?.name ?? "this track"} in Live` : `Arm ${ownView?.name ?? "this track"} in Live, so Rec can record it`}>
              {armDown ? "Armed" : "Arm"}
            </Btn>
          )}
          {!master && engine && !liveScript && (
            <Btn color="blood" onClick={pressRec} pressed={armed} className={isRec ? "rec-pulse" : ""}
              title={`Arm slot ${own + 1}. The next play records one take. (R)`}>
              {isRec ? "● Stop rec" : "● Rec"}
            </Btn>
          )}
          {!master && engine && liveScript && (
            <Btn color="blood" onClick={pressRec} pressed={recDown} className={`${recDown ? "rec-pulse" : ""} ${!armDown && !recDown ? "btn-dim" : ""}`}
              title={recDown ? "Stop recording in Live (R)" : armDown ? "Record in Live, and play if Live is stopped (R)" : "Arm the track first"}>
              {recDown ? "● Stop rec" : "● Rec"}
            </Btn>
          )}
          {!master && engine && liveScript && film?.available && (filmFor || filmOn) && (
            <Btn color="sky" onClick={() => void pressFilm()} pressed={filmOn} className={filmOn ? "rec-pulse" : ""}
              title={filmOn ? "Stop filming, and stop Live" : `Film a video for ${filmFor?.name}. Live plays it from a bar before and records no sound`}>
              {filmOn ? "■ Stop film" : "● Film"}
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
            {!!shownHint && <span className="chip-dark" role="status">{shownHint}</span>}
            {!master && filmOn && !!film?.name && <span className="chip-dark" role="status">Filming for {film.name}</span>}
            {!master && !filmOn && filmFor && (
              <span className="chip-dark" role="status" title={filmFor.name}>
                Film for: {ownView?.name ?? "this track"}, bar {filmFor.bar ?? 1}{filmFor.lane ? ", take lane" : ""}
                {filmFor.video !== "none" ? (filmFor.video === "filmed" ? " · filmed" : " · has video") : ""}
              </span>
            )}
            {!master && filmFor?.lane && <span className="chip-dark" role="note">Audition this lane (T) to hear it</span>}
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
                  onKeep={keep} onRetake={retake} onReset={(s) => say(`Slot ${s + 1} reset`)} onFilm={(f) => void pressFilm(f)} />
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
