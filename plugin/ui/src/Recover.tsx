import { ReactNode, useState } from "react";
import { api } from "./bridge";
import { Btn, BtnColor } from "./kit";

/**
 * A button that asks first. Used for everything that hides takes from the grid, so one stray click never does.
 * The question is shown in the page itself: a plugin's web view may not show the browser's own confirm dialog.
 */
export function AskFirst({ label, question, confirmLabel, onConfirm, disabled, title, color = "bone", small, className, refused }: {
  label: ReactNode;
  question: string;
  confirmLabel: string;
  onConfirm: () => Promise<boolean>;
  disabled?: boolean;
  title?: string;
  color?: BtnColor;
  small?: boolean;
  className?: string;
  refused?: string;
}) {
  const [asking, setAsking] = useState(false);
  const [error, setError] = useState("");

  if (!asking)
    return (
      <>
        <Btn color={color} small={small} disabled={disabled} title={title} className={className} onClick={() => { setError(""); setAsking(true); }}>{label}</Btn>
        {error && <p className="error-line">{error}</p>}
      </>
    );

  return (
    <div className="ask" role="alertdialog" aria-label={typeof label === "string" ? label : confirmLabel}>
      <p>{question}</p>
      <div className="row" style={{ gap: 8 }}>
        <Btn color="blood" small onClick={async () => {
          const ok = await onConfirm();
          setAsking(false);
          if (!ok) setError(refused ?? "Not now: a take or an export is running. Stop it and try again.");
        }}>{confirmLabel}</Btn>
        <Btn small onClick={() => setAsking(false)}>Cancel</Btn>
      </div>
    </div>
  );
}

/** Back to the starting step for one slot: live camera, ready to record. */
export function ResetSlot({ slot, pending, label = "Reset slot", onDone }: { slot: number; pending?: boolean; label?: string; onDone?: () => void }) {
  if (pending) return <span className="small-note">Resetting slot {slot + 1}…</span>;
  return (
    <AskFirst
      label={label}
      small
      title="Stop any take here, switch arm off and show the live camera again"
      question={`Reset slot ${slot + 1}? A take in progress ends, arm goes off and the slot shows its live camera. `
        + "Its takes stay on disk and in the take list, where Restore brings any of them back."}
      confirmLabel={`Reset slot ${slot + 1}`}
      onConfirm={async () => {
        const ok = await api.resetSlot(slot);
        if (ok) onDone?.();
        return ok;
      }}
    />
  );
}
