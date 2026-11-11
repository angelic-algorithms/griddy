import { useEffect, useRef } from "react";
import { onEvent, previewUrl } from "./bridge";

/**
 * Shows the collage exactly as it will be recorded.
 *
 * C++ composites every frame and emits a "preview" event with a serial number
 * when a new JPEG is ready. We fetch it, decode it off the main thread with
 * createImageBitmap, and draw it to a canvas. Drawing to a canvas (rather than
 * swapping an <img> src) avoids a flash between frames.
 *
 * Only one fetch is in flight at a time: if frames arrive faster than we can
 * draw, we skip straight to the newest one instead of queueing.
 */
export function Preview() {
  const canvasRef = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    let busy = false;
    let pending: number | null = null;
    let cancelled = false;

    const draw = async (serial: number) => {
      busy = true;
      try {
        const res = await fetch(previewUrl(serial), { cache: "no-store" });
        if (!res.ok) return;
        const bitmap = await createImageBitmap(await res.blob());
        const canvas = canvasRef.current;
        if (!cancelled && canvas) {
          if (canvas.width !== bitmap.width) canvas.width = bitmap.width;
          if (canvas.height !== bitmap.height) canvas.height = bitmap.height;
          canvas.getContext("2d")?.drawImage(bitmap, 0, 0);
        }
        bitmap.close();
      } catch {
        // A missed frame is fine; the next event brings a newer one.
      } finally {
        busy = false;
        if (pending !== null && !cancelled) {
          const next = pending;
          pending = null;
          void draw(next);
        }
      }
    };

    const stop = onEvent<number>("preview", (serial) => {
      if (busy) pending = serial;
      else void draw(serial);
    });

    return () => {
      cancelled = true;
      stop();
    };
  }, []);

  return <canvas ref={canvasRef} className="preview" width={640} height={360} />;
}
