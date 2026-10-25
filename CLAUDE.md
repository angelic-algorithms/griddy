# Griddy project rules

## Location
- This folder IS the installed device. Live loads `Griddy.amxd` from here
  (`~/Music/Ableton/User Library/Presets/Audio Effects/Max Audio Effect/Griddy`).
- `Griddy.amxd` and `griddy.js` must stay side by side at the repo root.

## griddy.js
- Runs in Max's legacy `[js]` engine: ES5 only. Use `var`; no `let`/`const`,
  arrow functions, classes or template literals.
- Check syntax with `node --check griddy.js`.
- `autowatch` is 0 on purpose. After editing `griddy.js`, Christopher clicks
  **Reload Engine** on the device. After editing `Griddy.amxd`, he must reopen the Live set.

## Griddy.amxd
- Source of truth for the patch (no build script).
- Format: `"ampf"` + uint32 LE 4 + `"aaaa"` + `"meta"` + uint32 LE 4 + 4 bytes +
  `"ptch"` + uint32 LE length + patcher JSON + `"\n\0"`.
- To edit: write a small script that parses the JSON, changes it, and rewrites the
  header with the correct ptch length.
- After any patch edit verify: JSON parses, every patchline source/destination id
  exists, outlet index < numoutlets, inlet index < numinlets.

## State
- Persistent state lives in `~/Music/Ableton/Griddy Takes/griddy_data.json`, not in
  the Live set (Live's device storage was unreliable).
- New settings go in the `state` object in `griddy.js` and are merged in `applyState()`.
- Never commit `griddy_data.json` or any `.mov`.

## Max and Live API
- Live API edits can't happen inside observer callbacks; do them from the frame tick.
- Free Jitter objects with `freepeer()`. Keep Max Tasks in globals.
- Only one Griddy per set (singleton guard).
- Device height is capped at about 169 px by Live, so grow sideways, not down.

## Testing and communication
- Nobody can run Live or Max from here. For each change, describe exactly what
  Christopher should click and look for, then wait for his result.
- Christopher prefers no hyphens in prose written for him.
