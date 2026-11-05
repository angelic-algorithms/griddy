// =====================================================================
// GRIDDY engine for Max for Live  (v2: videos follow clips)
//
// Every video is tied to the AUDIO CLIP it was recorded with (by the
// clip's audio file). Move the clip to another track, trim it, duplicate
// it, and the video follows. The grid shows one tile per track that has
// video clips on it, so it builds up as you record and move takes.
//
// Lives inside Griddy.amxd as [js griddy.js]. Only one Griddy per set.
//
// Inlet messages:
//   init, frame, jit_matrix <name>, restore <json>, select <i>,
//   importvideo <path>, cleartake, folder <path>, offset <ms>,
//   follow <0|1>, window <0|1>, fullscreen <0|1>, camera <name>,
//   colors <0|1>, bgswatch <i>, accentswatch <i>, bghex <hex>, accenthex <hex>
// Outlets:
//   0 state -> pattr   1 recorder -> jit.record   2 track menu
//   3 status line      4 ui sync   5 camera -> jit.grab   6 camera menu
// =====================================================================

autowatch = 0;
inlets = 1;
outlets = 7;

var OUT_STATE = 0, OUT_REC = 1, OUT_TRACKS = 2, OUT_STATUS = 3, OUT_UI = 4, OUT_GRAB = 5, OUT_CAMMENU = 6;

// ---------- settings ----------
var CTX = "griddy";
var WIN_W = 1280, WIN_H = 720;
var ASPECT = WIN_W / WIN_H;
var TILE_W = 640, TILE_H = 360;
var DRIFT_MS = 80;     // only seek when a video drifts more than this
var GAP = 0.96;        // tile size inside its grid cell
var SCAN_EVERY = 15;   // frames between scans of the set (about twice a second)
var CAM_TIMEOUT = 3000; // ms without a camera frame before we reconnect

// ---------- saved state ----------
// videos:  audio file path -> { video, tempo, leadMs }
//   leadMs = how far the video's start sits after the audio file's start
// camera:  the camera to open with this set
// offsets: camera name -> video offset in ms (an iPhone over WiFi lags more
//          than a capture card, so each camera keeps its own)
// bg:      "#RRGGBB" for the grid window and device panel ("" = Live's theme)
// accent:  "#RRGGBB" for toggles and the Colors button ("" = Live's theme)
var state = { folder: "", follow: 1, videos: {}, camera: "", offsets: {}, bg: "", accent: "" };

// ---------- colors ----------
// Same swatches as the Flow version. The patch's swatch buttons send
// "bgswatch <i>" / "accentswatch <i>", so these lists are the only place
// the palette lives.
var BG_SWATCHES = ["#DCCFA6", "#8A9A70", "#D8A07A", "#C2CBD4", "#E9E3D2", "#1C1C1E", "#1C2836"];
var ACCENT_SWATCHES = ["#166534", "#9F1239", "#EA580C", "#7C3AED", "#38BDF8", "#E5E7EB"];
// Panel labels that switch between dark and light text to stay readable
var LABELS = ["lbl_cam", "lbl_track", "lbl_offset", "lbl_window", "lbl_full", "lbl_follow",
              "lbl_bgcol", "lbl_accent"];
var TOGGLES = ["tog_window", "tog_full", "tog_follow"];
// Everything inside the Colors drop down, hidden until Colors is clicked
var COLOR_ROW = ["lbl_bgcol", "hex_bg", "lbl_accent", "hex_ac"];
var colorsOpen = 0;
var PATCH = this.patcher;

// ---------- runtime ----------
var ready = false;
var song = null, probe = null, cp = null;
var observers = [];
var win = null, render = null;
var tracks = [];       // every armable track: { id, index, name, armed, clips: [...] }
var menuSig = "", gridSig = "";
var tiles = {};        // track id -> tile
var shown = [];        // tracks currently in the grid, in set order
var camMatrixName = null;
var selected = 0;
var recording = null;  // { ids, path, startBeat, tempo }
var isPlaying = 0, recordMode = 0, sessionRecord = 0, tempo = 120;
var frameCount = 0;
var lastCamFrame = 0, lastReopen = 0, camDropped = false;
var camTask = null;        // kept global: Max can drop a Task held only in a local
var pending = [];          // finished recordings still waiting for their clip
var videoIndex = null;     // takes folder: [{ name, secs }] for auto linking
var videoIndexAt = 0;
var PENDING_GIVEUP = 15000; // ms to keep looking for a new clip

// Griddy keeps its memory in its own file (Live's device storage proved
// unreliable). Videos are keyed by each clip's audio file, which is unique,
// so one file safely serves every set.
var HOME = "/Users/christopherwilliams";
var DATA_FILE = HOME + "/Music/Ableton/Griddy Takes/griddy_data.json";
var DEFAULT_TAKES = HOME + "/Music/Ableton/Griddy Takes/";
var dataDirty = false;
var folderCache = {};  // folder -> { list, at }

// Commands Claude can leave for Griddy (checked every couple of seconds)
var CMD_FILE = HOME + "/Music/Ableton/Griddy Takes/griddy_command.json";
var RESULT_FILE = HOME + "/Music/Ableton/Griddy Takes/griddy_result.json";
var CMD_MAX_AGE = 600;  // seconds; older commands are ignored

// The Griddy template: one track per part, one tile per track.
// Colors are Live palette style RGB values.
var TEMPLATE_TRACKS = [
    ["Lead", 0xFF3636], ["Double", 0xF66C03], ["Harmony High", 0xFFF034],
    ["Harmony Low", 0xBFFB00], ["Ad Libs", 0x1AFF2F], ["Omnichord", 0x5480E4],
    ["Guitar", 0x92A7FF], ["Keys", 0xD86CE4], ["Beat", 0x878787]
];

// Only one Griddy may run per set: copies would share one window and camera
var G = new Global("griddy_singleton");
var myId = Math.random();
function ownerAlive() {
    return G.owner && G.owner !== myId && (Date.now() - (G.beat || 0)) < 2000;
}

// =====================================================================
// Startup
// =====================================================================
function init() {
    if (ownerAlive()) {
        status("Griddy is already running on another track. Delete this copy and keep the one on Main.");
        return;
    }
    G.owner = myId; G.beat = Date.now();

    if (!win) {
        win = new JitterObject("jit.window", CTX);
        win.size = [WIN_W, WIN_H];
        win.title = "Griddy";
        win.visible = 1;
        render = new JitterObject("jit.gl.render", CTX);
        render.erase_color = [0, 0, 0, 1];
        render.ortho = 2;
    }

    song = new LiveAPI("live_set");
    probe = new LiveAPI("live_set");
    cp = new LiveAPI("live_set");
    observers = [];
    watch("is_playing", function (v) { isPlaying = v; checkRecord(); });
    watch("record_mode", function (v) { recordMode = v; checkRecord(); });
    watch("session_record", function (v) { sessionRecord = v; checkRecord(); });
    watch("tempo", function (v) { tempo = v; });

    loadData();
    colors(colorsOpen);
    uimsg("btn_colors", "set", colorsOpen);  // after Reload Engine, match the button
    applyColors();
    ready = true;
    scanSet();
    pushUI();
    if (state.camera) {
        // give the camera list a moment to fill in first
        camTask = new Task(function () { openCamera(); }, this);
        camTask.schedule(500);
    }
    status(state.folder ? "Ready" : "Ready. Pick a camera and choose a takes folder.");
}

function watch(prop, fn) {
    var api = new LiveAPI(function (args) {
        if (args[0] === prop) fn(Number(args[1]));
    }, "live_set");
    api.property = prop;
    observers.push(api);
}

// =====================================================================
// Reading the set
// =====================================================================
function str(v) {
    if (v instanceof Array) return v.join(" ");
    return String(v);
}

// Reads every armable track, its arm state, and its arrangement audio
// clips that have a video attached.
function scanSet() {
    if (!song) return;
    var count = song.getcount("tracks");
    var list = [];
    for (var i = 0; i < count; i++) {
        probe.path = "live_set tracks " + i;
        if (Number(probe.get("can_be_armed")) !== 1) continue; // group tracks
        var t = {
            id: Number(probe.id),
            index: i,
            name: str(probe.get("name")),
            armed: Number(probe.get("arm")) === 1,
            clips: []
        };
        var n = probe.getcount("arrangement_clips");
        for (var c = 0; c < n; c++) {
            cp.path = "live_set tracks " + i + " arrangement_clips " + c;
            if (Number(cp.get("is_audio_clip")) !== 1) continue;
            var f = str(cp.get("file_path"));
            if (!state.videos[f] && !autoLink(f)) continue;
            t.clips.push({
                start: Number(cp.get("start_time")),
                end: Number(cp.get("end_time")),
                marker: Number(cp.get("start_marker")),
                file: f
            });
        }
        list.push(t);
    }
    tracks = list;
    updateMenu();
    updateGrid();
    processPending();
}

function updateMenu() {
    var sig = tracks.map(function (t) { return t.id + ":" + t.name; }).join("\n");
    if (sig === menuSig) return;
    menuSig = sig;
    outlet(OUT_TRACKS, "clear");
    for (var j = 0; j < tracks.length; j++) outlet(OUT_TRACKS, "append", tracks[j].name);
    if (selected >= tracks.length) selected = 0;
}

// The grid holds every track that has video clips, plus armed tracks
// (so you can see the camera before you record).
function updateGrid() {
    shown = tracks.filter(function (t) {
        return t.clips.length > 0 || (t.armed && camMatrixName);
    });
    var sig = shown.map(function (t) { return t.id; }).join(",");
    if (sig === gridSig) return;
    gridSig = sig;

    var keep = {};
    for (var i = 0; i < shown.length; i++) {
        keep[shown[i].id] = true;
        if (!tiles[shown[i].id]) tiles[shown[i].id] = makeTile();
    }
    for (var key in tiles) {
        if (!keep[key]) { freeTile(tiles[key]); delete tiles[key]; }
    }
    layout();
}

// Square-ish grid: 1 = full, 2 = side by side, 4 = 2x2, 6 = 3x2, 9 = 3x3 ...
function layout() {
    var n = shown.length;
    if (!n) return;
    var cols = Math.ceil(Math.sqrt(n));
    var rows = Math.ceil(n / cols);
    var cw = (2 * ASPECT) / cols, ch = 2 / rows;
    var w = cw * GAP, h = w * 9 / 16;
    if (h > ch * GAP) { h = ch * GAP; w = h * 16 / 9; }
    for (var i = 0; i < n; i++) {
        var c = i % cols, r = Math.floor(i / cols);
        var p = tiles[shown[i].id].plane;
        p.position = [-ASPECT + cw * (c + 0.5), 1 - ch * (r + 0.5), 0];
        p.scale = [w / 2, h / 2, 1];
    }
}

function makeTile() {
    var plane = new JitterObject("jit.gl.videoplane", CTX);
    plane.enable = 0;
    return {
        plane: plane,
        matrix: new JitterMatrix(4, "char", TILE_W, TILE_H),
        movies: {},      // video path -> jit.movie (one per take used on this track)
        current: null,   // the movie playing right now
        running: false
    };
}

function freeTile(t) {
    for (var p in t.movies) t.movies[p].freepeer();
    t.plane.freepeer();
    t.matrix.freepeer();
}

function movieFor(tile, path) {
    if (!tile.movies[path]) {
        var m = new JitterObject("jit.movie");
        m.autostart = 0;
        m.loop = 0;
        m.adapt = 0;
        m.dim = [TILE_W, TILE_H];
        m.read(path);
        tile.movies[path] = m;
    }
    return tile.movies[path];
}

// =====================================================================
// Every frame
// =====================================================================
function frame() {
    try { frameInner(); } catch (e) { report("frame", e); }
}

function frameInner() {
    if (!ready) return;
    if (G.owner !== myId) {
        if (ownerAlive()) return;
        G.owner = myId; // the old copy is gone, take over
    }
    G.beat = Date.now();

    frameCount++;
    if (frameCount % SCAN_EVERY === 0) { scanSet(); cameraWatchdog(); }
    if (dataDirty && frameCount % 30 === 0) writeData();
    if (frameCount % 60 === 0) checkCommands();

    var beat = Number(song.get("current_song_time"));
    for (var i = 0; i < shown.length; i++) drawTile(shown[i], beat);

    render.erase();
    render.drawclients();
    render.swap();
}

function clipAt(t, beat) {
    var hit = null;
    for (var i = 0; i < t.clips.length; i++) {
        var c = t.clips[i];
        if (beat >= c.start && beat < c.end) hit = c;
    }
    return hit;
}

function drawTile(t, beat) {
    var tile = tiles[t.id];
    if (!tile) return;
    var clip = clipAt(t, beat);

    // Armed tracks show the live camera unless they're playing back a take
    if (camMatrixName && t.armed && (!isPlaying || isRecTarget(t.id) || !clip)) {
        stopCurrent(tile);
        tile.plane.enable = 1;
        tile.plane.jit_matrix(camMatrixName);
        return;
    }

    if (!clip) { stopCurrent(tile); tile.plane.enable = 0; return; }

    var v = state.videos[clip.file];
    var movie = movieFor(tile, v.video);
    if (tile.current !== movie) { stopCurrent(tile); tile.current = movie; }

    // Where is the audio file right now, and so where should the video be?
    var audioMs = (beat - clip.start + clip.marker) * 60000 / v.tempo;
    var target = audioMs + leadMs(v) + currentOffset();
    var dur = durationMs(movie);

    if (target < 0 || (dur > 0 && target > dur)) {
        stopCurrent(tile);
        tile.plane.enable = 0;
        return;
    }
    tile.plane.enable = 1;

    if (isPlaying) {
        if (!tile.running) {
            seekMs(movie, target);
            movie.start();
            tile.running = true;
        } else if (Math.abs(timeMs(movie) - target) > DRIFT_MS) {
            seekMs(movie, target); // loop wrap, locate, or slow drift
        }
    } else {
        if (tile.running) { movie.stop(); tile.running = false; }
        if (Math.abs(timeMs(movie) - target) > DRIFT_MS) seekMs(movie, target);
    }

    movie.matrixcalc(tile.matrix, tile.matrix);
    tile.plane.jit_matrix(tile.matrix.name);
}

// A take's audio and video start recording at the same moment, so the real
// lead is a few ms. Larger values came from reading the clip's position on
// the timeline (wrong after loop or punch recording, about 8 s off) and
// push the whole video past the end of its clip, so treat them as 0.
// Camera latency is handled by the per camera offset, not here.
var MAX_LEAD_MS = 1000;
function leadMs(v) {
    var l = Number(v.leadMs) || 0;
    return Math.abs(l) > MAX_LEAD_MS ? 0 : l;
}

function stopCurrent(tile) {
    if (tile.current && tile.running) tile.current.stop();
    tile.running = false;
    tile.current = null;
}

function timeMs(m) {
    var ts = Number(m.timescale);
    return ts > 0 ? Number(m.time) / ts * 1000 : 0;
}
function durationMs(m) {
    var ts = Number(m.timescale);
    return ts > 0 ? Number(m.duration) / ts * 1000 : 0;
}
function seekMs(m, ms) {
    var ts = Number(m.timescale);
    if (ts > 0) m.time = Math.max(0, Math.round(ms / 1000 * ts));
}

// =====================================================================
// Recording (follows Live's record button)
// =====================================================================
function checkRecord() {
    if (!ready || G.owner !== myId) return;
    var liveRec = isPlaying && (recordMode || sessionRecord);
    if (state.follow && liveRec && !recording) startRecording();
    else if (recording && !liveRec) stopRecording();
}

function startRecording() {
    if (!camMatrixName) { status("Live is recording but no camera is selected"); return; }
    scanSet();
    var folder = takesFolder();
    var ids = [], names = [];
    for (var i = 0; i < tracks.length; i++) {
        if (tracks[i].armed) { ids.push(tracks[i].id); names.push(tracks[i].name); }
    }
    if (!ids.length) return;

    var beat = Number(song.get("current_song_time"));
    var path = folder + safeName(names.join(" + ")) + " " + stamp() + ".mov";
    recording = { ids: ids, path: path, startBeat: beat, tempo: tempo };
    outlet(OUT_REC, "write", path);
    status("Recording video for " + names.join(", "));
}

function stopRecording() {
    outlet(OUT_REC, "stop");
    var r = recording;
    recording = null;
    r.stopBeat = Number(song.get("current_song_time"));
    r.at = Date.now();
    pending.push(r); // linked on the next scans, once Live has made the clip
    status("Saving take...");
}

// Keep trying to link finished recordings until Live's new clips appear
function processPending() {
    if (!pending.length) return;
    var still = [];
    for (var i = 0; i < pending.length; i++) {
        var r = pending[i];
        var age = Date.now() - r.at;
        if (age < 500) { still.push(r); continue; }
        var found = attachRecording(r);
        if (found) {
            save();
            status("Take saved. Move or copy the clip and the video follows it.");
        } else if (age > PENDING_GIVEUP) {
            status("Video saved, but no new clip was found. Record in Arrangement view.");
        } else {
            still.push(r);
        }
    }
    pending = still;
}

// Find the audio clip each armed track just recorded and tie the video to it.
// Returns how many clips were linked.
function attachRecording(r) {
    var found = 0;
    for (var k = 0; k < r.ids.length; k++) {
        if (r.done && r.done[r.ids[k]]) continue;
        var trackPath = pathForId(r.ids[k]);
        if (!trackPath) continue;
        probe.path = trackPath;
        var n = probe.getcount("arrangement_clips");
        var best = null, bestDist = 1e9;
        for (var c = 0; c < n; c++) {
            cp.path = trackPath + " arrangement_clips " + c;
            if (Number(cp.get("is_audio_clip")) !== 1) continue;
            var f = str(cp.get("file_path"));
            if (!f || state.videos[f]) continue; // no file yet, or already linked
            var start = Number(cp.get("start_time"));
            if (start < r.startBeat - 1 || start > Math.max(r.stopBeat, r.startBeat) + 1) continue;
            var d = Math.abs(start - r.startBeat);
            if (d < bestDist) {
                bestDist = d;
                best = { file: f, start: start, marker: Number(cp.get("start_marker")) };
            }
        }
        if (best) {
            var fileStartBeat = best.start - best.marker; // beat where the audio file begins
            state.videos[best.file] = {
                video: r.path,
                tempo: r.tempo,
                leadMs: (fileStartBeat - r.startBeat) * 60000 / r.tempo
            };
            r.done = r.done || {};
            r.done[r.ids[k]] = true;
            found++;
        }
    }
    return found;
}

// =====================================================================
// Auto linking by timestamp
// Live names recordings "REC 0006 [2026-10-22 193042].wav" and Griddy names
// videos "REC 20261022 193042.mov". A clip with no video gets the video
// that started within 2 seconds of it. This also recovers older takes.
// =====================================================================
function autoLink(file) {
    try { return autoLinkInner(file); }
    catch (e) { report("auto link", e); return false; }
}

var lastError = "";
function report(where, e) {
    var msg = "Griddy error (" + where + "): " + e.message + (e.lineNumber ? " line " + e.lineNumber : "");
    if (msg !== lastError) { lastError = msg; status(msg); post(msg + "\n"); }
}

function autoLinkInner(file) {
    if (!file) return false;
    if (pending.length) return false; // let the exact link finish first
    var m = /\[(\d{4})-(\d{2})-(\d{2}) (\d{2})(\d{2})(\d{2})\]/.exec(file);
    if (!m) return false;
    var secs = toSecs(m[1] + m[2] + m[3], m[4], m[5], m[6]);
    var vids = [];
    var folders = searchFolders(file);
    for (var fi = 0; fi < folders.length; fi++) vids = vids.concat(folderVideos(folders[fi]));
    var best = null, bestD = 3;
    for (var i = 0; i < vids.length; i++) {
        if (videoInUse(vids[i].path)) continue;
        var d = Math.abs(vids[i].secs - secs);
        if (d < bestD) { bestD = d; best = vids[i]; }
    }
    if (!best) return false;
    state.videos[file] = { video: best.path, tempo: tempo, leadMs: 0 };
    save();
    status("Linked " + best.name + " to its clip");
    return true;
}

function toSecs(ymd, hh, mm, ss) {
    var d = new Date(Number(ymd.substr(0, 4)), Number(ymd.substr(4, 2)) - 1, Number(ymd.substr(6, 2)),
        Number(hh), Number(mm), Number(ss));
    return d.getTime() / 1000;
}

function videoInUse(path) {
    for (var k in state.videos) if (state.videos[k].video === path) return true;
    return false;
}

// ----- folders -----
// Max likes "T7:/path" for external drives; Live gives "/Volumes/T7/path"
function maxPath(p) {
    var m = /^\/Volumes\/([^\/]+)\/(.*)$/.exec(p);
    return m ? m[1] + ":/" + m[2] : p;
}

// Project folder of a recorded clip: everything before "/Samples/"
function projectFolderOf(file) {
    var i = file.indexOf("/Samples/");
    return i > 0 ? maxPath(file.substring(0, i)) + "/" : null;
}

// Where new takes go: your chosen folder, else this project's folder,
// else Music/Ableton/Griddy Takes
function takesFolder() {
    if (state.folder) return state.folder;
    for (var i = 0; i < tracks.length; i++) {
        probe.path = "live_set tracks " + tracks[i].index;
        var n = probe.getcount("arrangement_clips");
        for (var c = 0; c < n; c++) {
            cp.path = "live_set tracks " + tracks[i].index + " arrangement_clips " + c;
            if (Number(cp.get("is_audio_clip")) !== 1) continue;
            var root = projectFolderOf(str(cp.get("file_path")));
            if (root) return root;
        }
    }
    return DEFAULT_TAKES;
}

function searchFolders(file) {
    var list = [];
    if (state.folder) list.push(state.folder);
    var root = projectFolderOf(file);
    if (root && list.indexOf(root) < 0) list.push(root);
    if (list.indexOf(DEFAULT_TAKES) < 0) list.push(DEFAULT_TAKES);
    return list;
}

// Take videos in a folder, refreshed every few seconds
function folderVideos(folder) {
    var c = folderCache[folder];
    if (c && Date.now() - c.at < 3000) return c.list;
    var list = [];
    var dir = new Folder(folder);
    dir.typelist = [];
    while (!dir.end) {
        var name = String(dir.filename);
        var ext = String(dir.extension || "");
        // Max may or may not include the extension in filename
        if (ext && name.slice(-ext.length).toLowerCase() !== ext.toLowerCase()) name = name + ext;
        var m = /(\d{8}) (\d{2})(\d{2})(\d{2})\.mov$/i.exec(name);
        if (m) list.push({ name: name, path: folder + name, secs: toSecs(m[1], m[2], m[3], m[4]) });
        dir.next();
    }
    dir.close();
    folderCache[folder] = { list: list, at: Date.now() };
    return list;
}

// ----- commands -----
function readText(path) {
    var f = new File(path, "read");
    if (!f.isopen) return "";
    var txt = "";
    while (f.position < f.eof) {
        var chunk = f.readstring(16000);
        if (!chunk) break;
        txt += chunk;
    }
    f.close();
    return txt;
}

function writeText(path, txt) {
    var f = new File(path, "write", "TEXT");
    if (!f.isopen) return false;
    f.position = 0;
    for (var i = 0; i < txt.length; i += 16000) f.writestring(txt.substring(i, i + 16000));
    f.eof = f.position;
    f.close();
    return true;
}

function checkCommands() {
    try {
        var txt = readText(CMD_FILE);
        if (!txt) return;
        var c = JSON.parse(txt);
        if (!c.id || c.id === state.lastCmd) return;
        state.lastCmd = c.id;
        save();
        // ids end in the unix time they were written; ignore anything stale
        // so an old command file can't fire when a new set is opened
        var m = String(c.id).match(/(\d{9,})$/);
        if (!m || Date.now() / 1000 - Number(m[1]) > CMD_MAX_AGE) {
            writeText(RESULT_FILE, JSON.stringify({ id: c.id, result: "ignored: stale command", at: String(new Date()) }));
            return;
        }
        var result = "unknown command";
        if (c.cmd === "build_template") result = buildTemplate();
        writeText(RESULT_FILE, JSON.stringify({ id: c.id, result: result, at: String(new Date()) }));
    } catch (e) { report("command", e); }
}

// Builds the Griddy template tracks in a fresh set. Refuses to touch a set
// that already has clips, so it can never wipe real work.
function buildTemplate() {
    // deleting the old tracks would delete Griddy itself if it sat on one
    var host = new LiveAPI("this_device canonical_parent");
    if (String(host.unquotedpath).indexOf("master_track") < 0) return "refused: put Griddy on the Main track first";
    var n0 = song.getcount("tracks");
    for (var i = 0; i < n0; i++) {
        probe.path = "live_set tracks " + i;
        if (probe.getcount("arrangement_clips") > 0) return "refused: this set already has clips";
        var slots = probe.getcount("clip_slots");
        for (var k = 0; k < slots; k++) {
            cp.path = "live_set tracks " + i + " clip_slots " + k;
            if (Number(cp.get("has_clip")) === 1) return "refused: this set already has clips";
        }
    }
    for (var t = 0; t < TEMPLATE_TRACKS.length; t++) {
        song.call("create_audio_track", -1);
        probe.path = "live_set tracks " + (song.getcount("tracks") - 1);
        probe.set("name", TEMPLATE_TRACKS[t][0]);
        probe.set("color", TEMPLATE_TRACKS[t][1]);
    }
    for (var d = n0 - 1; d >= 0; d--) song.call("delete_track", d);
    menuSig = ""; gridSig = "";
    scanSet();
    status("Griddy template tracks built");
    return "built " + TEMPLATE_TRACKS.length + " tracks";
}

// ----- Griddy's memory file -----
function loadData() {
    try {
        var f = new File(DATA_FILE, "read");
        if (!f.isopen) { post("Griddy: no saved data yet\n"); return; }
        var txt = "";
        while (f.position < f.eof) {
            var chunk = f.readstring(16000);
            if (!chunk) break;
            txt += chunk;
        }
        f.close();
        if (txt) applyState(JSON.parse(txt));
    } catch (e) { report("loading data", e); }
}

function writeData() {
    dataDirty = false;
    try {
        var f = new File(DATA_FILE, "write", "TEXT");
        if (!f.isopen) { status("Griddy can't write " + DATA_FILE); return; }
        var txt = JSON.stringify(state);
        f.position = 0;
        for (var i = 0; i < txt.length; i += 16000) f.writestring(txt.substring(i, i + 16000));
        f.eof = f.position;
        f.close();
    } catch (e) { report("saving data", e); }
}

function pathForId(id) {
    for (var i = 0; i < tracks.length; i++) {
        if (tracks[i].id === id) return "live_set tracks " + tracks[i].index;
    }
    return null;
}

function isRecTarget(id) {
    return recording && recording.ids.indexOf(id) >= 0;
}

function safeName(s) { return s.replace(/[\/:\\*?"<>|]/g, "_"); }

function stamp() {
    var d = new Date();
    function two(n) { return (n < 10 ? "0" : "") + n; }
    return d.getFullYear() + two(d.getMonth() + 1) + two(d.getDate()) + " " +
        two(d.getHours()) + two(d.getMinutes()) + two(d.getSeconds());
}

// =====================================================================
// Messages from the patch
// =====================================================================
function jit_matrix(name) {
    camMatrixName = name;
    lastCamFrame = Date.now();
    if (camDropped) { camDropped = false; status("Camera reconnected: " + state.camera); }
}

// =====================================================================
// Camera: iPhone (Continuity Camera), capture card, HDMI, webcam
// =====================================================================
function camera() {
    var name = arrayfromargs(arguments).join(" ");
    if (!name) return;
    state.camera = name;
    save();
    openCamera();
    pushUI(); // show this camera's offset
}

function openCamera() {
    if (!state.camera) return;
    outlet(OUT_GRAB, "close");
    outlet(OUT_GRAB, "vdevice", state.camera);
    outlet(OUT_GRAB, "open");
    outlet(OUT_CAMMENU, "setsymbol", state.camera);
    lastReopen = Date.now();
    lastCamFrame = Date.now();
}

// An iPhone drops out when you pick it up, unlock it, or take a call.
// If frames stop, keep trying to reopen it every few seconds.
function cameraWatchdog() {
    if (!state.camera) return;
    var now = Date.now();
    if (now - lastCamFrame < CAM_TIMEOUT || now - lastReopen < CAM_TIMEOUT) return;
    camDropped = true;
    status("Camera stopped sending video. Reconnecting " + state.camera + "...");
    openCamera();
}

function currentOffset() {
    return Number(state.offsets[state.camera || "default"]) || 0;
}

function select(i) { selected = Number(i); }

// The audio clip under the playhead on the selected track (or the nearest one)
function clipUnderPlayhead() {
    var t = tracks[selected];
    if (!t) return null;
    var beat = Number(song.get("current_song_time"));
    var trackPath = "live_set tracks " + t.index;
    probe.path = trackPath;
    var n = probe.getcount("arrangement_clips");
    var best = null, bestDist = 1e9;
    for (var c = 0; c < n; c++) {
        cp.path = trackPath + " arrangement_clips " + c;
        if (Number(cp.get("is_audio_clip")) !== 1) continue;
        var start = Number(cp.get("start_time")), end = Number(cp.get("end_time"));
        var d = (beat >= start && beat < end) ? 0 : Math.min(Math.abs(beat - start), Math.abs(beat - end));
        if (d < bestDist) {
            bestDist = d;
            best = { file: str(cp.get("file_path")), track: t.name };
        }
    }
    return best;
}

// Attach a video file to the audio clip under the playhead on the selected
// track. The video is lined up with the start of that clip's audio file.
function importvideo() {
    var path = arrayfromargs(arguments).join(" ");
    var clip = clipUnderPlayhead();
    if (!clip) { status("Put the playhead on an audio clip on the selected track first"); return; }
    state.videos[clip.file] = { video: path, tempo: tempo, leadMs: 0 };
    save();
    scanSet();
    status("Video attached to the clip on " + clip.track + ". Nudge Video offset if it's early or late.");
}

function cleartake() {
    var clip = clipUnderPlayhead();
    if (!clip || !state.videos[clip.file]) { status("No video on the clip under the playhead"); return; }
    delete state.videos[clip.file];
    save();
    scanSet();
    status("Removed the video from the clip on " + clip.track);
}

function folder() {
    var p = arrayfromargs(arguments).join(" ");
    if (p.charAt(p.length - 1) !== "/") p += "/";
    state.folder = p;
    save();
    status("Takes folder: " + p);
}

function offset(v) { state.offsets[state.camera || "default"] = Number(v); save(); }
function follow(v) { state.follow = Number(v) ? 1 : 0; save(); }
function window(v) { if (win) win.visible = Number(v) ? 1 : 0; }
function fullscreen(v) { if (win) win.fullscreen = Number(v) ? 1 : 0; }

function restore() {
    var text = arrayfromargs(arguments).join(" ");
    try { applyState(JSON.parse(text)); }
    catch (e) { post("Griddy: could not read saved state\n"); }
}

// Merge saved state in (from the memory file or the Live set)
function applyState(s) {
    if (!s) return;
    if (s.folder) state.folder = s.folder;
    if (s.follow !== undefined) state.follow = s.follow;
    var v = s.videos || {};
    for (var k in v) state.videos[k] = v[k];
    var o = s.offsets || {};
    for (var c in o) state.offsets[c] = o[c];
    if (s.offset && !state.offsets[s.camera || "default"]) state.offsets[s.camera || "default"] = Number(s.offset);
    var oldCam = state.camera;
    if (s.camera) state.camera = s.camera;
    if (s.lastCmd) state.lastCmd = s.lastCmd;  // so a command never runs twice
    if (s.bg) state.bg = s.bg;
    if (s.accent) state.accent = s.accent;
    applyColors();
    pushUI();
    if (ready && state.camera && state.camera !== oldCam) openCamera();
}

function save() { outlet(OUT_STATE, "set", JSON.stringify(state)); dataDirty = true; }
function status(text) { outlet(OUT_STATUS, "set", text); }

function pushUI() {
    outlet(OUT_UI, "offset", currentOffset());
    outlet(OUT_UI, "follow", state.follow);
    outlet(OUT_UI, "window", win ? 1 : 0);
}

// =====================================================================
// Colors
// =====================================================================
function colors(v) {
    colorsOpen = Number(v) ? 1 : 0;
    var names = COLOR_ROW.slice(0);
    for (var i = 0; i < BG_SWATCHES.length; i++) names.push("sw_bg_" + i);
    for (var j = 0; j < ACCENT_SWATCHES.length; j++) names.push("sw_ac_" + j);
    for (var k = 0; k < names.length; k++) {
        var o = ui(names[k]);
        if (o) o.hidden = colorsOpen ? 0 : 1;
    }
}

function bgswatch(i) { setColor("bg", BG_SWATCHES[Number(i)]); }
function accentswatch(i) { setColor("accent", ACCENT_SWATCHES[Number(i)]); }
function bghex() { setColor("bg", parseHex(arrayfromargs(arguments))); }
function accenthex() { setColor("accent", parseHex(arrayfromargs(arguments))); }

function setColor(key, hex) {
    if (!hex) { status("Type a color as six hex digits, like #C2CBD4"); applyColors(); return; }
    state[key] = hex;
    save();
    applyColors();
}

// textedit sends "text #C2CBD4"; a hex like 000000 can arrive as the number 0
function parseHex(args) {
    for (var i = 0; i < args.length; i++) {
        var a = args[i];
        if (typeof a === "number") a = ("000000" + Math.round(a)).slice(-6);
        var m = String(a).match(/^#?([0-9a-fA-F]{6})$/);
        if (m) return "#" + m[1].toUpperCase();
    }
    return null;
}

function rgb(hex) {
    var n = parseInt(hex.slice(1), 16);
    return [((n >> 16) & 255) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255];
}

// Dark text on light backgrounds, light text on dark ones
function contrast(c) {
    var lum = 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
    return lum > 0.55 ? [0.1, 0.1, 0.1] : [0.93, 0.93, 0.93];
}

function ui(name) { return PATCH ? PATCH.getnamed(name) : null; }

function uimsg(name) {
    var o = ui(name);
    if (o) o.message.apply(o, arrayfromargs(arguments).slice(1));
}

function applyColors() {
    var i, c;
    var bg = state.bg ? rgb(state.bg) : null;
    if (render) render.erase_color = bg ? [bg[0], bg[1], bg[2], 1] : [0, 0, 0, 1];

    var panel = ui("bg_panel");
    if (panel) {
        panel.hidden = bg ? 0 : 1;
        if (bg) panel.message("bgfillcolor", bg[0], bg[1], bg[2], 1);
    }
    var text = bg ? contrast(bg) : null;
    if (text) for (i = 0; i < LABELS.length; i++) uimsg(LABELS[i], "textcolor", text[0], text[1], text[2], 1);

    // paint the swatches from the lists above; the chosen one gets a ring
    var ring = text || [0.93, 0.93, 0.93];
    paintSwatches("sw_bg_", BG_SWATCHES, state.bg, ring);
    paintSwatches("sw_ac_", ACCENT_SWATCHES, state.accent, ring);

    if (state.accent) {
        var a = rgb(state.accent);
        for (i = 0; i < TOGGLES.length; i++) uimsg(TOGGLES[i], "checkedcolor", a[0], a[1], a[2], 1);
        uimsg("btn_colors", "bgoncolor", a[0], a[1], a[2], 1);
        uimsg("btn_colors", "bgoveroncolor", a[0], a[1], a[2], 1);
        c = contrast(a);
        uimsg("btn_colors", "textoncolor", c[0], c[1], c[2], 1);
    }
    uimsg("hex_bg", "set", state.bg || "");
    uimsg("hex_ac", "set", state.accent || "");
}

function paintSwatches(prefix, list, chosen, ring) {
    for (var i = 0; i < list.length; i++) {
        var o = ui(prefix + i);
        if (!o) continue;
        var c = rgb(list[i]);
        o.message("bgcolor", c[0], c[1], c[2], 1);
        o.message("bgovercolor", c[0], c[1], c[2], 1);
        var on = list[i] === chosen;
        o.message("border", on ? 2 : 1);
        o.message("bordercolor", on ? ring[0] : 0.4, on ? ring[1] : 0.4, on ? ring[2] : 0.4, 1);
    }
}

// Clean up when the device is removed
function teardown() {
    if (dataDirty) writeData();
    if (G.owner === myId) { G.owner = null; G.beat = 0; }
    ready = false;
    for (var k in tiles) freeTile(tiles[k]);
    tiles = {};
    gridSig = "";
    if (render) { render.freepeer(); render = null; }
    if (win) { win.freepeer(); win = null; }
}
function notifydeleted() { teardown(); }
function freebang() { teardown(); }
