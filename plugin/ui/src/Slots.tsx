import { useEffect, useState } from "react";
import { api, Device, params } from "./bridge";
import { useParam } from "./hooks";

/** One row per active slot: pick which camera (or test pattern) fills it. */
export function Slots({ camerasAllowed }: { camerasAllowed: boolean }) {
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

  return (
    <section className="panel">
      <header className="panel-head">
        <h2>Cameras</h2>
        <button className="ghost" onClick={refresh} title="Look for newly plugged in cameras">
          Rescan
        </button>
      </header>

      {Array.from({ length: numSlots }, (_, slot) => (
        <label key={slot} className="slot">
          <span className="slot-num">{slot + 1}</span>
          <select value={slots[slot] ?? ""} onChange={(e) => assign(slot, e.target.value)}>
            <option value="">Empty</option>
            {cameras.length > 0 && (
              <optgroup label="Cameras and capture cards">
                {cameras.map((d) => (
                  <option key={d.id} value={d.id}>{d.name}</option>
                ))}
              </optgroup>
            )}
            <optgroup label="Test patterns">
              {tests.map((d) => (
                <option key={d.id} value={d.id}>{d.name}</option>
              ))}
            </optgroup>
          </select>
        </label>
      ))}

      {!camerasAllowed && <p className="note">Only test patterns are available here. See the message at the top.</p>}
      {error && <p className="error">{error}</p>}
    </section>
  );
}
