import { api, EngineStatus } from "./bridge";

const mmss = (s: number) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, "0")}`;

/** Writes the grid and the mix to an MP4 while the DAW plays. The master instance must sit on the track that carries the mix. */
export function ExportPanel({ engine }: { engine: EngineStatus }) {
  const x = engine.export;
  if (!x) return null;
  const busy = x.state === "waiting" || x.state === "recording" || x.state === "writing";

  return (
    <section className="panel">
      <h2>Export MP4</h2>
      {!x.available && <p className="muted">This build cannot write MP4 files.</p>}
      {x.available && (
        <>
          <p className="muted">
            Press Start, then play in the DAW. The grid and what this instance hears are recorded until the DAW stops. Put this
            instance on the master track.
          </p>
          <div className="row">
            {!busy && <button className="primary" onClick={() => api.startExport()}>Start export</button>}
            {(x.state === "waiting" || x.state === "recording") && <button onClick={() => api.stopExport()}>Stop</button>}
          </div>
          {x.state === "waiting" && <span className="chip warn">Waiting for play</span>}
          {x.state === "recording" && <span className="chip ok">Recording {mmss(x.seconds)}</span>}
          {x.state === "writing" && <span className="chip">Finishing the file…</span>}
          {x.state === "done" && <p className="path" title={x.file}>Saved: {x.file}</p>}
          {x.state === "failed" && <span className="chip warn">{x.error || "Export failed"}</span>}
        </>
      )}
    </section>
  );
}
