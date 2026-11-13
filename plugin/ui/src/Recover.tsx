import { useState } from "react";
import { api, EngineStatus } from "./bridge";

/**
 * A button that asks first. Used for everything that hides takes from the grid, so one stray click never does.
 * The question is shown in the page itself: a plugin's web view may not show the browser's own confirm dialog.
 */
function AskFirst({ label, question, confirmLabel, onConfirm, disabled, title }: {
  label: string;
  question: string;
  confirmLabel: string;
  onConfirm: () => Promise<boolean>;
  disabled?: boolean;
  title?: string;
}) {
  const [asking, setAsking] = useState(false);
  const [error, setError] = useState("");

  if (!asking)
    return (
      <>
        <button disabled={disabled} title={title} onClick={() => { setError(""); setAsking(true); }}>{label}</button>
        {error && <p className="error">{error}</p>}
      </>
    );

  return (
    <div className="ask" role="alertdialog" aria-label={label}>
      <p>{question}</p>
      <div className="row">
        <button className="primary" onClick={async () => {
          const ok = await onConfirm();
          setAsking(false);
          if (!ok) setError("Not now: a take or an export is running. Stop it and try again.");
        }}>{confirmLabel}</button>
        <button onClick={() => setAsking(false)}>Cancel</button>
      </div>
    </div>
  );
}

/** Back to the starting step for one slot: live camera, ready to record. */
export function ResetSlot({ slot, pending }: { slot: number; pending?: boolean }) {
  if (pending) return <span className="muted">Resetting slot {slot + 1}…</span>;
  return (
    <AskFirst
      label="Reset slot"
      title="Stop any take here, switch arm off and show the live camera again"
      question={`Reset slot ${slot + 1}? A take in progress ends, arm goes off and the slot shows its live camera. `
        + "Its takes stay on disk and in this list, where Restore brings any of them back."}
      confirmLabel={`Reset slot ${slot + 1}`}
      onConfirm={() => api.resetSlot(slot)}
    />
  );
}

/** Which session folder this set uses, and a way to start an empty one. */
export function SessionPanel({ engine }: { engine: EngineStatus }) {
  const folder = engine.sessionFolder ?? "";
  return (
    <section className="panel">
      <h2>Session</h2>
      <p className="path" title={folder}>{engine.session}</p>
      <p className="note">Takes for this set are saved in this folder. Saving your set keeps it; a new set or a template starts its own.</p>
      <div className="row">
        <button onClick={() => api.revealSessionFolder()}>Show folder</button>
        <AskFirst
          label="New session"
          disabled={engine.recording}
          question="Start an empty session for this set? Every slot goes back to its live camera. Nothing is deleted: the current session folder stays in Sessions with all its takes."
          confirmLabel="Start new session"
          onConfirm={() => api.newSession()}
        />
      </div>
    </section>
  );
}
