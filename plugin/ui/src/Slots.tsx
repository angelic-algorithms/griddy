import { useEffect, useState } from "react";
import { api, Device, params } from "./bridge";
import { useParam } from "./hooks";

/** One colour per cell, in the order of the poster: orange, blue, pink, green, then more. */
export const slotColors = ["#ff7a1a", "#4dc9ff", "#ff4fd8", "#b5ff1f", "#ffd400", "#a78bfa", "#2ee6a6", "#ff4d5e", "#e8e4d8"];
export const slotColor = (slot: number) => slotColors[((slot % slotColors.length) + slotColors.length) % slotColors.length];

function shortName(name: string) {
  return name.replace(/camera/i, "").trim() || name;
}

/** One taped photo per active cell. Tap a pad to pick which camera (or test pattern) fills it. */
export function CameraPads({ ownSlot, camerasAllowed }: { ownSlot: number; camerasAllowed: boolean }) {
  const numSlots = useParam(params.numSlots);
  const [devices, setDevices] = useState<Device[]>([]);
  const [slots, setSlots] = useState<string[]>([]);
  const [error, setError] = useState("");

  const refresh = async () => {
    const [d, s] = await Promise.all([api.getDevices(), api.getSlots()]);
    setDevices(d);
    setSlots(s);
  };

  useEffect(() => {
    void refresh();
  }, []);

  const assign = async (slot: number, id: string) => {
    const result = await api.setSlotSource(slot, id);
    setSlots(result.slots);
    setError(result.ok ? "" : result.error);
  };

  const cameras = devices.filter((d) => d.kind === "camera");
  const tests = devices.filter((d) => d.kind !== "camera");
  const nameOf = (id: string) => devices.find((d) => d.id === id)?.name ?? "Empty";

  return (
    <div className="pads-wrap">
      <div className="pads">
        {Array.from({ length: numSlots }, (_, slot) => {
          const color = slotColor(slot);
          return (
            <div key={slot} className={`pad ${slot === ownSlot ? "own" : ""}`} style={{ ["--c" as string]: color }}>
              {slot === ownSlot && <span className="pad-on">ON</span>}
              <div className="pad-photo">
                <span className="pad-big">{slot + 1}</span>
              </div>
              <div className="pad-label">
                <span className="pad-num">{slot + 1}</span>
                <span className="pad-name">{shortName(nameOf(slots[slot] ?? ""))}</span>
              </div>
              <select
                className="pad-select"
                value={slots[slot] ?? ""}
                onChange={(e) => assign(slot, e.target.value)}
                aria-label={`Camera for cell ${slot + 1}`}
              >
                <option value="">Empty</option>
                {cameras.length > 0 && (
                  <optgroup label="Cameras and capture cards">
                    {cameras.map((d) => <option key={d.id} value={d.id}>{d.name}</option>)}
                  </optgroup>
                )}
                <optgroup label="Test patterns">
                  {tests.map((d) => <option key={d.id} value={d.id}>{d.name}</option>)}
                </optgroup>
              </select>
            </div>
          );
        })}
      </div>
      <p className="hint">Tap a pad to pick its camera.</p>
      <button className="chip-btn" onClick={refresh} title="Look for newly plugged in cameras">Rescan cameras</button>
      {!camerasAllowed && <p className="hint">Only test patterns are available here. See the message at the top.</p>}
      {error && <p className="error">{error}</p>}
    </div>
  );
}
