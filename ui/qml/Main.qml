// Reelcut - a Basecamp view: paste a video address, get short reels with captions.
//
// Pure QML: the Basecamp 0.3 sandbox gives a view no network and no files outside its own
// install dir, so everything runs in the reelcut_core module (reelcut in a container, or a local
// reelcut). This view renders reelcut_core.snapshot(), polled, and loads the pictures the core
// copies under <this dir>/cache.
//
// One job at a time, in five screens with a pipeline bar over them: Video, Speakers, Clips,
// Framing, Reels. Each part of the state is kept in its own property and replaced only when it
// changed, so a poll does not rebuild lists that did not change; nothing animates continuously
// (a running step blinks once a second).
//
// Every core call goes through callVia (logos.callModuleAsync; never a blocking call in a view).
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import Logos.Theme
import Logos.Controls

Item {
    id: root
    width: 1280
    height: 820

    // ── colours: design-system tokens with fallbacks (a missing token is undefined, not fatal)
    function tok(v, fb) { return (v === undefined || v === null) ? fb : v }
    readonly property var pal: (typeof Theme !== "undefined" && Theme.palette) ? Theme.palette : ({})
    readonly property color cBg: tok(pal.background, "#141416")
    readonly property color cSurface: tok(pal.surface, "#1c1c20")
    readonly property color cRaised: tok(pal.surfaceRaised, "#26262c")
    readonly property color cText: tok(pal.text, "#ececf1")
    readonly property color cSub: tok(pal.textSecondary, "#a8a8b3")
    readonly property color cFaint: tok(pal.textTertiary, "#74747f")
    readonly property color cLine: tok(pal.borderHairline, "#2e2e36")
    readonly property color cAccent: tok(pal.primary, "#ff7a3d")
    readonly property color cOk: tok(pal.success, "#5fbf7f")
    readonly property color cBad: tok(pal.error, "#ef6f6c")
    readonly property color cWord: "#ffe14d"
    readonly property color cBlue: "#7fd1ff"
    // Each speaker's caption colour in the reels (reelcut's SPEAKER_COLOURS): A, B, C, …
    readonly property var speakerColours: ["#ffffff", "#8fd3ff", "#ffb86c", "#b9f18c", "#ffa0e0"]
    function speakerColour(letter) { return root.speakerColours[(String(letter).charCodeAt(0) - 65) % root.speakerColours.length] }
    // The boxes of a framing, one colour each.
    readonly property var panelColours: ["#ff7a3d", "#7fd1ff", "#b9f18c", "#ffa0e0", "#ffe14d", "#c9a0ff"]
    function panelColour(i) { return root.panelColours[i % root.panelColours.length] }
    readonly property int sp: 8

    // ── core bridge
    readonly property string coreName: "reelcut_core"
    readonly property int callTimeoutMs: 30000
    function _deliver(cb, raw) {
        if (!cb) return
        try { cb(raw === undefined || raw === null ? "" : raw) } catch (e) { console.warn("reelcut view: callback error: " + e) }
    }
    function callVia(module, method, args, cb) {
        var a = args || []
        if (typeof logos === "undefined" || logos === null) { Qt.callLater(function () { root._deliver(cb, "") }); return }
        if (typeof logos.callModuleAsync === "function") {
            try { logos.callModuleAsync(module, method, a, function (res) { root._deliver(cb, res) }, root.callTimeoutMs) }
            catch (e) { Qt.callLater(function () { root._deliver(cb, "") }) }
            return
        }
        Qt.callLater(function () {
            var raw = ""
            try { raw = (typeof logos.callModule === "function") ? logos.callModule(module, method, a) : "" } catch (e) { raw = "" }
            root._deliver(cb, raw)
        })
    }
    // Results may arrive as JSON, or JSON-quoted once or twice.
    function parse(raw) {
        var v = raw
        for (var i = 0; i < 3 && typeof v === "string"; i++) {
            var s = v.trim()
            if (s === "") return null
            try { v = JSON.parse(s) } catch (e) { return null }
        }
        return (v && typeof v === "object") ? v : null
    }

    // ── state: each part replaced only when it changed (a binding on it re-evaluates only then)
    property var st: ({ agents: [], settings: {}, runner: {} })   // everything but the jobs
    property var jobs: []          // the list on the left: a few fields per job
    property var allJobs: []       // the full jobs, not bound to by the UI
    property var job: null         // the open job, without the parts below
    property var jobLog: []
    property var clips: []
    property var chosen: []
    property var reels: []
    property var speakers: ({})
    property var scenes: ({})      // {state, error, i, n, clips: {n: {start, end, scenes: [...]}}}
    property var framing: ({})     // {n: {scenes: [...]}} saved by hand
    property var cards: []
    property var _seen: ({})
    function keep(name, v) {
        var s = JSON.stringify(v === undefined ? null : v)
        if (root._seen[name] === s) return
        root._seen[name] = s
        root[name] = v
    }
    property bool coreSeen: false
    property int missedPolls: 0
    property bool busy: false
    property bool again: false
    property string selectedId: ""
    readonly property bool coreLost: root.coreSeen && root.missedPolls >= 4
    readonly property bool anyRunning: root.jobs.some(function (j) { return j.status === "running" || j.status === "queued" || j.scenes === "running" })
    readonly property bool jobRunning: !!root.job && (root.job.status === "running" || root.job.status === "queued")

    readonly property var heavyKeys: ["log", "clips", "chosen", "reels", "speakers", "scenes", "framing"]
    function apply(obj) {
        if (!obj) return
        if (obj.ok === false) { toast(obj.error || "That did not work.", true); return }
        if (obj.jobs === undefined) return
        var rest = {}
        for (var k in obj) if (k !== "jobs") rest[k] = obj[k]
        keep("st", rest)
        keep("cards", obj.cards || [])
        root.allJobs = obj.jobs
        keep("jobs", obj.jobs.map(function (j) {
            return { id: j.id, title: j.title, url: j.url, status: j.status, step: j.step, activity: j.activity,
                     progress: j.progress, duration: j.duration, reels: (j.reels || []).length,
                     clips: (j.clips || []).length, scenes: j.scenes ? j.scenes.state || "" : "",
                     scenesI: j.scenes ? j.scenes.i || 0 : 0, scenesN: j.scenes ? j.scenes.n || 0 : 0 }
        }))
        syncJob()
        root.coreSeen = true
        root.missedPolls = 0
        if (!obj.thumbsOk && !root.sandboxImages) registerViewDir()
    }
    function syncJob() {
        var j = null
        for (var i = 0; i < root.allJobs.length; i++) if (root.allJobs[i].id === root.selectedId) j = root.allJobs[i]
        if (!j && root.allJobs.length) j = root.allJobs[0]
        var meta = null
        if (j) {
            meta = {}
            for (var k in j) if (root.heavyKeys.indexOf(k) < 0) meta[k] = j[k]
        }
        keep("job", meta)
        keep("jobLog", j ? j.log || [] : [])
        keep("clips", j ? j.clips || [] : [])
        keep("chosen", j ? j.chosen || [] : [])
        keep("reels", j ? j.reels || [] : [])
        keep("speakers", j ? j.speakers || {} : {})
        keep("scenes", j ? j.scenes || {} : {})
        keep("framing", j ? j.framing || {} : {})
        if (j && !root.pinned[j.id]) {
            var t = root.autoTab(j)
            if (t !== root.tab) root.tab = t
        }
    }
    onSelectedIdChanged: syncJob()
    function refresh() {
        if (root.busy) { root.again = true; return }
        root.busy = true
        callVia(coreName, "snapshot", [], function (raw) {
            var o = parse(raw)
            if (o) apply(o)
            else root.missedPolls++
            root.busy = false
            if (root.again) { root.again = false; root.refresh() }
        })
    }
    // Every action: the core answers with the fresh state, or {ok:false,error}.
    function act(method, args, okMsg, after) {
        callVia(coreName, method, args, function (raw) {
            var o = parse(raw)
            if (!o) { toast("No answer from reelcut_core - is it installed and loaded?", true); return }
            if (o.ok === false) { toast(o.error || "That did not work.", true); return }
            apply(o)
            if (okMsg) toast(okMsg, false)
            if (after) after(o)
        })
    }
    function viewDir() {
        var u = String(Qt.resolvedUrl("."))
        if (u.indexOf("file://") === 0) u = decodeURIComponent(u.substring(7))
        return u
    }
    property bool viewDirSent: false
    function registerViewDir() {
        if (root.viewDirSent) return
        root.viewDirSent = true
        callVia(coreName, "setViewDir", [viewDir()], function (raw) { var o = parse(raw); if (o) apply(o) })
    }
    function fileUrl(path) {
        return "file://" + String(path).split("/").map(function (seg) { return encodeURIComponent(seg) }).join("/")
    }
    // Pictures: through the host's SandboxImages where Basecamp has it (newer than 0.3.2): the core
    // hands the bytes (picture), the view shows them from memory, nothing is written into the
    // view's install folder. Older hosts: copies the core puts under this view's folder.
    readonly property bool sandboxImages: typeof SandboxImages !== "undefined" && SandboxImages !== null && typeof SandboxImages.sourceFor === "function"
    property var pics: ({})        // picture ref ("path#stamp") -> image source
    property var picsAsked: ({})
    property int picsCount: 0
    function pic(ref) {
        if (!ref) return ""
        if (!root.sandboxImages) return root.fileUrl(ref)
        var have = root.pics[ref]
        if (have) return have
        if (!root.picsAsked[ref]) {
            root.picsAsked[ref] = true
            callVia(coreName, "picture", [ref], function (raw) {
                var o = parse(raw)
                if (!o || !o.ok || !o.data) return
                var src = ""
                try { src = SandboxImages.sourceFor(o.data) } catch (e) { return }
                if (!src) return
                // A long session: let go of everything now and then (the host caps memory per view).
                if (root.picsCount > 400) {
                    for (var k in root.pics) { try { SandboxImages.release(root.pics[k]) } catch (e2) {} }
                    root.pics = ({}); root.picsAsked = ({}); root.picsCount = 0
                    root.picsAsked[ref] = true
                }
                var n = Object.assign({}, root.pics)
                n[ref] = src
                root.picsCount++
                root.pics = n
            })
        }
        return ""
    }

    // ── the screens: 0 Video, 1 Speakers, 2 Clips, 3 Framing, 4 Reels. A job's screen follows its
    // progress until the person picks one; from then on it stays where they put it.
    property int tab: 0
    property var pinned: ({})
    function show(t) {
        var p = Object.assign({}, root.pinned)
        if (root.job) p[root.job.id] = true
        root.pinned = p
        root.tab = t
    }
    function autoTab(j) {
        if (j.step === "speakers") return 1
        if (j.step === "pick" || j.step === "clips") return 2
        if (j.step === "cut" || j.step === "thumbs" || j.step === "done") return 4
        return 0
    }
    // The framing screen opens on this clip.
    property int frameClip: 0
    function reframe(n) { root.frameClip = n; root.show(3) }

    // ── actions
    function start() {
        var u = urlField.text.trim()
        if (!/^https?:\/\//i.test(u)) { toast("Paste a video address that starts with http:// or https://.", true); return }
        var brief = briefNew.text.trim()
        act("startJob", [u], "", function (o) {
            urlField.text = ""
            if (o.jobs && o.jobs.length) { root.selectedId = o.jobs[0].id; root.setBrief(o.jobs[0].id, brief) }
        })
    }
    function importUrl(u) {
        var brief = briefNew.text.trim()
        act("importFile", [String(u)], "", function (o) { if (o.jobs && o.jobs.length) { root.selectedId = o.jobs[0].id; root.setBrief(o.jobs[0].id, brief) } })
    }
    // Who can pick the clips: an agent on this computer (shrooms-agent, no mesh needed), sessions on the
    // mesh, or nobody (the person copies the prompt into a chat). [{value, label}]
    function pickerOptions(withNobody) {
        var o = [], la = root.st.localAgent || {}, hs = la.harnesses || []
        if (la.ok) {
            o.push({ value: "auto", label: "Automatic: an agent on this computer" })
            if (hs.indexOf("claude") >= 0) o.push({ value: "local:claude", label: "Claude Code on this computer (your Claude login)" })
            if (hs.indexOf("pi") >= 0) o.push({ value: "local:pi", label: "pi on this computer (pi's own model and key)" })
        }
        ;(root.st.agents || []).forEach(function (a) {
            if (!/\/reelcut-(claude|pi)$/.test(a.name)) o.push({ value: a.name, label: a.name + " (on the mesh)" })
        })
        if (withNobody) o.push({ value: "", label: "Nobody: I paste from an AI chat" })
        return o
    }
    property var pickOpts: []
    onStChanged: keep("pickOpts", pickerOptions(false))
    readonly property var pickOptsAll: root.pickOpts.concat([{ value: "", label: "Nobody: I paste from an AI chat" }])
    function pickerLabel(v) {
        if (v === "auto") return "an agent on this computer"
        if (v === "local:claude") return "Claude Code on this computer"
        if (v === "local:pi") return "pi on this computer"
        return v
    }
    function setBrief(id, text, after) {
        callVia(coreName, "setJob", [id, JSON.stringify({ brief: text })], function (raw) { var o = parse(raw); if (o && o.jobs) apply(o); if (after) after() })
    }
    // The layouts cut --frame takes for clips not framed by hand: "" lets reelcut choose.
    readonly property var layouts: [
        { value: "", label: "Automatic" },
        { value: "fit", label: "The whole picture" },
        { value: "stack", label: "Two people, one above the other" },
        { value: "speaker", label: "Follow whoever speaks" },
        { value: "crop", label: "Crop the middle" }
    ]
    function layoutValue(i) { return i >= 0 && i < root.layouts.length ? root.layouts[i].value : "" }
    function layoutIndex(v) {
        for (var i = 0; i < root.layouts.length; i++) if (root.layouts[i].value === (v || "")) return i
        return 0
    }
    // The reels' shape (cut --ratio): per job.
    readonly property var ratios: [
        { value: "9:16", label: "Vertical 9:16", w: 1080, h: 1920 },
        { value: "4:5", label: "Portrait 4:5", w: 1080, h: 1350 },
        { value: "1:1", label: "Square 1:1", w: 1080, h: 1080 }
    ]
    function ratioIndex(v) {
        for (var i = 0; i < root.ratios.length; i++) if (root.ratios[i].value === v) return i
        return 0
    }
    readonly property var jobRatio: root.ratios[root.ratioIndex(root.job ? root.job.ratio || "9:16" : "9:16")]
    function framedByHand(n) { return !!root.framing[String(n)] }
    readonly property int framedCount: Object.keys(root.framing).length

    function copyText(t) { clipEdit.text = t; clipEdit.selectAll(); clipEdit.copy(); clipEdit.deselect(); clipEdit.text = ""; root.toast("Copied", false) }
    function copyFrom(method, okText) {
        callVia(coreName, method, [root.job.id], function (raw) {
            var o = parse(raw)
            if (!o || o.ok === false) { toast(o && o.error ? o.error : "Could not build the prompt.", true); return }
            clipEdit.text = o.text; clipEdit.selectAll(); clipEdit.copy(); clipEdit.deselect(); clipEdit.text = ""
            toast(okText, false)
        })
    }
    function copyPrompt() { copyFrom("getPrompt", "Copied the prompt. Paste it into an AI chat, then paste the answer here.") }
    function copyYoutubePrompt() { copyFrom("youtubePrompt", "Copied the prompt. Paste it into an AI chat, then paste the answer here.") }
    property string cardFor: ""
    function addCard(u) {
        var which = root.cardFor
        callVia(coreName, "addCard", [String(u)], function (raw) {
            var o = parse(raw)
            if (!o || o.ok === false) { toast(o && o.error ? o.error : "Could not add the image.", true); return }
            apply(o)
            if (which && root.job) act("setJob", [root.job.id, JSON.stringify(which === "intro" ? { intro: o.added } : { outro: o.added })])
        })
    }
    function openCardDialog(key) { root.cardFor = key; cardDialog.open() }

    // ── formatting
    function fmtDur(s) {
        s = Math.round(Number(s) || 0)
        if (s <= 0) return ""
        var h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60), x = s % 60
        return (h ? h + ":" + (m < 10 ? "0" : "") : "") + m + ":" + (x < 10 ? "0" : "") + x
    }
    function stepIndex(step) { return ({ fetch: 0, transcribe: 0, diarize: 0, speakers: 1, pick: 2, clips: 2, cut: 4, thumbs: 4, done: 5 })[step] || 0 }
    // One line saying where a job is, for the list.
    function statusText(j) {
        if (!j) return ""
        if (j.status === "failed") return "Failed"
        if (j.status === "cancelled") return "Stopped"
        if (j.status === "queued") return "Waiting for its turn"
        if (j.status === "running") {
            if (j.step === "cut" && j.progress && j.progress.n) return "Cutting reel " + Math.min(j.progress.i + 1, j.progress.n) + " of " + j.progress.n
            if (j.step === "pick") return "Picking the clips"
            return j.activity || "Working"
        }
        if (j.scenes === "running") return "Finding scenes" + (j.scenesN ? " " + j.scenesI + "/" + j.scenesN : "")
        if (j.status === "done") return j.reels + (j.reels === 1 ? " reel" : " reels")
        if (j.status === "waiting" && j.step === "speakers") return "Name the speakers"
        if (j.status === "waiting") return j.clips ? "Check the clips, then cut" : "Needs clips"
        return ""
    }
    function busyJob(j) { return !!j && (j.status === "running" || j.status === "queued" || j.scenes === "running") }
    function statusColor(j) {
        if (!j) return root.cFaint
        if (j.status === "failed") return root.cBad
        if (busyJob(j)) return root.cAccent
        if (j.status === "done") return root.cOk
        if (j.status === "waiting") return root.cWord
        return root.cFaint
    }

    // ── the pipeline: what each step is doing. state: idle | running | waiting | done | failed | off
    readonly property var nodes: {
        var j = root.job
        var out = [
            { t: "Video", state: "idle", sub: "" },
            { t: "Speakers", state: "idle", sub: "" },
            { t: "Clips", state: "idle", sub: "" },
            { t: "Framing", state: "idle", sub: "" },
            { t: "Reels", state: "idle", sub: "" }
        ]
        if (!j) return out
        var at = root.stepIndex(j.step), running = j.status === "running" || j.status === "queued", failed = j.status === "failed"
        // Video: download, transcribe (and telling the speakers apart beside it)
        if (at > 0) out[0] = { t: "Video", state: "done", sub: [root.fmtDur(j.duration), j.words ? j.words + " words" : ""].filter(function (x) { return x }).join(" · ") }
        else out[0] = { t: "Video", state: failed ? "failed" : running ? "running" : "idle",
                        sub: failed ? "failed" : j.status === "queued" ? "waiting its turn" : (j.activity || "starting") }
        // Speakers
        var found = root.speakers && root.speakers.found ? root.speakers.found : 0
        if (j.step === "speakers") out[1] = { t: "Speakers", state: "waiting", sub: "name them" }
        else if (at > 1) out[1] = { t: "Speakers", state: found >= 2 ? "done" : "off", sub: found >= 2 ? found + " voices" : "one voice" }
        else out[1] = { t: "Speakers", state: "idle", sub: "" }
        // Clips
        if (j.step === "pick" && running) out[2] = { t: "Clips", state: "running", sub: "asking " + root.pickerLabel(j.agent) }
        else if (j.step === "pick" && failed) out[2] = { t: "Clips", state: "failed", sub: "failed" }
        else if (root.clips.length) out[2] = { t: "Clips", state: j.step === "clips" ? "waiting" : "done", sub: root.chosen.length + " of " + root.clips.length + " ticked" }
        else if (at >= 2) out[2] = { t: "Clips", state: "waiting", sub: "none yet" }
        // Framing: optional, never holds the rest up
        var sc = root.scenes || {}
        if (sc.state === "running") out[3] = { t: "Framing", state: "running", sub: "finding scenes" + (sc.n ? " " + (sc.i || 0) + "/" + sc.n : "") }
        else if (sc.state === "failed") out[3] = { t: "Framing", state: "failed", sub: "scenes failed" }
        else if (root.framedCount) out[3] = { t: "Framing", state: "done", sub: root.framedCount + " by hand" }
        else if (sc.state === "done") out[3] = { t: "Framing", state: "idle", sub: "scenes ready" }
        else if (root.clips.length) out[3] = { t: "Framing", state: "idle", sub: "automatic" }
        // Reels
        if ((j.step === "cut" || j.step === "thumbs") && running)
            out[4] = { t: "Reels", state: "running", sub: j.progress && j.progress.n ? "cutting " + Math.min(j.progress.i + 1, j.progress.n) + " of " + j.progress.n : (j.activity || "cutting") }
        else if ((j.step === "cut" || j.step === "thumbs") && failed) out[4] = { t: "Reels", state: "failed", sub: "failed" }
        else if (root.reels.length) out[4] = { t: "Reels", state: "done", sub: root.reels.length + (root.reels.length === 1 ? " reel" : " reels") + (root.recut.length ? " · cut again" : "") }
        return out
    }
    // Clips framed by hand since their reel was cut: the Reels screen offers to cut them again.
    property var recut: []
    function markRecut(n) {
        if (!root.reels.some(function (r) { return r.n === n })) return
        if (root.recut.indexOf(n) < 0) root.recut = root.recut.concat([n])
    }

    // ── one blink a second while something runs: the only thing that moves
    property bool pulse: true
    Timer { interval: 1000; running: root.anyRunning; repeat: true; onTriggered: root.pulse = !root.pulse; onRunningChanged: if (!running) root.pulse = true }

    // ── toast
    property string toastText: ""
    property bool toastBad: false
    function toast(t, bad) { root.toastText = t; root.toastBad = !!bad; toastTimer.restart() }
    Timer { id: toastTimer; interval: 4200; onTriggered: root.toastText = "" }

    Timer { interval: root.anyRunning ? 1500 : 5000; running: true; repeat: true; onTriggered: root.refresh() }
    Component.onCompleted: { if (!root.sandboxImages) registerViewDir(); refresh(); callVia(coreName, "listAgents", [], function (raw) { var o = parse(raw); if (o && o.jobs) apply(o) }) }

    component Label2: Text { color: root.cSub; font.pixelSize: 12; textFormat: Text.PlainText }
    component Section: Text { color: root.cFaint; font.pixelSize: 11; font.letterSpacing: 1.2; textFormat: Text.PlainText }

    Rectangle { anchors.fill: parent; color: root.cBg }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ── left: new job + the list of jobs
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 270
            color: root.cSurface
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: root.sp * 2
                spacing: root.sp
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "Reelcut"; color: root.cText; font.pixelSize: 20; font.bold: true; textFormat: Text.PlainText; Layout.fillWidth: true }
                    Text { text: root.st.version ? "v" + root.st.version : ""; color: root.cFaint; font.pixelSize: 11; textFormat: Text.PlainText }
                    Rectangle {
                        implicitWidth: gearRow.implicitWidth + 20
                        implicitHeight: 28
                        radius: 14
                        color: gearMouse.containsMouse ? Qt.lighter(root.cRaised, 1.25) : root.cRaised
                        border.width: 1
                        border.color: gearMouse.containsMouse ? root.cAccent : root.cLine
                        Row {
                            id: gearRow
                            anchors.centerIn: parent
                            spacing: 6
                            Text { text: "⚙"; color: root.cAccent; font.pixelSize: 14; anchors.verticalCenter: parent.verticalCenter; textFormat: Text.PlainText }
                            Text { text: "Settings"; color: root.cText; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter; textFormat: Text.PlainText }
                        }
                        MouseArea { id: gearMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: settings.openIt() }
                    }
                }
                Field {
                    id: urlField
                    app: root
                    Layout.fillWidth: true
                    Layout.topMargin: root.sp
                    Layout.preferredHeight: 38
                    placeholderText: "Paste a video address"
                    onAccepted: root.start()
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 56
                    radius: 8; color: root.cBg
                    border.width: 1; border.color: briefNew.activeFocus ? root.cAccent : root.cLine
                    ScrollView {
                        anchors.fill: parent; anchors.margins: 4
                        TextArea {
                            id: briefNew
                            placeholderText: "What I'm looking for (optional)"
                            placeholderTextColor: root.cFaint
                            color: root.cText; font.pixelSize: 12; wrapMode: TextEdit.Wrap; selectByMouse: true
                            background: null
                            readonly property string seed: root.st.settings ? root.st.settings.brief || "" : ""
                            onSeedChanged: if (!activeFocus) text = seed
                        }
                    }
                }
                LogosButton { text: "Make reels"; Layout.fillWidth: true; enabled: urlField.text.trim() !== "" && !!root.st.runner && !root.st.runner.error; onClicked: root.start() }
                Link { app: root; text: "or use a video file…"; Layout.alignment: Qt.AlignHCenter; onClicked: fileDialog.open() }

                Rectangle {
                    visible: !!root.st.runner && !!root.st.runner.error
                    Layout.fillWidth: true
                    implicitHeight: runErr.implicitHeight + 20
                    radius: 8
                    color: Qt.darker(root.cBad, 2.4)
                    border.width: 1; border.color: root.cBad
                    Text {
                        id: runErr
                        anchors.fill: parent; anchors.margins: 10
                        text: (root.st.runner && root.st.runner.error ? root.st.runner.error : "") + " Install podman, or reelcut from github.com/vpavlin/reelcut."
                        color: root.cText; font.pixelSize: 12; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                    }
                }

                Section { text: "JOBS"; Layout.topMargin: root.sp * 2; visible: root.jobs.length > 0 }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 4
                    model: root.jobs
                    boundsBehavior: Flickable.StopAtBounds
                    delegate: Rectangle {
                        id: jRow
                        required property var modelData
                        width: ListView.view.width
                        height: 54
                        radius: 8
                        readonly property bool active: !!root.job && modelData.id === root.job.id
                        readonly property bool working: root.busyJob(modelData)
                        color: active ? root.cRaised : (jMouse.containsMouse ? Qt.darker(root.cRaised, 1.15) : "transparent")
                        border.width: active ? 1 : 0
                        border.color: root.cAccent
                        MouseArea { id: jMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.selectedId = jRow.modelData.id }
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 9
                            spacing: 4
                            Text { text: jRow.modelData.title || jRow.modelData.url; color: root.cText; font.pixelSize: 13; font.bold: jRow.active; elide: Text.ElideRight; Layout.fillWidth: true; textFormat: Text.PlainText }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                Rectangle { width: 7; height: 7; radius: 4; color: root.statusColor(jRow.modelData); opacity: jRow.working && !root.pulse ? 0.25 : 1 }
                                Text { text: root.statusText(jRow.modelData); color: root.cSub; font.pixelSize: 11; elide: Text.ElideRight; Layout.fillWidth: true; textFormat: Text.PlainText }
                                Text { text: root.fmtDur(jRow.modelData.duration); color: root.cFaint; font.pixelSize: 11; font.features: { "tnum": 1 }; textFormat: Text.PlainText }
                            }
                        }
                    }
                }
                Text {
                    visible: !!root.st.root
                    text: "Saved in " + (root.st.root || "")
                    color: root.cFaint; font.pixelSize: 10; elide: Text.ElideMiddle; Layout.fillWidth: true; textFormat: Text.PlainText
                }
            }
        }
        Rectangle { Layout.fillHeight: true; width: 1; color: root.cLine }

        // ── right: the open job
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                visible: !root.job
                anchors.centerIn: parent
                width: Math.min(parent.width - 80, 460)
                spacing: 12
                Text { text: "Paste a long video, get reels."; color: root.cText; font.pixelSize: 22; font.bold: true; textFormat: Text.PlainText; Layout.alignment: Qt.AlignHCenter }
                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: root.cSub; font.pixelSize: 14; textFormat: Text.PlainText
                    text: "A livestream, a Space, a talk or a podcast. Reelcut downloads it, transcribes it on this computer, helps you pick the best moments and cuts them into reels with word-by-word captions."
                }
            }

            ColumnLayout {
                visible: !!root.job
                anchors.fill: parent
                spacing: 0

                // header
                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 24; Layout.rightMargin: 24; Layout.topMargin: 16
                    spacing: 12
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text { text: root.job ? (root.job.title || root.job.url) : ""; color: root.cText; font.pixelSize: 19; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true; textFormat: Text.PlainText }
                        Text {
                            text: root.job ? [root.job.channel, root.fmtDur(root.job.duration), root.job.url || root.job.file].filter(function (x) { return !!x }).join("  ·  ") : ""
                            color: root.cFaint; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true; textFormat: Text.PlainText
                        }
                    }
                    LogosButton { text: "Open folder"; onClicked: root.act("openPath", [root.job.dir]) }
                    LogosButton { visible: root.jobRunning; text: "Stop"; onClicked: root.act("cancelJob", [root.job.id]) }
                    LogosButton {
                        visible: !!root.job && (root.job.status === "failed" || root.job.status === "cancelled")
                        text: "Retry"; onClicked: root.act("retryJob", [root.job.id])
                    }
                    Text {
                        text: "⋯"; color: root.cSub; font.pixelSize: 20; textFormat: Text.PlainText
                        MouseArea { anchors.fill: parent; anchors.margins: -8; cursorShape: Qt.PointingHandCursor; onClicked: jobMenu.popup() }
                    }
                }

                // the pipeline: one box per step, click to open its screen
                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 24; Layout.rightMargin: 24; Layout.topMargin: 14
                    spacing: 0
                    Repeater {
                        model: root.nodes.length
                        delegate: RowLayout {
                            id: nodeRow
                            required property int index
                            readonly property var node: root.nodes[index]
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            spacing: 0
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 52
                                radius: 8
                                readonly property bool on: root.tab === nodeRow.index
                                readonly property color tint: nodeRow.node.state === "failed" ? root.cBad
                                                            : nodeRow.node.state === "running" ? root.cAccent
                                                            : nodeRow.node.state === "waiting" ? root.cWord
                                                            : nodeRow.node.state === "done" ? root.cOk : root.cFaint
                                color: on ? root.cRaised : nMouse.containsMouse ? Qt.darker(root.cRaised, 1.1) : root.cSurface
                                border.width: on ? 2 : 1
                                border.color: on ? root.cAccent : root.cLine
                                RowLayout {
                                    anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 10
                                    spacing: 10
                                    Rectangle {
                                        width: 10; height: 10; radius: 5
                                        color: nodeRow.node.state === "idle" || nodeRow.node.state === "off" ? "transparent" : parent.parent.tint
                                        border.width: 2; border.color: parent.parent.tint
                                        opacity: nodeRow.node.state === "running" && !root.pulse ? 0.2 : 1
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 1
                                        Text { text: (nodeRow.index + 1) + "  " + nodeRow.node.t; color: root.cText; font.pixelSize: 13; font.bold: parent.parent.parent.on; textFormat: Text.PlainText }
                                        Text { text: nodeRow.node.sub; visible: text !== ""; color: parent.parent.parent.tint === root.cFaint ? root.cFaint : root.cSub; font.pixelSize: 11; elide: Text.ElideRight; Layout.fillWidth: true; textFormat: Text.PlainText }
                                    }
                                }
                                MouseArea { id: nMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.show(nodeRow.index) }
                                Accessible.role: Accessible.PageTab
                                Accessible.name: nodeRow.node.t
                            }
                            Text { visible: nodeRow.index < root.nodes.length - 1; text: "›"; color: root.cFaint; font.pixelSize: 18; Layout.leftMargin: 6; Layout.rightMargin: 6; textFormat: Text.PlainText }
                        }
                    }
                }

                Rectangle { Layout.fillWidth: true; Layout.topMargin: 14; height: 1; color: root.cLine }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: root.tab
                    VideoScreen { app: root }
                    SpeakersScreen { app: root }
                    ClipsScreen { app: root }
                    FramingScreen { id: framingScreen; app: root; active: root.tab === 3 }
                    ReelsScreen { app: root }
                }
            }
        }
    }

    // ── toast
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        visible: root.toastText !== ""
        width: Math.min(toastLabel.implicitWidth + 32, parent.width - 48)
        height: toastLabel.implicitHeight + 20
        radius: 10
        color: root.toastBad ? Qt.darker(root.cBad, 1.6) : root.cRaised
        border.width: 1
        border.color: root.toastBad ? root.cBad : root.cLine
        Text { id: toastLabel; anchors.centerIn: parent; width: parent.width - 32; text: root.toastText; color: root.cText; font.pixelSize: 13; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter; textFormat: Text.PlainText }
    }

    Rectangle {
        visible: root.coreLost || (!root.coreSeen && root.missedPolls >= 3)
        anchors.top: parent.top; anchors.horizontalCenter: parent.horizontalCenter; anchors.topMargin: 12
        width: lostT.implicitWidth + 28; height: 32; radius: 8
        color: Qt.darker(root.cBad, 1.8); border.width: 1; border.color: root.cBad
        Text { id: lostT; anchors.centerIn: parent; text: "reelcut_core does not answer. Is it installed?"; color: root.cText; font.pixelSize: 12; textFormat: Text.PlainText }
    }

    // ── menus and dialogs
    Menu {
        id: jobMenu
        MenuItem { text: "Copy the address"; enabled: !!root.job && !!root.job.url; onTriggered: root.copyText(root.job.url) }
        MenuItem { text: "Open the transcript"; enabled: !!root.job; onTriggered: root.act("openPath", [root.job.dir + "/transcript-ts.txt"]) }
        MenuItem { text: "Remove from the list"; onTriggered: root.act("removeJob", [root.job.id, false]) }
        MenuItem { text: "Delete it and its files"; onTriggered: confirmDelete.open() }
    }
    TextEdit { id: clipEdit; visible: false }

    FileDialog {
        id: cardDialog
        title: "An image for the card (a logo, a QR code, a URL)"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Images (*.png *.jpg *.jpeg *.webp)"]
        onAccepted: root.addCard(cardDialog.selectedFile)
    }
    FileDialog {
        id: fileDialog
        title: "A video to make reels from"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Videos (*.mp4 *.mkv *.webm *.mov *.m4v *.avi)", "All files (*)"]
        onAccepted: root.importUrl(fileDialog.selectedFile)
    }
    Popup {
        id: confirmDelete
        anchors.centerIn: parent
        width: 400
        modal: true
        padding: 20
        background: Rectangle { radius: 12; color: root.cSurface; border.width: 1; border.color: root.cLine }
        contentItem: ColumnLayout {
            spacing: 10
            Text { text: "Delete this job?"; color: root.cText; font.pixelSize: 17; font.bold: true; textFormat: Text.PlainText }
            Text {
                text: "Its folder goes too: the downloaded video, the transcript and the reels."
                color: root.cSub; font.pixelSize: 13; wrapMode: Text.WordWrap; Layout.fillWidth: true; textFormat: Text.PlainText
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 8
                Item { Layout.fillWidth: true }
                LogosButton { text: "Cancel"; onClicked: confirmDelete.close() }
                LogosButton { text: "Delete"; onClicked: { confirmDelete.close(); root.act("removeJob", [root.job.id, true], "Deleted") } }
            }
        }
    }

    Popup {
        id: settings
        function openIt() {
            var s = root.st.settings || {}
            countBox.value = s.candidates || 12
            minBox.value = s.min || 20
            maxBox.value = s.max || 55
            diarizeBox.checked = s.diarize !== false
            autoCutBox.checked = s.autoCut === true
            autoCountBox.value = s.autoCount || 5
            setEdges(s.lead === undefined ? 0.25 : s.lead, s.tail === undefined ? 0.4 : s.tail, s.fade === undefined ? 0.3 : s.fade)
            cardField.text = String(s.card || 2)
            var m = s.captionMargin === undefined ? -1 : s.captionMargin
            var at = capPlace.margins.indexOf(m)
            capPlace.currentIndex = at >= 0 ? at : capPlace.margins.length
            capExact.value = m >= 0 ? m : 470
            storeBox.currentIndex = Math.max(0, ["", "local", "node"].indexOf(s.storeTo || ""))
            storeNodeField.text = s.storeNode || ""
            forumAliasField.text = s.forumAlias === undefined ? "Reelcut" : s.forumAlias
            colourRow.field.text = s.colour || ""
            sayingRow.field.text = s.sayingColour || ""
            saidRow.field.text = s.saidColour || ""
            var vals = setAgent.opts.map(function (o) { return o.value })
            var want = s.agent === undefined ? "auto" : s.agent
            setAgent.currentIndex = Math.max(0, vals.indexOf(want))
            runnerBox.currentIndex = Math.max(0, ["auto", "podman", "docker", "local"].indexOf(s.runner || "auto"))
            open()
            root.callVia(root.coreName, "listAgents", [], function (raw) { var o = root.parse(raw); if (o && o.jobs) root.apply(o) })
        }
        function setEdges(l, t, f) { leadField.text = String(l); tailField.text = String(t); fadeField.text = String(f) }
        // "#ffaa00", "ffaa00" -> "#FFAA00"; anything else -> "" (the default).
        function hex(t) { var c = String(t).trim().replace(/^#/, ""); return /^[0-9A-Fa-f]{6}$/.test(c) ? "#" + c.toUpperCase() : "" }
        function num(field, def) { var v = parseFloat(String(field.text).replace(",", ".")); return isNaN(v) ? def : v }
        function save() {
            var agent = setAgent.currentIndex >= 0 ? setAgent.opts[setAgent.currentIndex].value : ""
            root.act("setSettings", [JSON.stringify({
                agent: agent, candidates: countBox.value, min: minBox.value, max: Math.max(maxBox.value, minBox.value + 5),
                autoCut: autoCutBox.checked, autoCount: autoCountBox.value, diarize: diarizeBox.checked,
                lead: num(leadField, 0.25), tail: num(tailField, 0.4), fade: num(fadeField, 0.3), card: num(cardField, 2),
                storeTo: ["", "local", "node"][storeBox.currentIndex], storeNode: storeNodeField.text.trim(),
                forumAlias: forumAliasField.text.trim(),
                captionMargin: capPlace.margin, colour: hex(colourRow.field.text), sayingColour: hex(sayingRow.field.text), saidColour: hex(saidRow.field.text),
                runner: ["auto", "podman", "docker", "local"][runnerBox.currentIndex] })], "Saved")
            close()
        }
        anchors.centerIn: parent
        width: Math.min(640, root.width - 40)
        height: Math.min(setCol.implicitHeight + 40, root.height - 40)
        modal: true
        padding: 20
        background: Rectangle { radius: 12; color: root.cSurface; border.width: 1; border.color: root.cLine }
        component Small: Field { app: root; Layout.preferredWidth: 64; Layout.preferredHeight: 32; validator: DoubleValidator { bottom: 0; top: 10 } }
        component Check: CheckBox {
            id: cb
            contentItem: Text { text: cb.text; color: root.cText; font.pixelSize: 13; leftPadding: cb.indicator.width + 6; verticalAlignment: Text.AlignVCenter; wrapMode: Text.WordWrap; textFormat: Text.PlainText }
        }
        component ColourRow: RowLayout {
            id: cr
            property alias field: crField
            property string label
            property string hint
            property color fallback
            readonly property color shown: settings.hex(crField.text) || fallback
            Layout.fillWidth: true
            spacing: 8
            Label2 { text: cr.label; Layout.fillWidth: true }
            Rectangle { width: 22; height: 22; radius: 4; color: cr.shown; border.width: 1; border.color: root.cLine }
            Field { app: root; id: crField; Layout.preferredWidth: 150; Layout.preferredHeight: 32; placeholderText: cr.hint; maximumLength: 7 }
        }
        contentItem: Flickable {
            clip: true
            contentHeight: setCol.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            ColumnLayout {
                id: setCol
                width: parent.width
                spacing: 8
                Text { text: "Settings"; color: root.cText; font.pixelSize: 17; font.bold: true; textFormat: Text.PlainText }

                Section { text: "PICKING"; Layout.topMargin: 8 }
                Label2 { text: "Who picks the clips" }
                ComboBox {
                    id: setAgent
                    readonly property var opts: root.pickOptsAll
                    Layout.fillWidth: true
                    model: opts.map(function (o) { return o.label })
                }
                Text {
                    Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                    color: root.cFaint; font.pixelSize: 12
                    text: {
                        var la = root.st.localAgent || {}
                        if (la.ok) return "An agent on this computer runs in a container of its own (a cage), with its own folder; it reads the transcript and answers with the clips. Claude Code uses your Claude login; pi uses the model and key in pi's own settings."
                        return "No agent on this computer (" + (la.why || "no shrooms-agent") + "), so you copy the prompt into any AI chat. With shrooms-agent (no mesh needed) and podman, Claude Code or pi picks them for you."
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label2 { text: "Clips to propose, best first"; Layout.fillWidth: true }
                    SpinBox { id: countBox; from: 1; to: 30; editable: true }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label2 { text: "Each clip, in seconds"; Layout.fillWidth: true }
                    SpinBox { id: minBox; from: 5; to: 170; editable: true }
                    Label2 { text: "to" }
                    SpinBox { id: maxBox; from: 10; to: 180; editable: true }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Check { id: autoCutBox; text: "Cut the best ones as soon as the agent answers:"; Layout.fillWidth: true }
                    SpinBox { id: autoCountBox; from: 1; to: 30; editable: true }
                }
                Text {
                    Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                    color: root.cFaint; font.pixelSize: 12
                    text: "That many of the best are ticked when the clips come in; you can tick others and cut again."
                }
                Check { id: diarizeBox; text: "Tell the speakers apart (about 7 min per hour of video)"; Layout.fillWidth: true }
                Text {
                    Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                    color: root.cFaint; font.pixelSize: 12
                    text: "For interviews and calls: you name the people once, the reels show who speaks, and two people on a call are cut one above the other."
                }

                Section { text: "THE REELS"; Layout.topMargin: 10 }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label2 { text: "Before the first word" }
                    Small { id: leadField }
                    Label2 { text: "s   after the last" }
                    Small { id: tailField }
                    Label2 { text: "s   fade" }
                    Small { id: fadeField }
                    Label2 { text: "s" }
                    Item { Layout.fillWidth: true }
                }
                RowLayout {
                    spacing: 14
                    Text {
                        text: "Tight (the default)"
                        color: tightMouse.containsMouse ? root.cAccent : root.cSub; font.pixelSize: 12; textFormat: Text.PlainText
                        MouseArea { id: tightMouse; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: settings.setEdges(0.25, 0.4, 0.3) }
                    }
                    Text {
                        text: "Breathing room"
                        color: roomMouse.containsMouse ? root.cAccent : root.cSub; font.pixelSize: 12; textFormat: Text.PlainText
                        MouseArea { id: roomMouse; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: settings.setEdges(1.5, 1.5, 0.8) }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label2 { text: "Opening and closing cards show for" }
                    Small { id: cardField }
                    Label2 { text: "s each" }
                    Item { Layout.fillWidth: true }
                }

                Section { text: "CAPTIONS"; Layout.topMargin: 10 }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label2 { text: "Captions sit"; Layout.fillWidth: true }
                    ComboBox {
                        id: capPlace
                        // px from the bottom of the 1920-high reel; the last entry is "exactly".
                        readonly property var margins: [-1, 250, 470, 860, 1250, 1550]
                        readonly property int margin: currentIndex < margins.length ? margins[currentIndex] : capExact.value
                        Layout.preferredWidth: 260
                        model: ["Where the layout puts them", "Near the bottom", "Lower third", "In the middle", "Upper third", "Near the top", "Exactly…"]
                    }
                }
                RowLayout {
                    visible: capPlace.currentIndex === capPlace.margins.length
                    Layout.fillWidth: true
                    spacing: 8
                    Item { Layout.fillWidth: true }
                    SpinBox { id: capExact; from: 0; to: 1800; stepSize: 10; editable: true }
                    Label2 { text: "px up from the bottom (the reel is 1920 high)" }
                }
                ColourRow { id: colourRow; label: "Words still to come"; hint: "speaker's colour"; fallback: "#FFFFFF" }
                ColourRow { id: sayingRow; label: "The word being said"; hint: "#FFE500"; fallback: "#FFE500" }
                ColourRow { id: saidRow; label: "Words already said"; hint: "as those to come"; fallback: colourRow.shown }
                // What it looks like, on a dark picture.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 52
                    radius: 8
                    color: "#262a30"
                    Row {
                        anchors.centerIn: parent
                        spacing: 12
                        Repeater {
                            model: [{ t: "ALREADY", c: saidRow.shown }, { t: "SAYING", c: sayingRow.shown }, { t: "TO COME", c: colourRow.shown }]
                            delegate: Text {
                                required property var modelData
                                text: modelData.t; color: modelData.c
                                font.pixelSize: 20; font.bold: true; style: Text.Outline; styleColor: "#000"; textFormat: Text.PlainText
                            }
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                    color: root.cFaint; font.pixelSize: 12
                    text: "Colours as #RRGGBB; leave one empty for the default. With the speakers told apart, the words to come take each speaker's colour unless you set one here. Changes apply to the next cut; tick clips and cut again to redo them."
                }

                Section { text: "LOGOS STORAGE"; Layout.topMargin: 10 }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label2 { text: "Put the reels in Logos Storage"; Layout.fillWidth: true }
                    ComboBox { id: storeBox; Layout.preferredWidth: 260; model: ["No", "This computer's node", "A node elsewhere"] }
                }
                Field { app: root;
                    id: storeNodeField
                    visible: storeBox.currentIndex === 2
                    Layout.fillWidth: true
                    Layout.preferredHeight: 34
                    placeholderText: "http://[fd00::1]:8480 (the node's REST API, over your VPN or mesh)"
                }
                Text {
                    Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                    color: root.cFaint; font.pixelSize: 12
                    text: storeBox.currentIndex === 0 ? "Off: the reels stay in the job's folder."
                        : "Each cut reel goes to the node, and with them a small reels.json (each reel's hook, length and CID): its CID shares the whole set. Anyone with a CID can fetch it from the node while it is reachable." + (storeBox.currentIndex === 2 ? " A node on an always-on server serves them all the time; its REST API has no password, so give an address on your VPN or mesh only." : "")
                }

                Section { text: "FORUMS"; Layout.topMargin: 10 }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label2 { text: "Name on Reelcut's posts"; Layout.fillWidth: true }
                    Field { app: root; id: forumAliasField; Layout.preferredWidth: 260; Layout.preferredHeight: 34; maximumLength: 32; placeholderText: "Reelcut" }
                }
                Text {
                    Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                    color: root.cFaint; font.pixelSize: 12
                    text: "Reelcut posts to Forumer and Logos Forum with a key of its own" + (root.st.forumKey ? " (" + root.st.forumKey + "…)" : "")
                        + ": the forums show this name next to the key's fingerprint. Empty: the fingerprint only."
                }

                Section { text: "RUNNING"; Layout.topMargin: 10 }
                Label2 { text: "Run reelcut with" }
                ComboBox { id: runnerBox; Layout.fillWidth: true; model: ["Whatever is installed", "podman", "docker", "reelcut on PATH"] }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    font.pixelSize: 12
                    color: root.st.runner && root.st.runner.error ? root.cBad : root.cFaint
                    text: {
                        var r = root.st.runner || {}
                        if (r.error) return r.error
                        if (r.kind === "local") return "Using " + r.bin + "."
                        if (r.kind) return "Using " + r.kind + " with " + r.image + (r.imagePresent ? "." : " (downloaded on the first job, about 1.4 GB).")
                        return ""
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 10
                    Item { Layout.fillWidth: true }
                    LogosButton { text: "Cancel"; onClicked: settings.close() }
                    LogosButton { text: "Save"; onClicked: settings.save() }
                }
            }
        }
    }
}
