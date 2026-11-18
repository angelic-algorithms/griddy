// The Export MP4 sheet: the mockup's taped dialog, driven by the real export. It records while Live plays.
import { useEffect } from "react";
import { api, EngineStatus } from "./bridge";
import { ASPECTS, Btn, Label } from "./kit";

const mmss = (s: number) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, "0")}`;

export function ExportSheet({ engine, onClose }: { engine: EngineStatus; onClose: () => void }) {
  useEffect(() => {
    const k = (e: KeyboardEvent) => e.key === "Escape" && onClose();
    window.addEventListener("keydown", k);
    return () => window.removeEventListener("keydown", k);
  }, [onClose]);

  const x = engine.export;
  const layout = engine.layout;
  const shape = ASPECTS.find((a) => a.canvas === layout.canvas)?.id ?? "16:9";
  const n = layout.cells.length;

  return (
    <div style={{ position: "fixed", inset: 0, zIndex: 50, display: "flex", alignItems: "center", justifyContent: "center", background: "rgba(0,0,0,.7)", padding: 16 }} onClick={onClose}>
      <div role="dialog" aria-modal="true" aria-label="Export MP4" className="paper tape" style={{ position: "relative", width: "100%", maxWidth: 520, padding: 20 }} onClick={(e) => e.stopPropagation()}>
        <div className="row" style={{ justifyContent: "space-between", marginBottom: 16 }}>
          <h2 className="f-sign" style={{ fontSize: 28, lineHeight: 1, margin: 0 }}>Export MP4</h2>
          <Btn small onClick={onClose}>Close</Btn>
        </div>

        <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 16, marginBottom: 16 }}>
          <div>
            <Label>Video</Label>
            <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 8 }}>
              <Btn small color="lime" pressed>H.264</Btn>
              <Btn small pressed={false} disabled title="Not built yet">HEVC</Btn>
            </div>
          </div>
          <div>
            <Label>Audio, 48 kHz</Label>
            <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 8 }}>
              <Btn small color="lime" pressed>AAC</Btn>
              <Btn small pressed={false} disabled title="Not built yet">PCM 24 bit</Btn>
            </div>
          </div>
        </div>

        <div style={{ fontSize: 15, marginBottom: 16 }}>
          {layout.width}×{layout.height} · {shape} · {n} {n === 1 ? "panel" : "panels"} · H.264 with AAC audio from the master bounce.
          {" "}Griddy records the grid while Live plays, with the sound the master Griddy hears, so put it on the master track.
        </div>

        {!x.available && <p className="error-line">This build cannot write MP4 files.</p>}
        {x.available && (x.state === "idle" || x.state === "done" || x.state === "failed") && (
          <Btn color="hot" onClick={() => api.startExport()} style={{ width: "100%" }}>Start export</Btn>
        )}
        {x.state === "waiting" && (
          <div className="row" style={{ gap: 10, justifyContent: "space-between" }}>
            <span style={{ fontWeight: 700, fontSize: 14 }}>Press play in Live to start</span>
            <Btn small onClick={() => api.stopExport()}>Stop</Btn>
          </div>
        )}
        {x.state === "recording" && (
          <div className="row" style={{ gap: 10, justifyContent: "space-between" }}>
            <span className="num" style={{ fontWeight: 700, fontSize: 14 }}>Recording {mmss(x.seconds)}. Stop Live, or press Stop.</span>
            <Btn small color="blood" onClick={() => api.stopExport()}>Stop</Btn>
          </div>
        )}
        {x.state === "writing" && (
          <div>
            <div style={{ height: 24, border: "3px solid #000", background: "#fff" }}>
              <div style={{ height: "100%", width: "100%", background: "var(--lime)" }} />
            </div>
            <div style={{ fontWeight: 700, fontSize: 14, marginTop: 8 }}>Compositing and encoding: finishing the file</div>
          </div>
        )}
        {x.state === "done" && (
          <div style={{ marginTop: 12 }}>
            <p className="path" title={x.file} style={{ fontWeight: 700 }}>Saved: {x.file}</p>
            <Btn small onClick={() => api.revealOutputFolder()} style={{ marginTop: 8 }}>Show in Finder</Btn>
          </div>
        )}
        {x.state === "failed" && <p className="error-line" style={{ marginTop: 12 }}>{x.error || "Export failed"}</p>}
      </div>
    </div>
  );
}
