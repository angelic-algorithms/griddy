import { useEffect, useState, useSyncExternalStore } from "react";
import { onEvent } from "./bridge";

/** Subscribe a component to a parameter. Re-renders when the DAW, automation or another control changes it. */
export function useParam<T>(param: { get(): T; subscribe(fn: () => void): () => void }): T {
  return useSyncExternalStore(param.subscribe, param.get, param.get);
}

/** Latest payload of an event pushed from C++, or the initial value until the first one arrives. */
export function useBackendEvent<T>(name: "transport" | "preview" | "status", initial: T): T {
  const [value, setValue] = useState<T>(initial);
  useEffect(() => onEvent<T>(name, setValue), [name]);
  return value;
}
