// The inspector's Audio tab: Griddy listens to the DAW, so the only mode is DAW routed, with a meter per slot.
import { Transport } from "./bridge";
import { Btn, Label, Meter } from "./kit";
import { SlotView } from "./model";

export function AudioTab({ views, transport }: { views: SlotView[]; transport: Transport }) {
  return (
    <div className="stack" style={{ gap: 16 }}>
      <div>
        <Label>Audio mode</Label>
        <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 8 }}>
          <Btn color="lime" pressed>DAW routed</Btn>
          <Btn color="bone" pressed={false} disabled title="Not built yet">Internal mixer</Btn>
        </div>
      </div>
      <div>
        <p className="msg" style={{ marginBottom: 12, color: "var(--ink)" }}>
          Mix on your DAW tracks. Griddy listens to the audio routed from each assigned track. These meters show it where Griddy sits on the track, before the track's fader.
        </p>
        <div className="stack" style={{ gap: 8 }}>
          {views.map((v) => (
            <div key={v.slot} style={{ display: "grid", gridTemplateColumns: "24px 1fr", alignItems: "center", gap: 8 }}>
              <span className="f-sign" style={{ fontSize: 11, lineHeight: 1, padding: "3px 4px", border: "2px solid #000", textAlign: "center", background: v.color }}>{v.slot + 1}</span>
              <div style={{ minWidth: 0 }}>
                <div className="truncate" style={{ fontWeight: 700, textTransform: "uppercase", fontSize: 13, lineHeight: 1, marginBottom: 4 }}>{v.name}</div>
                <Meter level={transport.levels?.[v.slot] ?? 0} color={v.color} />
              </div>
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}
