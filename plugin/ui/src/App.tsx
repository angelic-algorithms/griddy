import { useState } from "react";
import { api, params, Status, Transport } from "./bridge";
import { LayoutPanel } from "./Layout";
import { ExportPanel } from "./Export";
import { SyncChip, Takes } from "./Takes";
import { Controls, RecordWhen } from "./Controls";
import { useBackendEvent, useParam } from "./hooks";
import { Preview } from "./Preview";
import { ResetSlot, SessionPanel } from "./Recover";
import { CameraPads } from "./Slots";

const initialTransport: Transport = {
  bpm: 120, ppq: 0, bar: 1, beat: 1, timeSig: "4/4", playing: false, recording: false, hostSync: false,
};

const initialStatus: Status = {
  outputFolder: "", lastTake: "", lastError: "", fps: 0,
  camerasAllowed: true, cameraBlockedReason: "", outputSize: "",
};

function clock(t: Transport) {
  const seconds = Math.max(0, Math.floor((t.ppq * 60) / Math.max(1, t.bpm)));
  return `${String(Math.floor(seconds / 60)).padStart(2, "0")}:${String(seconds % 60).padStart(2, "0")}`;
}

function ArmButton({ recording }: { recording: boolean }) {
  const armed = useParam(params.recordArm);
  const label = recording ? "Recording" : armed ? "Armed" : "Arm";
  return (
    <button
      className={`tape-btn arm ${armed ? "armed" : ""} ${recording ? "live" : ""}`}
      onClick={() => params.recordArm.set(!armed)}
      aria-pressed={armed}
    >
      {label}
    </button>
  );
}

function SlotPicker({ slot, instances }: { slot: number; instances: number }) {
  const value = useParam(params.slot);
  const { min, max } = params.slot.range();
  return (
    <div className="field">
      <span className="field-label">This track's cell</span>
      <div className="stepper">
        <button onClick={() => params.slot.set(Math.max(min, value - 1))} aria-label="Earlier cell">−</button>
        <span>{slot >= 0 ? slot + 1 : value}</span>
        <button onClick={() => params.slot.set(Math.min(max, value + 1))} aria-label="Later cell">+</button>
        <span className="muted">{instances} {instances === 1 ? "track" : "tracks"} in this set</span>
      </div>
    </div>
  );
}

function MasterToggle() {
  const on = useParam(params.master);
  return (
    <button className={`tape-btn ${on ? "on" : ""}`} aria-pressed={on} onClick={() => params.master.set(!on)}
      title="Master: arrange the grid and see every slot. A master has no cell of its own.">
      {on ? "Master view" : "Master"}
    </button>
  );
}

export function App() {
  const transport = useBackendEvent<Transport>("transport", initialTransport);
  const status = useBackendEvent<Status>("status", initialStatus);
  const [open, setOpen] = useState(false);
  const engine = status.engine;
  const recording = !!engine?.recording;
  const cells = engine ? Math.max(1, engine.slots.length) : 4;
  const tag = recording ? `Cell ${(engine?.slot ?? 0) + 1} recording` : engine?.master ? "Master view" : `Grid · ${cells} cells`;

  return (
    <div className="app">
      <header className="topbar">
        <div className="logo">
          <h1>Griddy</h1>
          <span className="logo-tag">Park Angel</span>
        </div>
        <nav className="modes" aria-label="Modes">
          <button className="mode on">Grid</button>
          <button className="mode" disabled title="Not built yet">Cut</button>
          <button className="mode" disabled title="Not built yet">Spotlight</button>
        </nav>
        <div className="top-right">
          <MasterToggle />
          {!engine?.master && <ArmButton recording={recording} />}
          <div className={`rec-badge ${recording ? "live" : ""}`}>REC</div>
          <span className="clock">{clock(transport)}</span>
        </div>
      </header>

      {!status.camerasAllowed && status.cameraBlockedReason && (
        <div className="banner">{status.cameraBlockedReason}</div>
      )}

      <main className="board">
        <aside className="left">
          <CameraPads ownSlot={engine?.slot ?? -1} camerasAllowed={status.camerasAllowed} />
        </aside>

        <section className="stage">
          <div className="frame">
            <span className="tape t1" />
            <span className="tape t2" />
            <Preview />
            <span className="live-tag">{tag}</span>
            <div className="counter">
              <div className="counter-num">{transport.bar}.{transport.beat}</div>
              <div className="counter-sub">{transport.bpm.toFixed(0)} BPM · {transport.timeSig}</div>
            </div>
          </div>
          {status.lastError && <p className="error">{status.lastError}</p>}
        </section>

        {open && (
          <aside className="drawer">
            {engine && !engine.master && (
              <>
                <section className="panel">
                  <h2>This track</h2>
                  <SlotPicker slot={engine.slot} instances={engine.instances} />
                </section>
                <Takes engine={engine} />
              </>
            )}
            {engine?.master && (
              <>
                <section className="panel">
                  <h2>Every slot</h2>
                  <ul className="slotlist">
                    {engine.slots.map((s) => (
                      <li key={s.slot}>
                        <span className="slot-num">{s.slot + 1}</span>
                        <SyncChip slot={s} />
                        {!s.claimed && <span className="muted">no track</span>}
                        <ResetSlot slot={s.slot} />
                      </li>
                    ))}
                  </ul>
                </section>
                <LayoutPanel engine={engine} />
                <ExportPanel engine={engine} />
              </>
            )}
            <Controls />
            {engine && <SessionPanel engine={engine} />}
            <section className="panel">
              <h2>Takes folder</h2>
              <p className="path" title={status.outputFolder}>{status.outputFolder || "…"}</p>
              {!!engine?.pendingFolder && <p className="path">Moves to {engine.pendingFolder} when this take ends.</p>}
              <p className="path">Every finished take is also saved as a plain .mov in the Takes folder inside it.</p>
              <div className="row">
                <button onClick={() => api.chooseOutputFolder()}>Choose…</button>
              </div>
            </section>
          </aside>
        )}
      </main>

      {!!engine?.recordHint && <div className="record-hint" role="status">{engine.recordHint}</div>}

      <footer className="bottombar">
        <div className="record-when">
          <span className="rw-label">Record when</span>
          <RecordWhen />
        </div>
        <div className="bottom-right">
          <span className="folder" title={status.outputFolder}>{status.outputFolder || ""}</span>
          <button className="paper-btn" onClick={() => api.revealOutputFolder()}>Show takes</button>
          <button className={`paper-btn ${open ? "on" : ""}`} onClick={() => setOpen(!open)}>{open ? "Hide controls" : "Controls"}</button>
        </div>
      </footer>
    </div>
  );
}
