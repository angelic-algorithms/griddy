// The inspector's Setup tab: what the plugin needs that the Overdub Cam design has no place for.
import { api, EngineStatus, params, Status } from "./bridge";
import { useParam } from "./hooks";
import { Btn, Label } from "./kit";
import { SlotView } from "./model";
import { AskFirst, ResetSlot } from "./Recover";
import { SyncChip } from "./SlotTab";

function MasterToggle() {
  const on = useParam(params.master);
  return (
    <Btn color={on ? "hot" : "bone"} pressed={on} onClick={() => params.master.set(!on)}
      title="Master: arrange the grid and see every slot. A master has no slot of its own.">
      {on ? "Master view" : "Master"}
    </Btn>
  );
}

function RecordWhen() {
  const index = useParam(params.recordMode);
  const labels = ["DAW records", "DAW plays", "Armed only"];
  return (
    <div className="row" role="radiogroup" aria-label="Record when" style={{ gap: 8, flexWrap: "wrap" }}>
      {params.recordMode.choices().map((choice, i) => (
        <Btn key={choice} small color={i === index ? "lime" : "bone"} pressed={i === index} onClick={() => params.recordMode.set(i)}>
          {labels[i] ?? choice}
        </Btn>
      ))}
    </div>
  );
}

export function SetupTab({ engine, status, views, say }: { engine: EngineStatus; status: Status; views: SlotView[]; say: (t: string) => void }) {
  return (
    <div className="stack" style={{ gap: 20 }}>
      <div>
        <Label>This Griddy</Label>
        <div className="row" style={{ gap: 10, flexWrap: "wrap" }}>
          <MasterToggle />
          <span className="small-note">{engine.instances} {engine.instances === 1 ? "track" : "tracks"} in this set</span>
        </div>
      </div>

      {!engine.master && (
        <div>
          <Label>Record when</Label>
          <RecordWhen />
        </div>
      )}

      <div>
        <Label>Session</Label>
        <p className="path" title={engine.sessionFolder}>{engine.session}</p>
        <p className="small-note" style={{ margin: "4px 0 8px" }}>Takes for this set are saved in this folder. Saving your set keeps it; a new set or a template starts its own.</p>
        <div className="row" style={{ gap: 8, flexWrap: "wrap" }}>
          <Btn small onClick={() => api.revealSessionFolder()}>Show folder</Btn>
          <AskFirst
            label="New session"
            small
            disabled={engine.recording}
            question="Start an empty session for this set? Every slot goes back to its live camera. Nothing is deleted: the current session folder stays in Sessions with all its takes."
            confirmLabel="Start new session"
            onConfirm={async () => {
              const ok = await api.newSession();
              if (ok) say("New session started");
              return ok;
            }}
          />
        </div>
      </div>

      <div>
        <Label>Takes folder</Label>
        <p className="path" title={status.outputFolder}>{status.outputFolder || "…"}</p>
        {!!engine.pendingFolder && <p className="path">Moves to {engine.pendingFolder} when this take ends.</p>}
        <p className="small-note" style={{ margin: "4px 0 8px" }}>Every finished take is also saved as a plain .mov in the Takes folder inside it.</p>
        <div className="row" style={{ gap: 8 }}>
          <Btn small onClick={() => api.chooseOutputFolder()}>Choose…</Btn>
          <Btn small onClick={() => api.revealOutputFolder()}>Show takes</Btn>
        </div>
      </div>

      {engine.master && (
        <div>
          <Label>Every slot</Label>
          <ul className="stack" style={{ listStyle: "none", margin: 0, padding: 0, gap: 8 }}>
            {views.map((v) => (
              <li key={v.slot} className="row" style={{ gap: 8, flexWrap: "wrap" }}>
                <span className="f-sign" style={{ fontSize: 11, lineHeight: 1, padding: "3px 4px", border: "2px solid #000", background: v.color }}>{v.slot + 1}</span>
                <span className="truncate" style={{ fontWeight: 700, textTransform: "uppercase", fontSize: 13, maxWidth: 110 }}>{v.claimed ? v.name : "No track"}</span>
                <SyncChip slot={v.info} />
                <span style={{ marginLeft: "auto" }}><ResetSlot slot={v.slot} onDone={() => say(`Slot ${v.slot + 1} reset`)} /></span>
              </li>
            ))}
          </ul>
        </div>
      )}
    </div>
  );
}
