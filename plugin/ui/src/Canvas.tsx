// The grid: the engine's composited picture in the mockup's taped frame, with a tile laid over each cell.
import { useEffect, useRef, useState } from "react";
import { Layout, Transport } from "./bridge";
import { SlotView } from "./model";
import { Preview } from "./Preview";
import { densityFor, Tile } from "./Tile";

export function Canvas({ layout, views, selected, transport, master, onSelect, onKeep, onRetake, onNudge }: {
  layout: Layout;
  views: SlotView[];
  selected: number;
  transport: Transport;
  master: boolean;
  onSelect: (slot: number) => void;
  onKeep: (slot: number) => void;
  onRetake: (slot: number) => void;
  onNudge: (slot: number, ms: number) => void;
}) {
  const ratio = (layout.width || 1280) / (layout.height || 720);
  const areaRef = useRef<HTMLDivElement>(null);
  const [box, setBox] = useState({ w: 640, h: 360 });

  // The mockup's fit: as wide as the area, then shrink to its height.
  useEffect(() => {
    const el = areaRef.current;
    if (!el) return;
    const measure = () => {
      const r = el.getBoundingClientRect();
      let w = r.width, h = r.width / ratio;
      if (h > r.height) {
        h = r.height;
        w = h * ratio;
      }
      setBox({ w: Math.max(120, Math.floor(w)), h: Math.max(120, Math.floor(h)) });
    };
    measure();
    const ro = new ResizeObserver(measure);
    ro.observe(el);
    return () => ro.disconnect();
  }, [ratio]);

  // The engine measures frame and corners on a 1280 wide canvas; the picture is drawn inside the 3 px border.
  const inner = { w: box.w - 6, h: box.h - 6 };
  const scale = inner.w / 1280;
  const cellRadius = layout.radius * scale;

  return (
    <div className="stage-area">
      <div ref={areaRef} className="stage-fit">
        <div style={{ position: "relative", width: box.w, height: box.h }}>
          <div
            className="frame"
            style={{ width: box.w, height: box.h, background: `#${layout.background}`, borderRadius: (layout.radius + layout.gap * 0.5) * scale }}
          >
            <Preview />
            {layout.cells.map((c, i) => {
              const v = views[c.slot];
              if (!v) return null;
              return (
                <div
                  key={i}
                  className="cell"
                  style={{ left: `${c.x * 100}%`, top: `${c.y * 100}%`, width: `${c.w * 100}%`, height: `${c.h * 100}%`, borderRadius: cellRadius }}
                >
                  <Tile
                    v={v}
                    density={densityFor(c.w * inner.w, c.h * inner.h)}
                    selected={selected === c.slot}
                    transport={transport}
                    master={master}
                    onSelect={() => onSelect(c.slot)}
                    onKeep={() => onKeep(c.slot)}
                    onRetake={() => onRetake(c.slot)}
                    onNudge={(ms) => onNudge(c.slot, ms)}
                  />
                </div>
              );
            })}
          </div>
          <span className="tape-piece" style={{ position: "absolute", top: -12, left: -12, width: 64, height: 20, transform: "rotate(-38deg)" }} />
          <span className="tape-piece" style={{ position: "absolute", top: -12, right: -12, width: 64, height: 20, transform: "rotate(36deg)" }} />
        </div>
      </div>
    </div>
  );
}
