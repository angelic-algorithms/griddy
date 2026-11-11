import { api, EngineStatus, SlotInfo, TakeInfo } from "./bridge";

const syncText: Record<SlotInfo["status"], string> = {
  noTake: "No take yet",
  stopped: "Stopped",
  prior: "Following the playhead",
  locked: "Locked to your audio",
  held: "Holding the last frame",
  lost: "Audio does not match",
};

const sourceText: Record<SlotInfo["source"], string> = {
  none: "",
  hostPosition: "playhead",
  fingerprint: "fingerprint",
  embedded: "sync signal",
};

function seconds(s: number) {
  const m = Math.floor(s / 60);
  return `${m}:${(s - m * 60).toFixed(1).padStart(4, "0")}`;
}

/** The sync state of this instance's slot, in words. */
export function SyncChip({ slot }: { slot: SlotInfo | undefined }) {
  if (!slot) return null;
  const tone = slot.status === "locked" ? "ok" : slot.status === "lost" || slot.warp ? "warn" : "";
  return (
    <div className={`chip ${tone}`} title="How this slot is following your DAW">
      {syncText[slot.status]}
      {slot.status === "locked" && sourceText[slot.source] ? ` by ${sourceText[slot.source]}` : ""}
      {slot.warp && " · warped audio?"}
    </div>
  );
}

function TakeRow({ slot, take }: { slot: number; take: TakeInfo }) {
  const nudge = (ms: number) => api.setSlip(slot, take.id, take.slip + ms / 1000);
  return (
    <li className={`take ${take.state}`}>
      <div className="take-head">
        <span className="take-name">Take {take.id}</span>
        <span className="muted">{seconds(take.seconds)}</span>
        <span className={`badge ${take.state}`}>{take.state === "review" ? "waiting" : take.state}</span>
      </div>
      <div className="row">
        {take.state === "review" && (
          <>
            <button className="primary" onClick={() => api.keepTake(slot, take.id)}>Keep</button>
            <button onClick={() => api.retakeTake(slot, take.id)}>Retake</button>
          </>
        )}
        {take.state === "archived" && <button onClick={() => api.restoreTake(slot, take.id)}>Restore</button>}
        {take.state !== "archived" && (
          <div className="slip" title="Move the picture earlier or later against the sound">
            <button onClick={() => nudge(-10)} aria-label="Picture 10 ms earlier">−</button>
            <span>{take.slip >= 0 ? "+" : ""}{Math.round(take.slip * 1000)} ms</span>
            <button onClick={() => nudge(10)} aria-label="Picture 10 ms later">+</button>
          </div>
        )}
      </div>
    </li>
  );
}

export function Takes({ engine }: { engine: EngineStatus }) {
  const slot = engine.slot;
  const takes = engine.takes[slot] ?? [];
  const info = engine.slots.find((s) => s.slot === slot);

  return (
    <section className="panel">
      <header className="panel-head">
        <h2>Takes in slot {slot + 1}</h2>
        {engine.recording && <span className="badge live">Recording</span>}
      </header>
      <SyncChip slot={info} />
      {takes.length === 0 ? (
        <p className="note">Arm, then record in your DAW. Each take lands here for you to keep or redo.</p>
      ) : (
        <ul className="takes">
          {takes.map((t) => <TakeRow key={t.id} slot={slot} take={t} />)}
        </ul>
      )}
      {(engine.framesDropped ?? 0) > 0 && <p className="error">{engine.framesDropped} camera frames were dropped in the last take.</p>}
    </section>
  );
}
