// A dev only picker that switches the mock backend between every state the UI can show. Never shown in the plugin.
import { useState } from "react";
import { devMock } from "./bridge";

export function DevStatePicker() {
  const [value, setValue] = useState(() => devMock?.scenario() ?? "");
  if (!devMock) return null;
  return (
    <label className="dev-picker">
      <span>Mock state</span>
      <select
        value={value}
        onChange={(e) => {
          setValue(e.target.value);
          devMock?.setScenario(e.target.value);
        }}
      >
        {devMock.scenarios.map((s) => (
          <option key={s.id} value={s.id}>{s.label}</option>
        ))}
      </select>
    </label>
  );
}
