import { api, params, Status, Transport } from "./bridge";
import { LayoutPanel } from "./Layout";
import { SyncChip, Takes } from "./Takes";
import { Controls, RecordControls } from "./Controls";
import { useBackendEvent, useParam } from "./hooks";
import { Preview } from "./Preview";
import { Slots } from "./Slots";

const initialTransport: Transport = {
  bpm: 120, ppq: 0, bar: 1, beat: 1, timeSig: "4/4", playing: false, recording: false, hostSync: false,
};

const initialStatus: Status = {
  outputFolder: "", lastTake: "", lastError: "", fps: 0,
  camerasAllowed: true, cameraBlockedReason: "", outputSize: "",
};

function TransportBar({ t }: { t: Transport }) {
  return (
    <div className="transport" title={t.hostSync ? "Synced to your DAW" : "No DAW transport, free running"}>
      <span className={`dot ${t.playing ? "play" : ""}`} aria-label={t.playing ? "Playing" : "Stopped"} />
      <span className={`dot ${t.recording ? "rec" : ""}`} aria-label={t.recording ? "DAW recording" : "DAW not recording"} />
      <span className="position">{t.bar}.{t.beat}</span>
      <span className="muted">{t.bpm.toFixed(1)} BPM · {t.timeSig}</span>
    </div>
  );
}

function ArmButton({ recording }: { recording: boolean }) {
  const armed = useParam(params.recordArm);
  const label = recording ? "Recording" : armed ? "Armed" : "Arm";
  return (
    <button
      className={`arm ${armed ? "armed" : ""} ${recording ? "live" : ""}`}
      onClick={() => params.recordArm.set(!armed)}
      aria-pressed={armed}
    >
      <span className="arm-dot" /> {label}
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
    <button className={`ghost ${on ? "on" : ""}`} aria-pressed={on} onClick={() => params.master.set(!on)}
      title="Master: arrange the grid and see every slot. A master has no cell of its own.">
      {on ? "Master view" : "Master"}
    </button>
  );
}

function fileName(path: string) {
  return path.split(/[\\/]/).pop() ?? path;
}

export function App() {
  const transport = useBackendEvent<Transport>("transport", initialTransport);
  const status = useBackendEvent<Status>("status", initialStatus);

  return (
    <div className="app">
      <header className="topbar">
        <h1>Griddy</h1>
        <TransportBar t={transport} />
        <MasterToggle />
        {!status.engine?.master && <ArmButton recording={!!status.engine?.recording} />}
      </header>

      {!status.camerasAllowed && status.cameraBlockedReason && (
        <div className="banner">{status.cameraBlockedReason}</div>
      )}

      <main className="main">
        <div className="stage">
          <Preview />
          <footer className="stage-foot muted">
            <span>{status.outputSize} · {status.fps.toFixed(0)} fps</span>
            {status.lastTake && <span>Last take: {fileName(status.lastTake)}</span>}
          </footer>
          {status.lastError && <p className="error">{status.lastError}</p>}
        </div>

        <aside className="side">
          {status.engine && !status.engine.master && (
            <>
              <section className="panel">
                <h2>This track</h2>
                <SlotPicker slot={status.engine.slot} instances={status.engine.instances} />
              </section>
              <Takes engine={status.engine} />
            </>
          )}
          {status.engine?.master && (
            <>
              <section className="panel">
                <h2>Every slot</h2>
                <ul className="slotlist">
                  {status.engine.slots.map((s) => (
                    <li key={s.slot}>
                      <span className="slot-num">{s.slot + 1}</span>
                      <SyncChip slot={s} />
                      {!s.claimed && <span className="muted">no track</span>}
                    </li>
                  ))}
                </ul>
              </section>
              <LayoutPanel engine={status.engine} />
            </>
          )}
          <Controls />
          <Slots camerasAllowed={status.camerasAllowed} />
          <RecordControls />
          <section className="panel">
            <h2>Takes folder</h2>
            <p className="path" title={status.outputFolder}>{status.outputFolder || "…"}</p>
            <div className="row">
              <button onClick={() => api.chooseOutputFolder()}>Choose…</button>
              <button className="ghost" onClick={() => api.revealOutputFolder()}>Show</button>
            </div>
          </section>
        </aside>
      </main>
    </div>
  );
}
