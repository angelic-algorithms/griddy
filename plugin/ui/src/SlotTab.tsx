// The inspector's Slot tab: the target slot, its track and camera, Keep and Retake, trim and slip, its takes.
import { useEffect, useState } from "react";
import { api, Device, EngineStatus, params, SlotInfo, TakeInfo } from "./bridge";
import { useParam } from "./hooks";
import { Btn, C, fmtFrames, fmtMs, Label, Sticker } from "./kit";
import { FOLLOWS_POSITION, SlotView } from "./model";
import { ResetSlot } from "./Recover";

const MESSAGES = {
  review: "Fresh take. Watch it back, fix the timing, then keep it or go again.",
  approved: "Approved and playing in time. Hit REC to overwrite it. The old take stays muted until you keep the new one.",
  empty: "Nothing here yet. Arm this slot and record in Live. Griddy records the picture with it.",
};

const syncText: Record<SlotInfo["status"], string> = {
  noTake: "No take yet",
  stopped: "Stopped",
  prior: "Following the playhead",
  locked: "Locked to your audio",
  held: "Holding the last frame",
  lost: "Audio does not match",
};

const sourceText: Record<SlotInfo["source"], string> = { none: "", hostPosition: "playhead", fingerprint: "fingerprint", embedded: "sync signal" };

/** How a slot is following the DAW, as a sticker. */
export function SyncChip({ slot }: { slot: SlotInfo | undefined }) {
  if (!slot) return null;
  const warn = slot.status === "lost" || slot.warp;
  return (
    <span title="How this slot is following your DAW">
      <Sticker color={slot.status === "locked" ? C.lime : warn ? C.blood : "#fff"} text={warn ? "#fff" : C.ink} rotate={-1} size={13}>
        {syncText[slot.status]}
        {slot.status === "locked" && sourceText[slot.source] ? ` by ${sourceText[slot.source]}` : ""}
        {slot.warp && " · warped audio?"}
      </Sticker>
    </span>
  );
}

const stateWords: Record<TakeInfo["state"], string> = { review: "REVIEW", kept: "✓ APPROVED", archived: "ARCHIVED" };
const stateColor: Record<TakeInfo["state"], string> = { review: C.gold, kept: C.lime, archived: "#d8d6cc" };

/** Takes listed before "Show all". */
const LISTED = 5;

function seconds(s: number) {
  const m = Math.floor(s / 60);
  return `${m}:${(s - m * 60).toFixed(1).padStart(4, "0")}`;
}

function TakeRow({ slot, take, playing, track, onKeep, onRetake }: { slot: number; take: TakeInfo; playing: boolean; track: string; onKeep: () => void; onRetake: () => void }) {
  const nudge = (ms: number) => api.setSlip(slot, take.id, Math.max(-0.5, Math.min(0.5, take.slip + ms / 1000)));
  return (
    <li className="take-row stack" style={{ gap: 6 }}>
      <div className="row" style={{ gap: 8 }}>
        <span style={{ fontWeight: 800, fontSize: 15, textTransform: "uppercase" }}>Take {take.id}</span>
        <span className="small-note num">{seconds(take.seconds)}</span>
        {playing && (
          <span role="status" aria-label={`Slot ${slot + 1} is showing take ${take.id}`} style={{ fontWeight: 800, fontSize: 11, textTransform: "uppercase", padding: "2px 5px", lineHeight: 1, border: "2px solid #000", background: C.ink, color: C.paper }}>
            Playing
          </span>
        )}
        <span style={{ marginLeft: "auto", fontWeight: 800, fontSize: 12, textTransform: "uppercase", padding: "3px 6px", lineHeight: 1, border: "2px solid #000", background: stateColor[take.state], boxShadow: "2px 2px 0 #000" }}>
          {stateWords[take.state]}
        </span>
      </div>
      {take.recognised === false && <span className="small-note">Too old to follow comps</span>}
      {!!take.heard && take.heard !== track && <span className="small-note">This take listened to {take.heard}. It follows edits on that track.</span>}
      <div className="row" style={{ gap: 8, flexWrap: "wrap" }}>
        {take.state === "review" && (
          <>
            <Btn small color="lime" onClick={onKeep}>✓ Keep</Btn>
            <Btn small color="blood" onClick={onRetake}>↺ Retake</Btn>
          </>
        )}
        {take.state === "archived" && <Btn small color="sky" onClick={() => api.restoreTake(slot, take.id)}>Restore</Btn>}
        {take.state !== "archived" && (
          <div role="group" aria-label="Slip" className="row" style={{ gap: 2, marginLeft: "auto", background: "#2b2b31", border: "2px solid #000", padding: "0 4px" }}>
            <button type="button" className="nudge" onClick={() => nudge(-10)} aria-label="Picture 10 ms earlier">◀</button>
            <span className="num" style={{ fontWeight: 800, fontSize: 13, color: C.sky, minWidth: 56, textAlign: "center" }}>{fmtMs(take.slip * 1000)}</span>
            <button type="button" className="nudge" onClick={() => nudge(10)} aria-label="Picture 10 ms later">▶</button>
          </div>
        )}
      </div>
    </li>
  );
}

/** Which camera fills a slot, as camera pads. Clicking the camera that is on turns it off. */
function CameraPicker({ slot, camerasAllowed }: { slot: number; camerasAllowed: boolean }) {
  const [devices, setDevices] = useState<Device[]>([]);
  const [sources, setSources] = useState<string[]>([]);
  const [error, setError] = useState("");

  const refresh = async () => {
    const [d, s] = await Promise.all([api.getDevices(), api.getSlots()]);
    setDevices(d);
    setSources(s);
  };
  useEffect(() => void refresh(), []);

  const current = sources[slot] ?? "";
  const pick = async (id: string) => {
    const r = await api.setSlotSource(slot, id === current ? "" : id);
    setSources(r.slots);
    setError(r.ok ? "" : r.error);
  };

  return (
    <div>
      <Label>Camera</Label>
      <div role="group" aria-label={`Camera for slot ${slot + 1}`} className="row" style={{ gap: 8, flexWrap: "wrap" }}>
        {devices.map((d) => (
          <Btn key={d.id} small color={d.id === current ? "lime" : "bone"} pressed={d.id === current} onClick={() => pick(d.id)}
            title={d.id === current ? `${d.name} is on. Click to turn it off.` : `Use ${d.name}`}>
            {d.name}
          </Btn>
        ))}
      </div>
      <div className="row" style={{ justifyContent: "space-between", marginTop: 8, gap: 8 }}>
        <span className="small-note">
          {!camerasAllowed ? "Cameras are blocked here. See the message at the top." : devices.length === 0 ? "No cameras found." : current === "" ? "Camera off." : ""}
        </span>
        <button type="button" className="link" onClick={refresh} title="Look for newly plugged in cameras">Rescan cameras</button>
      </div>
      {error && <p className="error-line">{error}</p>}
    </div>
  );
}

/** In a track instance: which slot this track records into. Picking a slot another track holds swaps the two. */
function SlotPicker({ views }: { views: SlotView[] }) {
  const value = useParam(params.slot);
  return (
    <select id="slot-track" className="select" value={value} onChange={(e) => params.slot.set(Number(e.target.value))} aria-label="This track's slot">
      {Array.from({ length: 9 }, (_, i) => {
        const v = views[i];
        const held = v && v.claimed && !v.mine;
        return <option key={i} value={i + 1}>Slot {i + 1}{held ? `  (${v.name}, in use, will swap)` : ""}</option>;
      })}
    </select>
  );
}

export function SlotTab({ engine, views, slot, camerasAllowed, onKeep, onRetake, onReset }: {
  engine: EngineStatus;
  views: SlotView[];
  slot: number;
  camerasAllowed: boolean;
  onKeep: (slot: number) => void;
  onRetake: (slot: number) => void;
  onReset: (slot: number) => void;
}) {
  const [showAll, setShowAll] = useState(false);
  const v = views[slot];
  if (!v) return null;
  const take = v.shown;
  const fps = take?.fps || 30;
  const frameMs = 1000 / fps;
  const msg = v.review ? MESSAGES.review : v.kept ? MESSAGES.approved : MESSAGES.empty;
  const nudge = (ms: number) => take && api.setSlip(slot, take.id, Math.max(-0.5, Math.min(0.5, Math.round((take.slip * 1000 + ms) * 10) / 10000)));

  return (
    <div className="stack" style={{ gap: 16 }}>
      <div>
        <div className="row" style={{ gap: 8, marginBottom: 8 }}>
          <span className="f-sign" style={{ fontSize: 18, lineHeight: 1, padding: "6px 8px", border: "3px solid #000", background: v.color, boxShadow: "3px 3px 0 #000" }}>{slot + 1}</span>
          <div style={{ fontWeight: 800, textTransform: "uppercase", fontSize: 20, lineHeight: 1 }}>Slot {slot + 1} is the target</div>
        </div>
        <p className="msg">{msg}</p>
      </div>

      <div>
        <Label>Assigned track</Label>
        {v.mine ? <SlotPicker views={views} /> : (
          <div className="select" aria-readonly="true" title={engine.master ? "Each track picks its own slot in its own Griddy" : "Another track holds this slot"}>
            {v.claimed ? v.name : "No track"}
          </div>
        )}
      </div>

      <CameraPicker slot={slot} camerasAllowed={camerasAllowed} />

      {v.review && (
        <div className="row" style={{ gap: 8 }}>
          <Btn color="lime" onClick={() => onKeep(slot)} className="grow" style={{ flex: 1 }} title="Keep this take (K)">✓ Keep take</Btn>
          <Btn color="blood" onClick={() => onRetake(slot)} style={{ flex: 1 }}>↺ Retake</Btn>
        </div>
      )}

      {take && (
        <div className="box">
          <div className="row" style={{ alignItems: "baseline", justifyContent: "space-between", marginBottom: 8 }}>
            <Label>Trim and slip</Label>
            <span className="num" style={{ fontWeight: 800, fontSize: 18, lineHeight: 1 }}>
              {fmtMs(take.slip * 1000)} <span style={{ color: "#5a564b", fontSize: 14 }}>· {fmtFrames(take.slip * 1000, fps)}</span>
            </span>
          </div>
          <div style={{ display: "grid", gridTemplateColumns: "repeat(4,1fr)", gap: 6 }}>
            <Btn small color="ink" onClick={() => nudge(-10)} label="Slip earlier by 10 milliseconds">−10 ms</Btn>
            <Btn small color="sky" onClick={() => nudge(-frameMs)} label="Slip earlier by one frame">−1 f</Btn>
            <Btn small color="sky" onClick={() => nudge(frameMs)} label="Slip later by one frame">+1 f</Btn>
            <Btn small color="ink" onClick={() => nudge(10)} label="Slip later by 10 milliseconds">+10 ms</Btn>
          </div>
          <div className="row" style={{ justifyContent: "space-between", marginTop: 8 }}>
            <span className="small-note">One frame is {frameMs.toFixed(1)} ms at {Math.round(fps)} fps</span>
            <button type="button" className="link" onClick={() => api.setSlip(slot, take.id, 0)}>Reset</button>
          </div>
        </div>
      )}

      {v.kept && !v.review && (
        <div style={{ alignSelf: "flex-start" }}>
          <ResetSlot slot={slot} pending={engine.resetPending && v.mine} label="Clear this slot" onDone={() => onReset(slot)} />
        </div>
      )}

      <div>
        <div className="row" style={{ justifyContent: "space-between", marginBottom: 6 }}>
          <Label>Takes in slot {slot + 1}</Label>
          <SyncChip slot={v.info} />
        </div>
        {!v.follows && v.takes.length > 0 && <p className="small-note" style={{ margin: "0 0 6px" }}>{FOLLOWS_POSITION}</p>}
        {v.takes.length === 0 ? (
          <p className="small-note" style={{ margin: 0 }}>Arm, then record in Live. Each take lands here for you to keep or redo.</p>
        ) : (
          <ul style={{ listStyle: "none", margin: 0, padding: 0 }}>
            {(showAll ? v.takes : v.takes.slice(0, LISTED)).map((t) => (
              <TakeRow key={t.id} slot={slot} take={t} playing={v.playing === t.id} track={v.info?.trackName ?? ""} onKeep={() => onKeep(slot)} onRetake={() => onRetake(slot)} />
            ))}
          </ul>
        )}
        {v.takes.length > LISTED && (
          <button type="button" className="link" style={{ marginTop: 6 }} onClick={() => setShowAll(!showAll)}>
            {showAll ? "Show fewer" : `Show all ${v.takes.length}`}
          </button>
        )}
        {v.mine && (engine.framesDropped ?? 0) > 0 && <p className="error-line">{engine.framesDropped} camera frames were dropped in the last take.</p>}
        {!v.kept && (
          <div style={{ marginTop: 10 }}>
            <ResetSlot slot={slot} pending={engine.resetPending && v.mine} onDone={() => onReset(slot)} />
          </div>
        )}
      </div>
    </div>
  );
}
