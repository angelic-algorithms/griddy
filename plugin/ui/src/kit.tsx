// The small parts of the Overdub Cam design (docs/design/overdub-cam.html), with the mockup's props and markup.
import { ReactNode, useMemo } from "react";

export const C = {
  hot: "#ff6a1a",
  lime: "#b8ff1f",
  pink: "#ff3fae",
  sky: "#35c6ff",
  gold: "#ffd400",
  blood: "#ff2e3e",
  violet: "#a66bff",
  mint: "#19e6b4",
  bone: "#f3efe4",
  paper: "#f3efe4",
  ink: "#0b0b0d",
};

/** One colour per slot, slot 1 first. */
export const SLOT_COLORS = [C.hot, C.sky, C.pink, C.lime, C.gold, C.blood, C.violet, C.mint, C.bone];
export const slotColor = (slot: number) => SLOT_COLORS[((slot % 9) + 9) % 9];

/** The grid templates, in the engine's order (CollageLayout.cpp): column, row, column span, row span. */
const grid = (c: number, r: number) => {
  const cells: number[][] = [];
  for (let y = 1; y <= r; y++) for (let x = 1; x <= c; x++) cells.push([x, y, 1, 1]);
  return cells;
};
export type Template = { id: string; label: string; cols: number; rows: number; cells: number[][] };
export const TEMPLATES: Template[] = [
  { id: "1x1", label: "1×1", cols: 1, rows: 1, cells: grid(1, 1) },
  { id: "1x2", label: "1×2", cols: 2, rows: 1, cells: grid(2, 1) },
  { id: "2x1", label: "2×1", cols: 1, rows: 2, cells: grid(1, 2) },
  { id: "1p2", label: "1+2", cols: 3, rows: 2, cells: [[1, 1, 2, 2], [3, 1, 1, 1], [3, 2, 1, 1]] },
  { id: "3s", label: "Stack 3", cols: 1, rows: 3, cells: grid(1, 3) },
  { id: "2x2", label: "2×2", cols: 2, rows: 2, cells: grid(2, 2) },
  { id: "1p3", label: "1+3", cols: 3, rows: 3, cells: [[1, 1, 3, 2], [1, 3, 1, 1], [2, 3, 1, 1], [3, 3, 1, 1]] },
  { id: "2p3", label: "2+3", cols: 6, rows: 2, cells: [[1, 1, 3, 1], [4, 1, 3, 1], [1, 2, 2, 1], [3, 2, 2, 1], [5, 2, 2, 1]] },
  { id: "1p5", label: "1+5", cols: 3, rows: 3, cells: [[1, 1, 2, 2], [3, 1, 1, 1], [3, 2, 1, 1], [1, 3, 1, 1], [2, 3, 1, 1], [3, 3, 1, 1]] },
  { id: "3x2", label: "3×2", cols: 3, rows: 2, cells: grid(3, 2) },
  { id: "3p4", label: "3+4", cols: 12, rows: 2, cells: [[1, 1, 4, 1], [5, 1, 4, 1], [9, 1, 4, 1], [1, 2, 3, 1], [4, 2, 3, 1], [7, 2, 3, 1], [10, 2, 3, 1]] },
  { id: "4x2", label: "4×2", cols: 4, rows: 2, cells: grid(4, 2) },
  { id: "3x3", label: "3×3", cols: 3, rows: 3, cells: grid(3, 3) },
];

/** The three canvas shapes the design offers, with the engine's canvas number for each. */
export const ASPECTS = [
  { id: "9:16", w: 9, h: 16, note: "Reels and TikTok", canvas: 3 },
  { id: "1:1", w: 1, h: 1, note: "Square", canvas: 2 },
  { id: "16:9", w: 16, h: 9, note: "Widescreen", canvas: 0 },
];

export const clamp = (v: number, a: number, b: number) => Math.min(b, Math.max(a, v));

export const fmtMs = (ms: number) => {
  const r = Math.round(ms);
  return (r > 0 ? "+" : r < 0 ? "−" : "") + Math.abs(r) + " ms";
};

export const fmtFrames = (ms: number, fps: number) => {
  const f = ms / (1000 / fps);
  return (f > 0.05 ? "+" : f < -0.05 ? "−" : "") + Math.abs(f).toFixed(1) + " f";
};

/** Quarter notes per bar and per beat for a time signature like "4/4". */
export function meter(timeSig: string | undefined) {
  const [n, d] = (timeSig ?? "4/4").split("/").map(Number);
  const num = n > 0 ? n : 4;
  const den = d > 0 ? d : 4;
  return { num, den, bar: (num * 4) / den, beat: 4 / den };
}

/** Bar.beat for a song position, both from 1, like a DAW shows them. */
export function barBeat(ppq: number, timeSig?: string) {
  const m = meter(timeSig);
  const p = Math.max(0, ppq);
  return `${Math.floor(p / m.bar) + 1}.${Math.floor((p % m.bar) / m.beat) + 1}`;
}

const BTN_BG = {
  hot: "bg-hot",
  lime: "bg-lime",
  pink: "bg-pink",
  sky: "bg-sky",
  gold: "bg-gold",
  blood: "bg-blood",
  bone: "bg-bone",
  ink: "bg-ink",
};
export type BtnColor = keyof typeof BTN_BG;

export function Btn({ color = "bone", onClick, children, disabled, className = "", title, small, pressed, label, style }: {
  color?: BtnColor;
  onClick?: () => void;
  children: ReactNode;
  disabled?: boolean;
  className?: string;
  title?: string;
  small?: boolean;
  pressed?: boolean;
  label?: string;
  style?: React.CSSProperties;
}) {
  return (
    <button
      type="button"
      onClick={onClick}
      disabled={disabled}
      title={title}
      aria-pressed={pressed}
      aria-label={label}
      style={style}
      className={`btn btn-ui ${BTN_BG[color]} ${small ? "btn-sm" : "btn-md"} ${className}`}
    >
      <span>{children}</span>
    </button>
  );
}

export function Sticker({ children, color = C.gold, className = "", rotate = -2, text = "#0b0b0d", size, style }: {
  children: ReactNode;
  color?: string;
  className?: string;
  rotate?: number;
  text?: string;
  size?: number;
  style?: React.CSSProperties;
}) {
  return (
    <span
      className={`f-tag ${className}`}
      style={{
        display: "inline-block", lineHeight: 1, padding: "4px 8px", border: "2.5px solid #000",
        background: color, color: text, transform: `rotate(${rotate}deg)`, boxShadow: "2px 2px 0 #000", fontSize: size, ...style,
      }}
    >
      {children}
    </span>
  );
}

export function Label({ children }: { children: ReactNode }) {
  return <div className="label">{children}</div>;
}

export function Slider({ id, value, min, max, step = 1, onChange, label, readout, color, disabled }: {
  id: string;
  value: number;
  min: number;
  max: number;
  step?: number;
  onChange: (v: number) => void;
  label: string;
  readout: string;
  color?: string;
  disabled?: boolean;
}) {
  return (
    <div className="stack" style={{ gap: 4 }}>
      <div className="row" style={{ alignItems: "baseline", justifyContent: "space-between" }}>
        <label htmlFor={id} style={{ fontWeight: 700, textTransform: "uppercase", letterSpacing: ".08em", fontSize: 13 }}>{label}</label>
        <span className="num" style={{ fontWeight: 800, fontSize: 14 }}>{readout}</span>
      </div>
      <input
        id={id}
        type="range"
        min={min}
        max={max}
        step={step}
        value={value}
        disabled={disabled}
        onChange={(e) => onChange(Number(e.target.value))}
        style={{ accentColor: color || C.hot, width: "100%", height: 20, cursor: "pointer" }}
      />
    </div>
  );
}

export function TplThumb({ tpl, on }: { tpl: Template; on: boolean }) {
  return (
    <div
      style={{
        display: "grid", gap: 2, width: "100%", aspectRatio: "4 / 3", padding: 3, background: "#000",
        gridTemplateColumns: `repeat(${tpl.cols},1fr)`, gridTemplateRows: `repeat(${tpl.rows},1fr)`,
      }}
    >
      {tpl.cells.map((c, i) => (
        <div key={i} style={{ gridColumn: `${c[0]} / span ${c[2]}`, gridRow: `${c[1]} / span ${c[3]}`, background: on ? C.lime : "#76757f" }} />
      ))}
    </div>
  );
}

export function Meter({ level, color }: { level: number; color: string }) {
  return (
    <div style={{ height: 10, width: "100%", background: "rgba(0,0,0,.8)", border: "2px solid #000" }}>
      <div style={{ height: "100%", width: `${clamp(level, 0, 1) * 100}%`, background: level > 0.85 ? C.blood : color, transition: "width .08s linear" }} />
    </div>
  );
}

/** A take's waveform: real peaks squeezed into `n` bars, coloured up to the play position. */
export function Wave({ peaks, progress, color, n = 44 }: { peaks: number[]; progress: number; color: string; n?: number }) {
  const bars = useMemo(() => {
    if (peaks.length === 0) return Array.from({ length: n }, () => 0.14);
    const top = Math.max(...peaks, 1e-6);
    return Array.from({ length: n }, (_, i) => {
      const a = Math.floor((i * peaks.length) / n);
      const b = Math.max(a + 1, Math.floor(((i + 1) * peaks.length) / n));
      return Math.max(0.14, Math.max(...peaks.slice(a, b)) / top);
    });
  }, [peaks, n]);
  return (
    <div style={{ display: "flex", alignItems: "flex-end", gap: 2, height: "100%", width: "100%" }}>
      {bars.map((h, i) => (
        <span key={i} style={{ flex: 1, borderRadius: 1, height: `${h * 100}%`, background: i / n <= progress ? color : "rgba(255,255,255,.3)" }} />
      ))}
    </div>
  );
}
