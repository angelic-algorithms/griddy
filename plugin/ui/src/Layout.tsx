import { api, EngineStatus } from "./bridge";

const canvases = ["16:9", "1:1", "9:16"];
// Engine canvas numbers: 0 is 1280x720, 1 is 1920x1080, 2 is 1:1, 3 is 9:16. The picker offers three shapes.
const canvasIndex = [0, 2, 3];
const swatches = ["0b0b0f", "000000", "ffffff", "1c1c24", "7c6cff", "ff4d5e"];

/** The look of the grid. Changing it never touches the takes, only how they are laid out. */
export function LayoutPanel({ engine }: { engine: EngineStatus }) {
  const { layout } = engine;
  const active = Math.max(1, engine.slots.length);
  const shape = canvasIndex.indexOf(layout.canvas);

  const set = (canvas: number, gap: number, background: string) => api.setLayout(canvas, gap, background);

  // The cells in reading order, each showing which slot sits there.
  const cells = Array.from({ length: active }, (_, i) => layout.order[i] ?? i);
  const columns = Math.ceil(Math.sqrt(active));

  return (
    <section className="panel">
      <h2>Grid look</h2>

      <div className="field">
        <span className="field-label">Shape</span>
        <div className="segmented" role="radiogroup" aria-label="Shape">
          {canvases.map((label, i) => (
            <button key={label} role="radio" aria-checked={i === shape} className={i === shape ? "on" : ""}
              onClick={() => set(canvasIndex[i], layout.gap, layout.background)}>
              {label}
            </button>
          ))}
        </div>
      </div>

      <div className="field">
        <span className="field-label">Frame thickness · {layout.gap.toFixed(0)} px</span>
        <input type="range" min={0} max={24} step={1} value={layout.gap}
          onChange={(e) => set(layout.canvas, Number(e.target.value), layout.background)} />
      </div>

      <div className="field">
        <span className="field-label">Frame colour</span>
        <div className="swatches">
          {swatches.map((c) => (
            <button key={c} className={`swatch ${layout.background === c ? "on" : ""}`} style={{ background: `#${c}` }}
              aria-label={`Colour ${c}`} onClick={() => set(layout.canvas, layout.gap, c)} />
          ))}
        </div>
      </div>

      <div className="field">
        <span className="field-label">Order of the cells (click two to swap)</span>
        <CellPicker cells={cells} columns={columns} />
      </div>
    </section>
  );
}

import { useState } from "react";

function CellPicker({ cells, columns }: { cells: number[]; columns: number }) {
  const [first, setFirst] = useState<number | null>(null);
  const pick = (cell: number) => {
    if (first === null) setFirst(cell);
    else {
      if (first !== cell) void api.swapCells(first, cell);
      setFirst(null);
    }
  };

  return (
    <div className="cells" style={{ gridTemplateColumns: `repeat(${columns}, 1fr)` }}>
      {cells.map((slot, i) => (
        <button key={i} className={first === i ? "on" : ""} onClick={() => pick(i)}>
          {slot + 1}
        </button>
      ))}
    </div>
  );
}
