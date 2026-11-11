import { ChoiceParam, params } from "./bridge";
import { useParam } from "./hooks";

/** A row of buttons for a choice parameter. Feels faster than a dropdown for 3 to 5 options. */
function Segmented({ label, param }: { label: string; param: ChoiceParam }) {
  const index = useParam(param);
  return (
    <div className="field">
      <span className="field-label">{label}</span>
      <div className="segmented" role="radiogroup" aria-label={label}>
        {param.choices().map((choice, i) => (
          <button
            key={choice}
            role="radio"
            aria-checked={i === index}
            className={i === index ? "on" : ""}
            onClick={() => param.set(i)}
          >
            {choice}
          </button>
        ))}
      </div>
    </div>
  );
}

function Stepper() {
  const value = useParam(params.numSlots);
  const { min, max } = params.numSlots.range();
  return (
    <div className="field">
      <span className="field-label">Slots</span>
      <div className="stepper">
        <button onClick={() => params.numSlots.set(Math.max(min, value - 1))} aria-label="Fewer slots">−</button>
        <span>{value}</span>
        <button onClick={() => params.numSlots.set(Math.min(max, value + 1))} aria-label="More slots">+</button>
      </div>
    </div>
  );
}

export function Controls() {
  return (
    <section className="panel">
      <h2>Grid</h2>
      <Stepper />
    </section>
  );
}

export function RecordControls() {
  return (
    <section className="panel">
      <h2>Recording</h2>
      <Segmented label="Record when" param={params.recordMode} />
    </section>
  );
}
