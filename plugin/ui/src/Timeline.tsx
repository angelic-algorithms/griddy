// The takes timeline under the canvas: one row per slot, takes placed where they sit in the project.
import { Transport } from "./bridge";
import { barBeat, fmtMs, meter } from "./kit";
import { SlotView, takeBeats } from "./model";

export function Timeline({ views, transport, selected, onSelect }: {
  views: SlotView[];
  transport: Transport;
  selected: number;
  onSelect: (slot: number) => void;
}) {
  const m = meter(transport.timeSig);
  const startOf = (v: SlotView) => (v.shown ? (v.shown.startPpq ?? 0) + (v.shown.slip * (v.shown.bpm || 120)) / 60 : 0);

  // From bar 1 to the later of the last take's end and the playhead, in whole bars, at least 8.
  let end = Math.max(0, transport.ppq);
  for (const v of views) if (v.shown) end = Math.max(end, startOf(v) + takeBeats(v.shown));
  const bars = Math.max(8, Math.ceil(end / m.bar + 1e-6));
  const total = bars * m.bar;
  const pct = (ppq: number) => `${(Math.max(0, ppq) / total) * 100}%`;
  const showLoop = !!transport.hasLoop && (transport.loopEnd ?? 0) > (transport.loopStart ?? 0);

  return (
    <div className="paper tape" style={{ position: "relative", padding: "12px 12px 24px", marginTop: 8, transform: "rotate(-.2deg)" }}>
      <div style={{ display: "flex" }}>
        <div style={{ width: 150, flexShrink: 0, paddingRight: 8, display: "flex", alignItems: "flex-end", paddingBottom: 4 }}>
          <span className="f-tag" style={{ fontSize: 14 }}>Takes timeline</span>
        </div>
        <div style={{ flex: 1, position: "relative" }} title="Move the playhead in Live" aria-label="Timeline ruler">
          <div style={{ position: "relative", height: 24, borderBottom: "3px solid #000" }}>
            {Array.from({ length: bars }, (_, b) => (
              <div key={b} style={{ position: "absolute", top: 0, bottom: 0, left: pct(b * m.bar), borderLeft: "2px solid rgba(0,0,0,.6)", paddingLeft: 4, fontWeight: 700, fontSize: 12 }}>
                {b + 1}
              </div>
            ))}
            {showLoop && (
              <div
                style={{ position: "absolute", bottom: 0, height: 6, background: "var(--hot)", border: "2px solid #000", left: pct(transport.loopStart ?? 0), width: pct((transport.loopEnd ?? 0) - (transport.loopStart ?? 0)) }}
                title={`Loop ${barBeat(transport.loopStart ?? 0, transport.timeSig)} to ${barBeat(transport.loopEnd ?? 0, transport.timeSig)}`}
              />
            )}
          </div>
        </div>
      </div>

      <div style={{ maxHeight: 132, overflowY: "auto" }}>
        {views.map((v) => {
          const t = v.shown;
          return (
            <div key={v.slot} style={{ display: "flex", alignItems: "stretch", height: 26, borderBottom: "1px solid rgba(0,0,0,.2)" }}>
              <button type="button" onClick={() => onSelect(v.slot)} aria-label={`Select slot ${v.slot + 1}`}
                style={{ width: 150, flexShrink: 0, display: "flex", alignItems: "center", gap: 6, paddingRight: 8, textAlign: "left", background: "none", border: 0, padding: "0 8px 0 0", cursor: "pointer", color: "var(--ink)" }}>
                <span className="f-sign" style={{ fontSize: 11, lineHeight: 1, padding: "3px 4px", border: "2px solid #000", background: v.color, color: "var(--ink)" }}>{v.slot + 1}</span>
                <span className="truncate" style={{ fontWeight: 700, textTransform: "uppercase", fontSize: 13, textDecoration: selected === v.slot ? "underline" : "none", textDecorationThickness: 3 }}>
                  {v.name}
                </span>
              </button>
              <div style={{ flex: 1, position: "relative" }} title="Move the playhead in Live">
                {Array.from({ length: bars }, (_, b) => (
                  <div key={b} style={{ position: "absolute", top: 0, bottom: 0, left: pct(b * m.bar), borderLeft: "1px solid rgba(0,0,0,.15)" }} />
                ))}
                {t && (
                  <div
                    style={{
                      position: "absolute", top: 3, bottom: 3, border: "2px solid #000", display: "flex", alignItems: "center", padding: "0 6px", overflow: "hidden",
                      left: pct(startOf(v)), width: pct(takeBeats(t)),
                      background: t.state === "review" ? `repeating-linear-gradient(45deg,${v.color} 0 8px, #f3efe4 8px 14px)` : v.color,
                      borderStyle: t.state === "review" ? "dashed" : "solid",
                      opacity: t.state === "kept" && v.armed && transport.playing ? 0.35 : 1,
                    }}
                    title={`${v.name}, ${t.state === "review" ? "review" : "approved"}, slip ${fmtMs(t.slip * 1000)}`}
                  >
                    <span style={{ fontWeight: 800, textTransform: "uppercase", fontSize: 11, background: "rgba(0,0,0,.8)", color: "var(--paper)", padding: "0 4px", lineHeight: "14px" }}>
                      {t.state === "review" ? "Pending" : "Take"}
                    </span>
                  </div>
                )}
                {v.recording && (
                  <div className="stripes-rec" style={{ position: "absolute", top: 3, bottom: 3, border: "2px solid #000", left: pct(v.info?.recordStartPpq ?? 0), width: pct(Math.max(0, transport.ppq - (v.info?.recordStartPpq ?? 0))) }} />
                )}
              </div>
            </div>
          );
        })}
      </div>

      <div style={{ position: "absolute", top: 34, bottom: 24, left: 162, right: 12, pointerEvents: "none" }}>
        <div style={{ position: "absolute", top: 0, bottom: 0, width: 3, background: "var(--blood)", left: pct(transport.ppq), boxShadow: "0 0 0 1px #000" }}>
          <span className="num" style={{ position: "absolute", bottom: -20, left: "50%", transform: "translateX(-50%)", fontWeight: 800, fontSize: 11, background: "var(--blood)", color: "#fff", padding: "0 4px", border: "1px solid #000" }}>
            {barBeat(transport.ppq, transport.timeSig)}
          </span>
        </div>
      </div>
    </div>
  );
}
