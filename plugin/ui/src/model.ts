// What each slot is doing, worked out once from the engine status and the host transport, so the tiles,
// the timeline and the inspector all say the same thing.
import { useEffect, useState } from "react";
import { api, EngineStatus, SlotInfo, TakeInfo, Transport } from "./bridge";
import { C, meter, slotColor } from "./kit";

export type Badge = { t: string; bg: string; fg: string };

export type SlotView = {
  slot: number;
  color: string;
  name: string;
  claimed: boolean;
  mine: boolean;
  armed: boolean;
  recording: boolean;
  /** Armed and about to record: Live is counting in (a position before the song start). Armed plus play records at once. */
  waiting: boolean;
  review?: TakeInfo;
  kept?: TakeInfo;
  /** The take the picture shows: the one in review, else the kept one. */
  shown?: TakeInfo;
  /** The picture is the live camera: recording, or armed with no take waiting in review. */
  live: boolean;
  empty: boolean;
  info?: SlotInfo;
  takes: TakeInfo[];
  /** While Live plays a take back: the take the picture shows now (a comp can switch it), or "gap" where no clip plays. */
  playing?: number | "gap";
  /** A Griddy owns this slot, so the slot hears its track and follows edits and comps. */
  follows: boolean;
};

export function slotViews(engine: EngineStatus | undefined, transport: Transport): SlotView[] {
  if (!engine) return [];
  const own = engine.master ? -1 : engine.slot;
  const count = Math.max(1, engine.layout?.cells?.length || engine.slots.length);

  return Array.from({ length: count }, (_, slot) => {
    const info = engine.slots.find((s) => s.slot === slot);
    const takes = engine.takes[slot] ?? [];
    const review = takes.find((t) => t.state === "review");
    const kept = takes.find((t) => t.state === "kept");
    const recording = !!info?.recording;
    const armed = slot === own ? engine.armed : !!info?.armed;
    const live = recording || (armed && !review);
    const waiting = armed && !recording && transport.ppq < 0;
    const claimed = slot === own || !!info?.claimed;
    const playing = transport.playing && !live && (review || kept) && info
      ? (info.gap ? "gap" as const : info.takeId >= 0 ? info.takeId : undefined)
      : undefined;
    return {
      slot,
      color: slotColor(slot),
      name: info?.trackName || (claimed ? `Track ${slot + 1}` : "No track"),
      claimed,
      mine: slot === own,
      armed,
      recording,
      waiting,
      review,
      kept,
      shown: review ?? kept,
      live,
      empty: !review && !kept && !live,
      info,
      takes,
      playing,
      follows: claimed,
    };
  });
}

/** The words for a slot whose takes play but no Griddy hears its track. */
export const FOLLOWS_POSITION = "Follows Live's position. Put Griddy on this track to follow edits and comps.";

/** What a tile's chip says, and what a screen reader hears. */
export function playingChip(v: SlotView): { text: string; label: string } | undefined {
  if (v.playing === undefined) return undefined;
  if (v.playing === "gap") return { text: "NO CLIP", label: `Slot ${v.slot + 1} has no clip here` };
  return { text: `TAKE ${v.playing}`, label: `Slot ${v.slot + 1} is showing take ${v.playing}` };
}

/** The status badge, in the mockup's words and colours. */
export function badgeFor(v: SlotView, density: "full" | "compact" | "tiny"): Badge {
  let b: Badge = { t: "EMPTY", bg: "#3a3a42", fg: "#d8d6cc" };
  if (v.recording) b = { t: "● REC", bg: C.blood, fg: "#fff" };
  else if (v.waiting) b = { t: "GET READY", bg: C.gold, fg: C.ink };
  else if (v.review) b = { t: "REVIEW", bg: C.gold, fg: C.ink };
  else if (v.live) b = { t: "ARMED · LIVE", bg: C.hot, fg: C.ink };
  else if (v.kept) b = { t: "✓ APPROVED", bg: C.lime, fg: C.ink };
  if (density !== "full") b = { ...b, t: ({ "GET READY": "READY", "ARMED · LIVE": "LIVE", "✓ APPROVED": "✓" } as Record<string, string>)[b.t] || b.t };
  return b;
}

/** Length of a take in quarter notes. */
export const takeBeats = (t: TakeInfo) => (t.seconds * (t.bpm || 120)) / 60;

/** Where in a take the playhead is, 0..1, with its slip. */
export function takeProgress(t: TakeInfo | undefined, ppq: number) {
  if (!t) return 0;
  const start = (t.startPpq ?? 0) + (t.slip * (t.bpm || 120)) / 60;
  return Math.min(1, Math.max(0, (ppq - start) / Math.max(1e-6, takeBeats(t))));
}

/** Beats of Live's count in still to go, when the host reports a position before the song start. */
export function countIn(transport: Transport) {
  if (transport.ppq >= 0) return 0;
  return Math.max(1, Math.ceil(-transport.ppq / meter(transport.timeSig).beat));
}

// ---- waveform peaks, read once per take ----
const peakCache = new Map<string, number[]>();
const pending = new Set<string>();
const listeners = new Set<() => void>();

export function usePeaks(slot: number, take: TakeInfo | undefined): number[] {
  const key = take ? `${slot}:${take.id}` : "";
  const [, bump] = useState(0);
  useEffect(() => {
    const fn = () => bump((n) => n + 1);
    listeners.add(fn);
    return () => void listeners.delete(fn);
  }, []);
  useEffect(() => {
    if (!take || peakCache.has(key) || pending.has(key)) return;
    pending.add(key);
    api.takePeaks(slot, take.id).then((p) => {
      pending.delete(key);
      peakCache.set(key, Array.isArray(p) ? p : []);
      listeners.forEach((fn) => fn());
    }, () => pending.delete(key));
  }, [key]);
  return peakCache.get(key) ?? [];
}
