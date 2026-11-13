// One cell of the grid, laid over the real picture: the mockup's Tile, state by state.
import { Transport } from "./bridge";
import { Btn, C, clamp, fmtMs, meter, Sticker, Wave } from "./kit";
import { badgeFor, countIn, SlotView, takeProgress, usePeaks } from "./model";

export type Density = "full" | "compact" | "tiny";

/** The mockup's densFor: by the cell's size on screen. */
export const densityFor = (w: number, h: number): Density => (w < 190 || h < 125 ? "tiny" : w < 300 || h < 190 ? "compact" : "full");

/** How far the take in progress has come: through the host's loop if it loops, else through the current bar. */
function recordProgress(t: Transport) {
  if (t.looping && t.hasLoop && (t.loopEnd ?? 0) > (t.loopStart ?? 0))
    return clamp((t.ppq - (t.loopStart ?? 0)) / ((t.loopEnd ?? 0) - (t.loopStart ?? 0)), 0, 1);
  const bar = meter(t.timeSig).bar;
  return clamp((Math.max(0, t.ppq) % bar) / bar, 0, 1);
}

export function Tile({ v, density, selected, transport, master, onSelect, onKeep, onRetake, onNudge }: {
  v: SlotView;
  density: Density;
  selected: boolean;
  transport: Transport;
  master: boolean;
  onSelect: () => void;
  onKeep: () => void;
  onRetake: () => void;
  onNudge: (ms: number) => void;
}) {
  const compact = density !== "full";
  const tiny = density === "tiny";
  const badge = badgeFor(v, density);
  const showTake = !!v.shown && !v.live;
  const peaks = usePeaks(v.slot, showTake ? v.shown : undefined);
  const progress = takeProgress(v.shown, transport.ppq);
  const prevMuted = (v.armed || v.recording) && !!v.kept;
  const count = v.waiting ? countIn(transport) : 0;
  const frameMs = 1000 / (v.review?.fps || 30);

  return (
    <div
      role="button"
      tabIndex={0}
      aria-label={`Slot ${v.slot + 1}, ${v.name}, ${badge.t}`}
      onClick={onSelect}
      onKeyDown={(e) => {
        if ((e.key === "Enter" || e.key === " ") && e.target === e.currentTarget) {
          e.preventDefault();
          e.stopPropagation();
          onSelect();
        }
      }}
      className="tile"
    >
      {v.empty && (
        <div className="hatch" style={{ position: "absolute", inset: 0, display: "flex", flexDirection: "column", alignItems: "center", justifyContent: "center", gap: 8 }}>
          <div className="f-sign" style={{ lineHeight: 1, fontSize: tiny ? 30 : compact ? 48 : 84, color: v.color, WebkitTextStroke: "3px #000", paintOrder: "stroke fill", textShadow: "4px 4px 0 #000" }}>
            {tiny ? "+" : v.slot + 1}
          </div>
          {!tiny && !v.claimed && !master && <Sticker color={C.gold} rotate={-3} size={13}>TAP TO CLAIM</Sticker>}
        </div>
      )}

      {!v.empty && <div className="scan" style={{ position: "absolute", inset: 0, pointerEvents: "none" }} />}

      {v.live && (
        <>
          <div className="vf" style={{ position: "absolute", inset: 12, pointerEvents: "none" }} />
          {!compact && !v.recording && !v.waiting && (
            <div style={{ position: "absolute", left: "50%", bottom: 12, transform: "translateX(-50%)", pointerEvents: "none" }}>
              <Sticker color="#fff" rotate={0} size={11}>LOW LATENCY PREVIEW</Sticker>
            </div>
          )}
        </>
      )}

      {selected && (
        <div
          className={v.recording ? "ring-rec" : v.armed ? "ring-armed" : ""}
          style={{ position: "absolute", inset: 0, pointerEvents: "none", borderRadius: "inherit", border: `5px solid ${v.recording ? C.blood : C.lime}` }}
        />
      )}

      <div style={{ position: "absolute", left: 8, top: 8, display: "flex", alignItems: "flex-start", gap: 6, maxWidth: "calc(100% - 1rem)", pointerEvents: "none" }}>
        <span className="f-sign" style={{ fontSize: 13, lineHeight: 1, padding: "4px 6px", border: "2.5px solid #000", background: v.color, color: C.ink, boxShadow: "2px 2px 0 #000", transform: "rotate(-3deg)" }}>
          {v.slot + 1}
        </span>
        {!compact && (
          <span className="truncate" style={{ fontWeight: 800, textTransform: "uppercase", letterSpacing: ".025em", fontSize: 13, lineHeight: 1, padding: "4px 6px", background: "rgba(0,0,0,.8)", color: C.paper }}>
            {v.name}
          </span>
        )}
      </div>

      <div style={{ position: "absolute", right: 8, top: 8, display: tiny && v.empty && !selected ? "none" : "flex", flexDirection: "column", alignItems: "flex-end", gap: 4, pointerEvents: "none" }}>
        <span
          className={v.recording ? "blink" : ""}
          style={{ fontWeight: 800, textTransform: "uppercase", letterSpacing: ".025em", fontSize: tiny ? 10 : 12, padding: tiny ? "4px 4px" : "4px 8px", lineHeight: 1, border: "2px solid #000", background: badge.bg, color: badge.fg, boxShadow: "2px 2px 0 #000" }}
        >
          {badge.t}
        </span>
        {prevMuted && !compact && <span style={tag(C.gold)}>Previous take muted</span>}
        {v.shown && v.shown.slip !== 0 && !v.live && !tiny && <span style={tag(C.sky)}>Slip {fmtMs(v.shown.slip * 1000)}</span>}
      </div>

      {count > 0 && (
        <div style={{ position: "absolute", inset: 0, display: "flex", alignItems: "center", justifyContent: "center", pointerEvents: "none", background: "rgba(0,0,0,.35)" }}>
          <div key={count} className="count f-sign" style={{ color: C.paper, lineHeight: 1, fontSize: tiny ? 44 : compact ? 72 : 140, WebkitTextStroke: "4px #000", paintOrder: "stroke fill", textShadow: "6px 6px 0 " + C.hot }}>
            {count}
          </div>
        </div>
      )}

      <div style={{ position: "absolute", left: 0, right: 0, bottom: 0, padding: tiny ? 4 : 8, display: "flex", flexDirection: "column", gap: 6, background: "linear-gradient(0deg, rgba(0,0,0,.82), rgba(0,0,0,0))" }}>
        {showTake && v.shown && (
          <div style={{ height: tiny ? 16 : 28, pointerEvents: "none" }}>
            <Wave peaks={peaks} progress={progress} color={v.color} n={tiny ? 14 : compact ? 28 : 52} />
          </div>
        )}
        {v.recording && (
          <div style={{ height: 6, background: "rgba(255,255,255,.2)", pointerEvents: "none" }}>
            <div style={{ height: "100%", background: C.blood, width: `${recordProgress(transport) * 100}%` }} />
          </div>
        )}
        {v.review && !v.recording && (
          <div style={{ display: "flex", flexWrap: "wrap", alignItems: "center", gap: 8 }} onClick={(e) => e.stopPropagation()}>
            <Btn color="lime" small={compact} onClick={onKeep} title="Keep this take (K)" label="Keep this take">{tiny ? "✓" : "✓ Keep"}</Btn>
            <Btn color="blood" small={compact} onClick={onRetake} title="Discard this take and record again" label="Retake">{tiny ? "↺" : "↺ Retake"}</Btn>
            {!compact && (
              <div role="group" aria-label="Slip offset" style={{ display: "flex", alignItems: "center", gap: 4, marginLeft: "auto", background: "rgba(0,0,0,.8)", border: "2px solid #000", padding: "2px 6px" }}>
                <button type="button" className="nudge" onClick={() => onNudge(-frameMs)} aria-label="Slip earlier by one frame">◀</button>
                <span className="num" style={{ fontWeight: 800, fontSize: 13, color: C.sky, minWidth: 64, textAlign: "center" }}>{fmtMs(v.review.slip * 1000)}</span>
                <button type="button" className="nudge" onClick={() => onNudge(frameMs)} aria-label="Slip later by one frame">▶</button>
              </div>
            )}
          </div>
        )}
      </div>
    </div>
  );
}

const tag = (color: string): React.CSSProperties => ({
  fontWeight: 700, textTransform: "uppercase", fontSize: 11, lineHeight: 1, padding: "4px 6px", background: "rgba(0,0,0,.85)", color,
});
