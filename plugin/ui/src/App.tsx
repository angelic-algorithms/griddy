import { api, params, Status, Transport } from "./bridge";
import { Controls, RecordControls } from "./Controls";
import { useBackendEvent, useParam } from "./hooks";
import { Preview } from "./Preview";
import { Slots } from "./Slots";

const initialTransport: Transport = {
  bpm: 120, ppq: 0, bar: 1, beat: 1, timeSig: "4/4", playing: false, recording: false, hostSync: false,
};

const initialStatus: Status = {
  outputFolder: "", lastTake: "", lastError: "", recorderFormat: "", recording: false, fps: 0,
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
        <ArmButton recording={status.recording} />
      </header>

      {!status.camerasAllowed && status.cameraBlockedReason && (
        <div className="banner">{status.cameraBlockedReason}</div>
      )}

      <main className="main">
        <div className="stage">
          <Preview />
          <footer className="stage-foot muted">
            <span>{status.outputSize} · {status.fps.toFixed(0)} fps · {status.recorderFormat}</span>
            {status.lastTake && <span>Last take: {fileName(status.lastTake)}</span>}
          </footer>
          {status.lastError && <p className="error">{status.lastError}</p>}
        </div>

        <aside className="side">
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
