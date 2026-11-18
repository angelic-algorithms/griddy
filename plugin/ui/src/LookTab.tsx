// The inspector's Look tab: grid template, canvas shape, frame, and who sits where. Any instance can change it.
import { useEffect, useState } from "react";
import { api, EngineStatus } from "./bridge";
import { ASPECTS, C, Label, Slider, slotColor, TEMPLATES, TplThumb } from "./kit";
import { AskFirst } from "./Recover";

const SWATCHES = [C.hot, C.lime, C.pink, C.sky, C.gold, "#0b0b0d", "#ffffff"];

export function LookTab({ engine, say }: { engine: EngineStatus; say: (text: string) => void }) {
  const layout = engine.layout;
  const color = `#${layout.background}`.toLowerCase();
  const [hex, setHex] = useState(color);
  const [confirm, setConfirm] = useState<number | null>(null);
  useEffect(() => setHex(color), [color]);

  const set = (patch: Partial<{ canvas: number; gap: number; background: string; radius: number }>) => {
    const next = { canvas: layout.canvas, gap: layout.gap, background: layout.background, radius: layout.radius, ...patch };
    void api.setLayout(next.canvas, next.gap, next.background, next.radius);
  };
  const setColor = (c: string) => set({ background: c.replace("#", "").toLowerCase() });

  const panels = layout.cells.length;
  const tplIndex = layout.preset;

  // Slots a smaller template would hide, that still hold a kept take or one in review.
  const hiddenWithTakes = (count: number) =>
    engine.takes.slice(count).some((list) => list?.some((t) => t.state === "kept" || t.state === "review"));

  const choose = async (i: number) => {
    setConfirm(null);
    const ok = await api.choosePreset(i);
    if (!ok) {
      const highest = 32 - Math.clz32(engine.claimedMask ?? 0);
      say(`Slot ${highest} belongs to a track. Pick a template with at least ${highest} panels.`);
    }
  };

  return (
    <div className="stack" style={{ gap: 20 }}>
      <div>
        <Label>Grid template · {panels} {panels === 1 ? "panel" : "panels"}</Label>
        <div style={{ display: "grid", gridTemplateColumns: "repeat(4,1fr)", gap: 8 }}>
          {TEMPLATES.map((t, i) => {
            const on = i === tplIndex;
            const count = t.cells.length;
            return (
              <button
                key={t.id}
                type="button"
                className="choice"
                aria-pressed={on}
                aria-label={`${t.label}, ${count} ${count === 1 ? "panel" : "panels"}`}
                onClick={() => (count < panels && hiddenWithTakes(count) ? setConfirm(i) : choose(i))}
                style={{ padding: 4, display: "flex", flexDirection: "column", gap: 4, alignItems: "center", ...(on ? { background: C.lime, boxShadow: "3px 3px 0 #000", transform: "translateY(-1px)" } : {}) }}
              >
                <TplThumb tpl={t} on={on} />
                <span style={{ fontWeight: 800, fontSize: 13, lineHeight: 1 }}>{t.label}</span>
              </button>
            );
          })}
        </div>
        {confirm !== null && (
          <div style={{ marginTop: 10 }}>
            <AskFirstOpen
              question={`${TEMPLATES[confirm].label} has ${TEMPLATES[confirm].cells.length} panels, so slots ${TEMPLATES[confirm].cells.length + 1} to ${panels} leave the grid. Their takes stay on disk and come back when you pick a bigger template.`}
              confirmLabel={`Use ${TEMPLATES[confirm].label}`}
              onConfirm={() => choose(confirm)}
              onCancel={() => setConfirm(null)}
            />
          </div>
        )}
      </div>

      <div>
        <Label>Canvas shape</Label>
        <div style={{ display: "grid", gridTemplateColumns: "repeat(3,1fr)", gap: 8 }}>
          {ASPECTS.map((a) => {
            const on = a.canvas === layout.canvas || (a.id === "16:9" && layout.canvas === 1);
            const w = a.w >= a.h ? 34 : Math.round(34 * (a.w / a.h));
            const h = a.h >= a.w ? 34 : Math.round(34 * (a.h / a.w));
            const size = layout.sizes?.[on ? layout.canvas : a.canvas];
            return (
              <button
                key={a.id}
                type="button"
                className="choice"
                aria-pressed={on}
                onClick={() => set({ canvas: a.canvas })}
                style={{ padding: "8px 4px", display: "flex", flexDirection: "column", alignItems: "center", gap: 4, ...(on ? { background: C.hot, boxShadow: "3px 3px 0 #000", transform: "translateY(-1px)" } : {}) }}
              >
                <div style={{ height: 36, display: "flex", alignItems: "center" }}>
                  <div style={{ border: "3px solid #000", background: C.paper, width: w, height: h }} />
                </div>
                <span style={{ fontWeight: 800, fontSize: 16, lineHeight: 1 }}>{a.id}</span>
                <span style={{ fontSize: 12, lineHeight: 1 }}>{a.note}</span>
                {size && <span className="num" style={{ fontSize: 12, lineHeight: 1, fontWeight: 700 }}>{size}</span>}
              </button>
            );
          })}
        </div>
      </div>

      <div className="stack" style={{ gap: 12 }}>
        <Label>Frame</Label>
        <Slider id="thick" label="Thickness" value={layout.gap} min={0} max={24} onChange={(v) => set({ gap: v })} readout={`${Math.round(layout.gap)} px`} />
        <Slider id="radius" label="Corner radius" value={layout.radius} min={0} max={40} onChange={(v) => set({ radius: v })} readout={`${Math.round(layout.radius)} px`} />
        <div>
          <div style={{ fontWeight: 700, textTransform: "uppercase", letterSpacing: ".08em", fontSize: 13, marginBottom: 6 }}>Frame colour</div>
          <div className="row" style={{ gap: 8, flexWrap: "wrap" }}>
            <input id="frame-color" type="color" value={color} onChange={(e) => setColor(e.target.value)}
              style={{ width: 44, height: 36, border: "3px solid #000", background: "#fff", padding: 0, cursor: "pointer" }} aria-label="Pick frame colour" />
            <input id="frame-hex" type="text" value={hex.toUpperCase()} maxLength={7} spellCheck={false} aria-label="Frame colour hex code"
              onChange={(e) => {
                setHex(e.target.value);
                if (/^#[0-9a-fA-F]{6}$/.test(e.target.value)) setColor(e.target.value.toLowerCase());
              }}
              className="num" style={{ width: 96, fontFamily: "inherit", fontWeight: 700, fontSize: 16, textTransform: "uppercase", background: "#fff", color: C.ink, border: "3px solid #000", padding: "4px 8px" }} />
            <div className="row" style={{ flexWrap: "wrap", gap: 6 }}>
              {SWATCHES.map((c) => (
                <button key={c} type="button" onClick={() => setColor(c)} aria-label={`Use ${c}`}
                  style={{ width: 24, height: 24, border: "2.5px solid #000", background: c, outline: color === c ? "3px solid #fff" : "none", outlineOffset: 1, cursor: "pointer", padding: 0 }} />
              ))}
            </div>
          </div>
        </div>
      </div>

      <WhoSitsWhere engine={engine} />
    </div>
  );
}

/** The confirm part of AskFirst, opened by a template button rather than its own. */
function AskFirstOpen({ question, confirmLabel, onConfirm, onCancel }: { question: string; confirmLabel: string; onConfirm: () => void; onCancel: () => void }) {
  return (
    <div className="ask" role="alertdialog" aria-label={confirmLabel}>
      <p>{question}</p>
      <div className="row" style={{ gap: 8 }}>
        <button type="button" className="btn btn-ui btn-sm bg-blood" onClick={onConfirm}><span>{confirmLabel}</span></button>
        <button type="button" className="btn btn-ui btn-sm bg-bone" onClick={onCancel}><span>Cancel</span></button>
      </div>
    </div>
  );
}

/** Which slot sits in which cell: click two to swap them. The first cell is the big one in 1+2, 1+3 and 1+5. */
function WhoSitsWhere({ engine }: { engine: EngineStatus }) {
  const [first, setFirst] = useState<number | null>(null);
  const cells = engine.layout.cells;
  const pick = (cell: number) => {
    if (first === null) setFirst(cell);
    else {
      if (first !== cell) void api.swapCells(first, cell);
      setFirst(null);
    }
  };
  if (cells.length < 2) return null;
  return (
    <div>
      <Label>Who sits where</Label>
      <p className="small-note" style={{ margin: "0 0 8px" }}>Click two cells to swap their slots.</p>
      <div style={{ position: "relative", width: "100%", aspectRatio: `${engine.layout.width} / ${engine.layout.height}`, maxHeight: 200, background: "#000", border: "3px solid #000" }}>
        {cells.map((c, i) => (
          <button
            key={i}
            type="button"
            className="choice"
            aria-pressed={first === i}
            aria-label={`Cell ${i + 1}, slot ${c.slot + 1}`}
            onClick={() => pick(i)}
            style={{ position: "absolute", left: `${c.x * 100}%`, top: `${c.y * 100}%`, width: `${c.w * 100}%`, height: `${c.h * 100}%`, display: "flex", alignItems: "center", justifyContent: "center", border: "2px solid #000", ...(first === i ? { background: C.lime } : {}) }}
          >
            <span className="f-sign" style={{ fontSize: 13, lineHeight: 1, padding: "3px 5px", border: "2px solid #000", background: slotColor(c.slot) }}>{c.slot + 1}</span>
          </button>
        ))}
      </div>
    </div>
  );
}
