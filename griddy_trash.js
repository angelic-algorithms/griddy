// Griddy helper for [node.script griddy_trash.js] (Node for Max, not the
// legacy [js] engine, so modern JavaScript is fine here).
//
// griddy.js can't move files, so it sends "trash <path>" here through
// [r griddy_trash_cmd]. Files go to the Finder Trash (Put Back works).
// Replies "trashresult ok|failed <path>" to griddy.js.
// The first time, macOS asks to let Live control Finder; click OK.

const maxApi = require("max-api");
const { execFile } = require("child_process");

// Max paths look like "T7:/ableton/take.mov"; Finder needs /Volumes/T7/...
function posixPath(p) {
    const m = /^([^:\/]+):(\/.*)$/.exec(p);
    return m ? "/Volumes/" + m[1] + m[2] : p;
}

maxApi.addHandler("trash", (...parts) => {
    const path = posixPath(parts.join(" "));
    const script = [
        "on run argv",
        'tell application "Finder" to delete (POSIX file (item 1 of argv) as alias)',
        "end run"
    ];
    const args = [];
    script.forEach((line) => args.push("-e", line));
    args.push(path);
    execFile("osascript", args, (err) => {
        if (err) maxApi.post("griddy_trash: " + err.message);
        maxApi.outlet("trashresult", err ? "failed" : "ok", path);
    });
});
