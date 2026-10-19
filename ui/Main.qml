// Reelcut - a Basecamp view: paste a video address, get short vertical reels with captions.
//
// Pure QML: the Basecamp 0.3 sandbox gives a view no network and no files outside its own
// install dir, so everything runs in the reelcut_core module (reelcut in a container, or a local
// reelcut). This view renders reelcut_core.snapshot(), polled (fast while a step runs), and loads
// the reel previews the core copies under <this dir>/cache.
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
    width: 1180
    height: 760

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
    // The caption yellow of the reels themselves.
    readonly property color cWord: "#ffe14d"
    // Each speaker's caption colour in the reels (reelcut's SPEAKER_COLOURS): A, B, C, …
    readonly property var speakerColours: ["#ffffff", "#8fd3ff", "#ffb86c", "#b9f18c", "#ffa0e0"]
    readonly property var sideValues: ["", "left", "middle", "right"]
    // Logos Storage for the open job: on in Settings, how many reels are not there yet, what failed.
    readonly property bool storeOn: !!root.st.settings && !!root.st.settings.storeTo
    readonly property int storeMissing: root.job && root.job.reels ? root.job.reels.filter(function (r) { return !r.cid }).length : 0
    readonly property bool storeFailed: !!root.job && !!root.job.reels && (root.job.reels.some(function (r) { return r.storing && r.storing !== "uploading" }) || (!!root.job.storage && /^failed/.test(root.job.storage.setStoring || "")))
    readonly property string storeStatus: {
        if (!root.storeOn || !root.job || !root.job.reels || !root.job.reels.length) return ""
        var up = root.job.reels.filter(function (r) { return r.storing === "uploading" }).length
        if (up) return "Putting " + up + (up === 1 ? " reel" : " reels") + " in Logos Storage…"
        if (root.storeFailed) return (root.job.storage && /^failed: /.test(root.job.storage.setStoring || "") ? root.job.storage.setStoring.replace(/^failed: /, "") : "Some reels didn't make it to Logos Storage.")
        if (root.job.storage && root.job.storage.setStoring === "uploading") return "Putting the set in Logos Storage…"
        if (root.job.storage && root.job.storage.set) return "In Logos Storage"
        return ""
    }
    function speakerColour(letter) { return root.speakerColours[(String(letter).charCodeAt(0) - 65) % root.speakerColours.length] }
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

    // ── state
    property var st: ({ jobs: [], agents: [], settings: {}, runner: {} })
    property bool coreSeen: false
    property int missedPolls: 0
    property bool busy: false
    property bool again: false
    property string selectedId: ""
    readonly property bool coreLost: root.coreSeen && root.missedPolls >= 4
    readonly property var jobs: st.jobs || []
    readonly property var job: {
        for (var i = 0; i < root.jobs.length; i++) if (root.jobs[i].id === root.selectedId) return root.jobs[i]
        return root.jobs.length ? root.jobs[0] : null
    }
    readonly property bool anyRunning: {
        for (var i = 0; i < root.jobs.length; i++) if (root.jobs[i].status === "running" || root.jobs[i].status === "queued") return true
        return false
    }

    function apply(obj) {
        if (!obj) return
        if (obj.ok === false) { toast(obj.error || "That did not work.", true); return }
        if (obj.jobs === undefined) return
        root.st = obj
        root.coreSeen = true
        root.missedPolls = 0
        if (!obj.thumbsOk) registerViewDir()
    }
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
            // Reelcut's own sessions are reached as "on this computer", not by name.
            if (!/\/reelcut-(claude|pi)$/.test(a.name)) o.push({ value: a.name, label: a.name + " (on the mesh)" })
        })
        if (withNobody) o.push({ value: "", label: "Nobody: I paste from an AI chat" })
        return o
    }
    // Kept as is until the choices change, so a poll does not reset a ComboBox's selection.
    property var pickOpts: []
    onStChanged: {
        var n = pickerOptions(false)
        if (JSON.stringify(n) !== JSON.stringify(root.pickOpts)) root.pickOpts = n
    }
    // The speakers of the selected job ([letter]), kept as is between polls so their fields keep what is typed.
    property var spkLetters: []
    onJobChanged: {
        updateClipRows()
        var cs = root.st.cards || []
        if (JSON.stringify(cs) !== JSON.stringify(root.cards)) root.cards = cs
        var s = root.job && root.job.speakers ? root.job.speakers : {}
        var n = (s.found || 0) >= 2 ? Object.keys(s.talk || {}).sort() : []
        n = n.length ? [root.job.id].concat(n) : []
        if (JSON.stringify(n) !== JSON.stringify(root.spkLetters)) root.spkLetters = n
    }
    // Names, sides and the layout from the speakers panel; proceed: go on to picking the clips.
    function saveSpeakers(proceed) {
        var names = {}, sides = {}
        for (var i = 0; i < spkRep.count; i++) {
            var it = spkRep.itemAt(i)
            if (!it) continue
            names[it.letter] = it.nameText
            if (it.side) sides[it.letter] = it.side
        }
        act("setSpeakers", [root.job.id, JSON.stringify({ names: names, sides: sides, layout: layoutValue(spkLayout.currentIndex) }), !!proceed],
            proceed ? "" : "Saved")
    }
    // The layouts cut --frame takes: "" lets reelcut choose (fit for landscape, crop for portrait).
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
    readonly property var pickOptsAll: root.pickOpts.concat([{ value: "", label: "Nobody: I paste from an AI chat" }])
    function pickerLabel(v) {
        if (v === "auto") return "an agent on this computer"
        if (v === "local:claude") return "Claude Code on this computer"
        if (v === "local:pi") return "pi on this computer"
        return v
    }

    // What the person is looking for, for one job (and the default for the next).
    function setBrief(id, text, after) {
        callVia(coreName, "setJob", [id, JSON.stringify({ brief: text })], function (raw) { var o = parse(raw); if (o && o.jobs) apply(o); if (after) after() })
    }
    // The clips to cut: ticked or not.
    function tick(n, on) {
        var c = (root.job.chosen || []).filter(function (x) { return x !== n })
        if (on) c.push(n)
        c.sort(function (a, b) { return a - b })
        act("setJob", [root.job.id, JSON.stringify({ chosen: c })])
    }
    function tickAll(on) {
        act("setJob", [root.job.id, JSON.stringify({ chosen: on ? root.job.clips.map(function (c) { return c.n }) : [] })])
    }
    // The candidates of the selected job, kept as is between polls (a row keeps its open transcript).
    property var clipRows: []
    property string clipRowsKey: ""
    function updateClipRows() {
        var k = root.job ? root.job.id + JSON.stringify(root.job.clips) : ""
        if (k !== root.clipRowsKey) { root.clipRowsKey = k; root.clipRows = root.job ? root.job.clips : [] }
    }
    // The card library: [{file, name, thumb}], kept as is between polls (a ComboBox keeps its choice).
    property var cards: []
    function cardIndex(file) {
        for (var i = 0; i < root.cards.length; i++) if (root.cards[i].file === file) return i + 1
        return 0
    }
    function cardThumb(file) {
        for (var i = 0; i < root.cards.length; i++) if (root.cards[i].file === file) return root.cards[i].thumb
        return ""
    }
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

    function copyText(t) { clip.text = t; clip.selectAll(); clip.copy(); clip.deselect(); clip.text = ""; root.toast("Copied", false) }
    // The prompt for any AI chat, to the clipboard.
    function copyPrompt() {
        callVia(coreName, "getPrompt", [root.job.id], function (raw) {
            var o = parse(raw)
            if (!o || o.ok === false) { toast(o && o.error ? o.error : "Could not build the prompt.", true); return }
            clip.text = o.text; clip.selectAll(); clip.copy(); clip.deselect(); clip.text = ""
            toast("Copied the prompt. Paste it into an AI chat, then paste the answer here.", false)
        })
    }
    // The YouTube texts' prompt for any AI chat, to the clipboard.
    function copyYoutubePrompt() {
        callVia(coreName, "youtubePrompt", [root.job.id], function (raw) {
            var o = parse(raw)
            if (!o || o.ok === false) { toast(o && o.error ? o.error : "Could not build the prompt.", true); return }
            clip.text = o.text; clip.selectAll(); clip.copy(); clip.deselect(); clip.text = ""
            toast("Copied the prompt. Paste it into an AI chat, then paste the answer here.", false)
        })
    }
    // The open job's forum target, and the threads read for its forum (if any).
    readonly property var forumT: root.job && root.job.forum && root.job.forum.target ? root.job.forum.target : ({})
    readonly property string forumApp: root.forumT.app || ""
    readonly property string forumName: root.forumT.forum || (root.forumApp === "forumer" ? "public" : root.forumApp ? "Logos Forum" : "")
    readonly property var forumList: root.forumApp && root.st.forumThreads ? (root.st.forumThreads[root.forumApp + "|" + root.forumName] || null) : null
    readonly property int forumPosted: root.job && root.job.reels ? root.job.reels.filter(function (r) { return !!r.forumPost }).length : 0
    function setForum(obj) { act("setForum", [root.job.id, JSON.stringify(obj)]) }
    readonly property bool anyYoutube: !!root.job && !!root.job.reels && root.job.reels.some(function (r) { return !!r.youtube })
    readonly property bool ytAsking: !!root.job && !!root.job.youtube && root.job.youtube.state === "asking"
    function importUrl(u) {
        var brief = briefNew.text.trim()
        act("importFile", [String(u)], "", function (o) { if (o.jobs && o.jobs.length) { root.selectedId = o.jobs[0].id; root.setBrief(o.jobs[0].id, brief) } })
    }

    // ── formatting
    function fmtDur(s) {
        s = Math.round(Number(s) || 0)
        if (s <= 0) return ""
        var h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60), x = s % 60
        return (h ? h + ":" + (m < 10 ? "0" : "") : "") + m + ":" + (x < 10 ? "0" : "") + x
    }
    function stepIndex(step) { return ({ fetch: 0, transcribe: 1, diarize: 2, speakers: 2, pick: 3, clips: 3, cut: 4, thumbs: 4, done: 5 })[step] || 0 }
    // One line saying where a job is, for the list and the header.
    function statusText(j) {
        if (!j) return ""
        if (j.status === "failed") return "Failed"
        if (j.status === "cancelled") return "Stopped"
        if (j.status === "done") return j.reels.length + (j.reels.length === 1 ? " reel" : " reels")
        if (j.status === "queued") return "Waiting for its turn"
        if (j.status === "waiting" && j.step === "speakers") return "Name the speakers"
        if (j.status === "waiting") return j.clips.length ? "Check the clips, then cut" : "Needs clips"
        // Before picking, several things run at once; the core says which, with the transcribe count.
        if (j.step === "fetch" || j.step === "transcribe" || j.step === "diarize") return j.activity || "Working"
        if (j.step === "cut" && j.progress.n) return "Cutting " + Math.min(j.progress.i + 1, j.progress.n) + "/" + j.progress.n
        return j.activity || "Working"
    }
    function statusColor(j) {
        if (!j) return root.cFaint
        if (j.status === "failed") return root.cBad
        if (j.status === "done") return root.cOk
        if (j.status === "waiting") return root.cWord
        if (j.status === "running" || j.status === "queued") return root.cAccent
        return root.cFaint
    }

    // ── toast
    property string toastText: ""
    property bool toastBad: false
    function toast(t, bad) { root.toastText = t; root.toastBad = !!bad; toastTimer.restart() }
    Timer { id: toastTimer; interval: 4200; onTriggered: root.toastText = "" }

    Timer { interval: root.anyRunning ? 1000 : 4000; running: true; repeat: true; onTriggered: root.refresh() }
    Component.onCompleted: { registerViewDir(); refresh(); callVia(coreName, "listAgents", [], function (raw) { var o = parse(raw); if (o && o.jobs) apply(o) }) }

    component Field: TextField {
        color: root.cText
        placeholderTextColor: root.cFaint
        font.pixelSize: 14
        selectByMouse: true
        background: Rectangle { radius: 8; color: root.cBg; border.width: 1; border.color: parent.activeFocus ? root.cAccent : root.cLine }
    }
    component Label2: Text { color: root.cSub; font.pixelSize: 12; textFormat: Text.PlainText }
    component Section: Text { color: root.cFaint; font.pixelSize: 11; font.letterSpacing: 1.2; textFormat: Text.PlainText }
    component Bar: Rectangle {
        id: bar
        property real value: 0
        property bool indeterminate: false
        height: 6; radius: 3; color: root.cRaised; clip: true
        Rectangle {
            height: parent.height; radius: 3; color: root.cAccent
            width: bar.indeterminate ? bar.width * 0.3 : bar.width * Math.max(0, Math.min(1, bar.value))
            x: bar.indeterminate ? slide.x : 0
        }
        Item {
            id: slide
            NumberAnimation on x { running: bar.indeterminate && bar.visible; from: -bar.width * 0.3; to: bar.width; duration: 1400; loops: Animation.Infinite }
        }
    }

    Rectangle { anchors.fill: parent; color: root.cBg }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ── left: new job + the list of jobs
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 300
            color: root.cSurface
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: root.sp * 2
                spacing: root.sp
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "Reelcut"; color: root.cText; font.pixelSize: 20; font.bold: true; textFormat: Text.PlainText; Layout.fillWidth: true }
                    Text { text: root.st.version ? "v" + root.st.version : ""; color: root.cFaint; font.pixelSize: 11; textFormat: Text.PlainText }
                    // Settings: a labelled button, not a bare gear (easy to miss: vpavlin, 2026-10-15).
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
                        Accessible.role: Accessible.Button
                        Accessible.name: "Settings"
                    }
                }
                Label2 { text: "A long video in, short vertical reels with captions out."; wrapMode: Text.WordWrap; Layout.fillWidth: true }

                Field {
                    id: urlField
                    Layout.fillWidth: true
                    Layout.topMargin: root.sp
                    Layout.preferredHeight: 38
                    placeholderText: "Paste a video address"
                    onAccepted: root.start()
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 64
                    radius: 8; color: root.cBg
                    border.width: 1; border.color: briefNew.activeFocus ? root.cAccent : root.cLine
                    ScrollView {
                        anchors.fill: parent; anchors.margins: 4
                        TextArea {
                            id: briefNew
                            placeholderText: "What I'm looking for (optional): topics, people, tone, keywords"
                            placeholderTextColor: root.cFaint
                            color: root.cText; font.pixelSize: 12; wrapMode: TextEdit.Wrap; selectByMouse: true
                            background: null
                            // The last one used, until the person types.
                            readonly property string seed: root.st.settings ? root.st.settings.brief || "" : ""
                            onSeedChanged: if (!activeFocus) text = seed
                        }
                    }
                }
                LogosButton { text: "Make reels"; Layout.fillWidth: true; enabled: urlField.text.trim() !== "" && !!root.st.runner && !root.st.runner.error; onClicked: root.start() }
                Text {
                    text: "or use a video file…"
                    color: fileMouse.containsMouse ? root.cAccent : root.cSub; font.pixelSize: 12; textFormat: Text.PlainText
                    Layout.alignment: Qt.AlignHCenter
                    MouseArea { id: fileMouse; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: fileDialog.open() }
                }

                // Nothing can run: say why and how to fix it.
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
                    delegate: Rectangle {
                        required property var modelData
                        width: ListView.view.width
                        height: 62
                        radius: 8
                        readonly property bool active: root.job && modelData.id === root.job.id
                        color: active ? root.cRaised : (jMouse.containsMouse ? Qt.darker(root.cRaised, 1.15) : "transparent")
                        border.width: active ? 1 : 0
                        border.color: root.cAccent
                        MouseArea { id: jMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.selectedId = modelData.id }
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 4
                            Text { text: modelData.title || modelData.url; color: root.cText; font.pixelSize: 13; font.bold: active; elide: Text.ElideRight; Layout.fillWidth: true; textFormat: Text.PlainText }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                Rectangle { width: 7; height: 7; radius: 4; color: root.statusColor(modelData) }
                                Text { text: root.statusText(modelData); color: root.cSub; font.pixelSize: 11; elide: Text.ElideRight; Layout.fillWidth: true; textFormat: Text.PlainText }
                                Text { text: root.fmtDur(modelData.duration); color: root.cFaint; font.pixelSize: 11; font.features: { "tnum": 1 }; textFormat: Text.PlainText }
                            }
                            Bar {
                                Layout.fillWidth: true
                                visible: modelData.status === "running"
                                height: 3
                                indeterminate: !modelData.progress.n
                                value: modelData.progress.n ? modelData.progress.i / modelData.progress.n : 0
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

        // ── right: the selected job
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // Empty state
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
                    text: "A livestream, a Space, a talk or a podcast. Reelcut downloads it, transcribes it on this computer, "
                        + (root.st.settings && root.st.settings.agent ? "asks " + root.st.settings.agent + " for the best moments"
                                                                       : "helps you pick the best moments with any AI chat")
                        + " and cuts them into 9:16 reels with word-by-word captions."
                }
            }

            Flickable {
                id: page
                visible: !!root.job
                anchors.fill: parent
                contentWidth: width
                contentHeight: body.implicitHeight + 48
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                ColumnLayout {
                    id: body
                    x: 28; y: 24
                    width: page.width - 56
                    spacing: 16

                    // header
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text { text: root.job ? (root.job.title || root.job.url) : ""; color: root.cText; font.pixelSize: 20; font.bold: true; wrapMode: Text.WordWrap; Layout.fillWidth: true; textFormat: Text.PlainText }
                            Text {
                                text: root.job ? [root.job.channel, root.fmtDur(root.job.duration), root.job.words ? root.job.words + " words" : "", root.job.url || root.job.file].filter(function (x) { return !!x }).join("  ·  ") : ""
                                color: root.cFaint; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true; textFormat: Text.PlainText
                            }
                        }
                        LogosButton { text: "Open folder"; visible: !!root.job; onClicked: root.act("openPath", [root.job.dir]) }
                        LogosButton {
                            visible: !!root.job && (root.job.status === "running" || root.job.status === "queued")
                            text: "Stop"; onClicked: root.act("cancelJob", [root.job.id])
                        }
                        LogosButton {
                            visible: !!root.job && (root.job.status === "failed" || root.job.status === "cancelled")
                            text: "Retry"; onClicked: root.act("retryJob", [root.job.id])
                        }
                        Text {
                            text: "⋯"; color: root.cSub; font.pixelSize: 20; textFormat: Text.PlainText
                            MouseArea { anchors.fill: parent; anchors.margins: -8; cursorShape: Qt.PointingHandCursor; onClicked: jobMenu.popup() }
                        }
                    }

                    // the steps
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: ["Download", "Transcribe", "Speakers", "Pick clips", "Cut"]
                            delegate: ColumnLayout {
                                required property string modelData
                                required property int index
                                Layout.fillWidth: true
                                Layout.preferredWidth: 1
                                spacing: 6
                                readonly property int at: root.job ? root.stepIndex(root.job.step) : 0
                                readonly property bool here: index === at && root.job && root.job.status !== "done"
                                readonly property bool doneStep: index < at || (root.job && root.job.status === "done")
                                readonly property bool bad: here && root.job && root.job.status === "failed"
                                Rectangle {
                                    Layout.fillWidth: true
                                    height: 4; radius: 2
                                    color: parent.bad ? root.cBad : parent.doneStep ? root.cOk : parent.here ? root.cAccent : root.cRaised
                                }
                                Text {
                                    text: (parent.doneStep ? "✓ " : "") + modelData
                                    color: parent.here ? root.cText : parent.doneStep ? root.cSub : root.cFaint
                                    font.pixelSize: 12; font.bold: parent.here; textFormat: Text.PlainText
                                }
                            }
                        }
                    }

                    // what runs now
                    Rectangle {
                        visible: !!root.job && (root.job.status === "running" || root.job.status === "queued")
                        Layout.fillWidth: true
                        implicitHeight: runCol.implicitHeight + 28
                        radius: 10
                        color: root.cSurface
                        border.width: 1; border.color: root.cLine
                        ColumnLayout {
                            id: runCol
                            anchors.fill: parent; anchors.margins: 14
                            spacing: 8
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: root.job ? root.statusText(root.job) : ""; color: root.cText; font.pixelSize: 15; font.bold: true; Layout.fillWidth: true; textFormat: Text.PlainText }
                                Text {
                                    visible: !!root.job && root.job.step === "transcribe"
                                    text: "on this computer, nothing leaves it"; color: root.cFaint; font.pixelSize: 11; textFormat: Text.PlainText
                                }
                            }
                            Bar {
                                Layout.fillWidth: true
                                indeterminate: !!root.job && !root.job.progress.n
                                value: root.job && root.job.progress.n ? root.job.progress.i / root.job.progress.n : 0
                            }
                            Text {
                                visible: !!root.job && root.job.log.length > 0
                                text: root.job ? root.job.log.slice(-4).join("\n") : ""
                                color: root.cFaint; font.pixelSize: 11; font.family: "monospace"; elide: Text.ElideRight
                                Layout.fillWidth: true; textFormat: Text.PlainText; maximumLineCount: 4
                            }
                        }
                    }

                    // what went wrong
                    Rectangle {
                        visible: !!root.job && !!root.job.error && root.job.status !== "running"
                        Layout.fillWidth: true
                        implicitHeight: errCol.implicitHeight + 24
                        radius: 10
                        color: root.job && root.job.status === "failed" ? Qt.darker(root.cBad, 2.6) : root.cSurface
                        border.width: 1; border.color: root.job && root.job.status === "failed" ? root.cBad : root.cLine
                        ColumnLayout {
                            id: errCol
                            anchors.fill: parent; anchors.margins: 12
                            spacing: 6
                            Text { text: root.job ? root.job.error : ""; color: root.cText; font.pixelSize: 13; wrapMode: Text.WordWrap; Layout.fillWidth: true; textFormat: Text.PlainText }
                            Text {
                                visible: !!root.job && root.job.status === "failed" && root.job.log.length > 0
                                text: root.job ? root.job.log.slice(-6).join("\n") : ""
                                color: root.cSub; font.pixelSize: 11; font.family: "monospace"; wrapMode: Text.WrapAnywhere
                                Layout.fillWidth: true; textFormat: Text.PlainText
                            }
                        }
                    }

                    // the reels
                    ColumnLayout {
                        visible: !!root.job && root.job.reels.length > 0
                        Layout.fillWidth: true
                        spacing: 10
                        RowLayout {
                            Layout.fillWidth: true
                            Section { text: "REELS"; Layout.fillWidth: true }
                            Text {
                                visible: root.storeStatus !== ""
                                text: root.storeStatus
                                color: root.storeFailed ? root.cBad : root.cSub; font.pixelSize: 12; textFormat: Text.PlainText
                                Layout.maximumWidth: 360; elide: Text.ElideRight
                            }
                            LogosButton {
                                visible: !!root.job && !!root.job.storage && !!root.job.storage.set
                                text: "Copy the set's CID"
                                onClicked: root.copyText(root.job.storage.set)
                            }
                            LogosButton {
                                visible: !!root.st.settings && !!root.st.settings.storeTo && !!root.job && root.job.status === "done" && (root.storeMissing > 0 || root.storeFailed)
                                text: root.storeFailed ? "Try Storage again" : "Put in Storage"
                                onClicked: root.act("storeReels", [root.job.id], "Putting the reels in Logos Storage")
                            }
                            LogosButton { visible: !!root.job && !!root.job.page; text: "Watch in the browser"; onClicked: root.act("openPath", [root.job.page]) }
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 14
                            Repeater {
                                model: root.job ? root.job.reels : []
                                delegate: Rectangle {
                                    required property var modelData
                                    width: 180
                                    height: 320 + 84 + (root.storeOn ? 22 : 0) + (root.anyYoutube ? 22 : 0)
                                    radius: 10
                                    color: root.cSurface
                                    border.width: 1; border.color: rMouse.containsMouse ? root.cAccent : root.cLine
                                    clip: true
                                    Rectangle {
                                        id: thumbBox
                                        x: 0; y: 0; width: parent.width; height: 320
                                        color: "#000"
                                        Image {
                                            anchors.fill: parent
                                            source: modelData.thumb ? root.fileUrl(modelData.thumb) : ""
                                            fillMode: Image.PreserveAspectCrop
                                            asynchronous: true
                                            cache: false
                                        }
                                        Rectangle {
                                            anchors.centerIn: parent
                                            width: 52; height: 52; radius: 26
                                            color: Qt.rgba(0, 0, 0, rMouse.containsMouse ? 0.7 : 0.45)
                                            Text { anchors.centerIn: parent; anchors.horizontalCenterOffset: 2; text: "▶"; color: "#fff"; font.pixelSize: 20; textFormat: Text.PlainText }
                                        }
                                        Rectangle {
                                            anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 8
                                            visible: modelData.seconds > 0
                                            width: durT.implicitWidth + 10; height: 18; radius: 4
                                            color: Qt.rgba(0, 0, 0, 0.7)
                                            Text { id: durT; anchors.centerIn: parent; text: Math.round(modelData.seconds) + " s"; color: "#fff"; font.pixelSize: 11; textFormat: Text.PlainText }
                                        }
                                    }
                                    // The browser plays them reliably; a desktop player may lack a working hardware decoder
                                    // (VLC on NVIDIA without VDPAU: sound, no picture).
                                    MouseArea {
                                        id: rMouse; anchors.fill: thumbBox; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                        onClicked: root.act("openPath", [root.job.page ? root.job.page : modelData.file])
                                    }
                                    Text {
                                        x: 10; y: 328; width: parent.width - 20
                                        text: modelData.name; color: root.cText; font.pixelSize: 13; font.bold: true
                                        wrapMode: Text.WordWrap; maximumLineCount: 2; elide: Text.ElideRight; textFormat: Text.PlainText
                                    }
                                    // Its YouTube texts, one click each to the clipboard.
                                    Row {
                                        visible: root.anyYoutube
                                        x: 10; y: parent.height - 46 - (root.storeOn ? 22 : 0)
                                        spacing: 10
                                        Text { text: "YouTube:"; color: root.cFaint; font.pixelSize: 11; textFormat: Text.PlainText; visible: !!modelData.youtube }
                                        Text { text: "not written"; color: root.cFaint; font.pixelSize: 11; textFormat: Text.PlainText; visible: !modelData.youtube }
                                        Repeater {
                                            model: modelData.youtube ? [
                                                { label: "Title", text: modelData.youtube.title || "" },
                                                { label: "Text", text: modelData.youtube.description || "" },
                                                { label: "Tags", text: (modelData.youtube.tags || []).join(", ") }
                                            ] : []
                                            delegate: Text {
                                                required property var modelData
                                                text: modelData.label
                                                color: ytMouse.containsMouse ? root.cAccent : root.cSub; font.pixelSize: 11; textFormat: Text.PlainText
                                                MouseArea { id: ytMouse; anchors.fill: parent; anchors.margins: -3; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; enabled: !!parent.modelData.text; onClicked: root.copyText(parent.modelData.text) }
                                                ToolTip.visible: ytMouse.containsMouse && !!modelData.text; ToolTip.delay: 300
                                                ToolTip.text: "Copy: " + (modelData.text.length > 300 ? modelData.text.substring(0, 300) + "…" : modelData.text)
                                            }
                                        }
                                    }
                                    // In Logos Storage: its CID (click to copy), or how far it got.
                                    Text {
                                        visible: root.storeOn
                                        x: 10; y: parent.height - 46; width: parent.width - 20
                                        text: modelData.cid ? "CID " + modelData.cid
                                            : modelData.storing === "uploading" ? "Putting in Storage…"
                                            : modelData.storing ? modelData.storing.replace(/^failed: /, "Not in Storage: ") : ""
                                        color: modelData.storing && modelData.storing !== "uploading" ? root.cBad : cidMouse.containsMouse ? root.cAccent : root.cFaint
                                        font.pixelSize: 11; elide: Text.ElideMiddle; textFormat: Text.PlainText
                                        MouseArea { id: cidMouse; anchors.fill: parent; enabled: !!modelData.cid; hoverEnabled: true; cursorShape: modelData.cid ? Qt.PointingHandCursor : Qt.ArrowCursor; onClicked: root.copyText(modelData.cid) }
                                        ToolTip.visible: cidMouse.containsMouse && !!modelData.cid; ToolTip.delay: 400; ToolTip.text: "Copy the CID"
                                    }
                                    Text {
                                        x: 10; y: parent.height - 24
                                        text: "Open in the video player"
                                        color: vpMouse.containsMouse ? root.cAccent : root.cFaint; font.pixelSize: 11; textFormat: Text.PlainText
                                        MouseArea { id: vpMouse; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.act("openPath", [modelData.file]) }
                                    }
                                }
                            }
                        }
                    }

                    // YouTube texts for the reels (title, description, tags each): an agent writes them, or any AI chat.
                    Rectangle {
                        visible: !!root.job && root.job.reels.length > 0 && root.job.status === "done"
                        Layout.fillWidth: true
                        implicitHeight: ytCol.implicitHeight + 32
                        radius: 10
                        color: root.cSurface
                        border.width: 1; border.color: root.cLine
                        ColumnLayout {
                            id: ytCol
                            anchors.fill: parent; anchors.margins: 16
                            spacing: 10
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10
                                Text { text: "YouTube titles and descriptions"; color: root.cText; font.pixelSize: 15; font.bold: true; textFormat: Text.PlainText; Layout.fillWidth: true }
                                Text {
                                    visible: root.ytAsking
                                    text: "Asking " + (root.job && root.job.youtube ? root.job.youtube.who : "") + "…"
                                    color: root.cAccent; font.pixelSize: 12; textFormat: Text.PlainText
                                }
                                LogosButton { visible: root.ytAsking; text: "Stop"; onClicked: root.act("stopYoutube", [root.job.id]) }
                                ComboBox {
                                    id: ytAgentBox
                                    readonly property var opts: root.pickOpts
                                    visible: opts.length > 0 && !root.ytAsking
                                    Layout.preferredWidth: 260
                                    model: opts.map(function (o) { return o.label })
                                    currentIndex: {
                                        var want = root.job && root.job.agent ? root.job.agent : (root.st.settings ? root.st.settings.agent : "")
                                        for (var i = 0; i < opts.length; i++) if (opts[i].value === want) return i
                                        return opts.length ? 0 : -1
                                    }
                                }
                                LogosButton {
                                    visible: ytAgentBox.visible
                                    text: root.anyYoutube ? "Write them again" : "Write them"
                                    enabled: ytAgentBox.currentIndex >= 0
                                    onClicked: root.act("writeYoutube", [root.job.id, ytAgentBox.opts[ytAgentBox.currentIndex].value])
                                }
                            }
                            Text {
                                Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                                color: root.cSub; font.pixelSize: 13
                                text: "A keyword-minded title, a description and tags for each reel, from what is said in it. "
                                    + (root.anyYoutube ? "Copy them from each reel: Title, Text, Tags." : (ytAgentBox.visible ? "Ask an agent, or copy the prompt into any AI chat and paste its answer below." : "Copy the prompt into any AI chat, such as Claude or ChatGPT, and paste its answer below."))
                            }
                            Text {
                                visible: !!root.job && !!root.job.youtube && !!root.job.youtube.error
                                Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                                color: root.cBad; font.pixelSize: 12
                                text: root.job && root.job.youtube ? root.job.youtube.error : ""
                            }
                            RowLayout {
                                spacing: 10
                                LogosButton { text: "Copy the prompt"; onClicked: root.copyYoutubePrompt() }
                                Text { text: "then paste the answer:"; color: root.cFaint; font.pixelSize: 12; textFormat: Text.PlainText }
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 90
                                radius: 8; color: root.cBg
                                border.width: 1; border.color: ytAnswer.activeFocus ? root.cAccent : root.cLine
                                ScrollView {
                                    anchors.fill: parent; anchors.margins: 6
                                    TextArea {
                                        id: ytAnswer
                                        placeholderText: "Paste the answer here (=== REEL 1 === …)"
                                        placeholderTextColor: root.cFaint
                                        color: root.cText; font.pixelSize: 12; font.family: "monospace"
                                        wrapMode: TextEdit.Wrap; selectByMouse: true
                                        background: null
                                    }
                                }
                            }
                            LogosButton {
                                Layout.alignment: Qt.AlignRight
                                text: "Use this answer"
                                enabled: ytAnswer.text.trim() !== ""
                                onClicked: root.act("setYoutube", [root.job.id, ytAnswer.text], "Saved the YouTube texts", function () { ytAnswer.text = "" })
                            }
                        }
                    }

                    // The reels as replies in a forum thread: Forumer or Logos Forum.
                    Rectangle {
                        visible: !!root.job && root.job.reels.length > 0 && root.job.status === "done"
                        Layout.fillWidth: true
                        implicitHeight: forumCol.implicitHeight + 32
                        radius: 10
                        color: root.cSurface
                        border.width: 1; border.color: root.cLine
                        ColumnLayout {
                            id: forumCol
                            anchors.fill: parent; anchors.margins: 16
                            spacing: 10
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10
                                Text { text: "Post the reels to a forum"; color: root.cText; font.pixelSize: 15; font.bold: true; textFormat: Text.PlainText; Layout.fillWidth: true }
                                ComboBox {
                                    id: forumAppBox
                                    Layout.preferredWidth: 160
                                    model: ["Choose…", "Forumer", "Logos Forum"]
                                    currentIndex: root.forumApp === "forumer" ? 1 : root.forumApp === "logos_forum" ? 2 : 0
                                    onActivated: (i) => { if (i > 0) root.setForum({ app: i === 1 ? "forumer" : "logos_forum" }) }
                                }
                                Field {
                                    id: forumNameField
                                    visible: !!root.forumApp
                                    Layout.preferredWidth: 160; Layout.preferredHeight: 34
                                    readonly property string seed: root.job ? root.job.id + "|" + root.forumName : ""
                                    onSeedChanged: text = root.forumName
                                    placeholderText: root.forumApp === "forumer" ? "public" : "Logos Forum"
                                    onEditingFinished: if (text.trim() !== root.forumName) root.setForum({ forum: text.trim() })
                                    ToolTip.visible: hovered; ToolTip.delay: 500
                                    ToolTip.text: root.forumApp === "forumer" ? "The forum: \"public\" is Forumer's open forum." : "The forum's name: \"Logos Forum\" is the app's own."
                                }
                            }
                            Text {
                                visible: !root.forumApp
                                Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                                color: root.cSub; font.pixelSize: 13
                                text: "Each reel as a reply in a thread (say, the post with the full video): its YouTube title and description, and where to get it from Logos Storage. Reelcut signs the posts with its own key."
                            }
                            // the thread
                            ColumnLayout {
                                visible: !!root.forumApp
                                Layout.fillWidth: true
                                spacing: 8
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 10
                                    Text {
                                        Layout.fillWidth: true; elide: Text.ElideRight; textFormat: Text.PlainText; font.pixelSize: 13
                                        color: root.forumT.thread ? root.cText : root.cFaint
                                        text: root.forumT.thread ? "Thread: " + (root.forumT.threadTitle || root.forumT.thread) : "No thread yet: pick one, paste a link, or start one."
                                    }
                                    LogosButton { visible: !!root.forumT.thread; text: "Change"; onClicked: root.setForum({ thread: "" }) }
                                }
                                ColumnLayout {
                                    visible: !root.forumT.thread
                                    Layout.fillWidth: true
                                    spacing: 8
                                    RowLayout {
                                        visible: root.forumApp === "forumer"
                                        Layout.fillWidth: true
                                        spacing: 8
                                        Field { id: threadLinkField; Layout.fillWidth: true; Layout.preferredHeight: 34; placeholderText: "Paste a Forumer thread link (Share on the thread)" }
                                        LogosButton { text: "Use it"; enabled: threadLinkField.text.trim() !== ""; onClicked: { root.setForum({ thread: threadLinkField.text.trim() }); threadLinkField.text = "" } }
                                    }
                                    RowLayout {
                                        spacing: 10
                                        LogosButton {
                                            text: root.forumList && root.forumList.state === "loading" ? "Reading the threads…" : "Pick a recent thread"
                                            enabled: !(root.forumList && root.forumList.state === "loading")
                                            onClicked: root.act("loadForumThreads", [root.forumApp, root.forumName])
                                        }
                                        LogosButton { text: "Start a new thread"; onClicked: newThreadBox.visible = !newThreadBox.visible }
                                        Text {
                                            visible: !!root.st.delivery && root.st.delivery.state === "starting"
                                            text: "Connecting to Logos Delivery…"; color: root.cAccent; font.pixelSize: 12; textFormat: Text.PlainText
                                        }
                                    }
                                    Text {
                                        visible: !!root.forumList && !!root.forumList.error
                                        Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                                        color: root.cFaint; font.pixelSize: 12
                                        text: root.forumList ? root.forumList.error + " The list holds what the network's store nodes still keep" + (root.forumApp === "forumer" ? "; an older thread: paste its link." : ".") : ""
                                    }
                                    // the threads read back, newest first
                                    Repeater {
                                        model: root.forumList && root.forumList.threads ? root.forumList.threads.slice(0, 30) : []
                                        delegate: Rectangle {
                                            required property var modelData
                                            Layout.fillWidth: true
                                            implicitHeight: 34
                                            radius: 6
                                            color: thMouse.containsMouse ? root.cRaised : "transparent"
                                            RowLayout {
                                                anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8
                                                spacing: 10
                                                Text { text: modelData.title; color: root.cText; font.pixelSize: 13; elide: Text.ElideRight; Layout.fillWidth: true; textFormat: Text.PlainText }
                                                Text { text: modelData.author + " · " + new Date(modelData.ts).toLocaleDateString(Qt.locale(), Locale.ShortFormat); color: root.cFaint; font.pixelSize: 11; textFormat: Text.PlainText }
                                            }
                                            MouseArea { id: thMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.setForum({ thread: modelData.id, threadTitle: modelData.title }) }
                                        }
                                    }
                                    // a new thread: the full video's
                                    ColumnLayout {
                                        id: newThreadBox
                                        visible: false
                                        Layout.fillWidth: true
                                        spacing: 6
                                        Field { id: ntTitle; Layout.fillWidth: true; Layout.preferredHeight: 34; placeholderText: "Title"; text: root.job ? root.job.title || "" : "" }
                                        Rectangle {
                                            Layout.fillWidth: true; Layout.preferredHeight: 80
                                            radius: 8; color: root.cBg; border.width: 1; border.color: ntBody.activeFocus ? root.cAccent : root.cLine
                                            ScrollView {
                                                anchors.fill: parent; anchors.margins: 6
                                                TextArea {
                                                    id: ntBody
                                                    color: root.cText; font.pixelSize: 12; wrapMode: TextEdit.Wrap; selectByMouse: true; background: null
                                                    placeholderText: "What the thread is about"; placeholderTextColor: root.cFaint
                                                    text: root.job && root.job.url ? "Full video: " + root.job.url : ""
                                                }
                                            }
                                        }
                                        Field { id: ntTags; visible: root.forumApp === "forumer"; Layout.fillWidth: true; Layout.preferredHeight: 34; placeholderText: "Topic tags, comma-separated (Forumer needs one), e.g. dev club, video" }
                                        LogosButton {
                                            Layout.alignment: Qt.AlignRight
                                            text: "Start it and post the reels"
                                            enabled: ntTitle.text.trim() !== "" && (root.forumApp !== "forumer" || ntTags.text.trim() !== "")
                                            onClicked: { root.act("startForumThread", [root.job.id, ntTitle.text.trim(), ntBody.text, ntTags.text, true]); newThreadBox.visible = false }
                                        }
                                    }
                                }
                            }
                            // posting
                            RowLayout {
                                visible: !!root.forumT.thread
                                Layout.fillWidth: true
                                spacing: 10
                                CheckBox {
                                    id: forumAuto
                                    checked: !!root.forumT.auto
                                    onToggled: root.setForum({ auto: checked })
                                }
                                Text {
                                    Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText; color: root.cSub; font.pixelSize: 12
                                    text: "Post the reels by themselves once they are in Logos Storage (with their CIDs)"
                                }
                                Text {
                                    text: root.job && root.job.forum && root.job.forum.state === "waiting" ? "Storing the reels first, then posting…"
                                        : root.job && root.job.forum && root.job.forum.state === "posting" ? "Posting… " + root.forumPosted + " of " + root.job.reels.length
                                        : root.forumPosted ? root.forumPosted + " of " + root.job.reels.length + " posted" : ""
                                    color: root.cAccent; font.pixelSize: 12; textFormat: Text.PlainText
                                }
                                LogosButton {
                                    visible: !!root.job && !!root.job.forum && (root.job.forum.state === "posting" || root.job.forum.state === "waiting")
                                    text: "Stop"; onClicked: root.act("stopForum", [root.job.id])
                                }
                                LogosButton {
                                    visible: !!root.job && (!root.job.forum || (root.job.forum.state !== "posting" && root.job.forum.state !== "waiting")) && root.forumPosted < root.job.reels.length
                                    text: root.forumPosted ? "Post the rest" : "Post the reels"
                                    onClicked: root.act("postToForum", [root.job.id])
                                }
                            }
                            Text {
                                visible: !!root.job && !!root.job.forum && !!root.job.forum.error
                                Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                                color: root.cBad; font.pixelSize: 12
                                text: root.job && root.job.forum ? root.job.forum.error : ""
                            }
                        }
                    }

                    // who speaks: names, where each sits, the layout
                    Rectangle {
                        visible: root.spkLetters.length > 0 && !!root.job && !(root.job.status === "running" && root.job.step === "diarize")
                        Layout.fillWidth: true
                        implicitHeight: spkCol.implicitHeight + 32
                        radius: 10
                        color: root.cSurface
                        border.width: 1; border.color: root.job && root.job.step === "speakers" ? root.cWord : root.cLine
                        ColumnLayout {
                            id: spkCol
                            anchors.fill: parent; anchors.margins: 16
                            spacing: 12
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "Who speaks"; color: root.cText; font.pixelSize: 15; font.bold: true; textFormat: Text.PlainText; Layout.fillWidth: true }
                                Text {
                                    text: "Open the transcript"
                                    color: tr3Mouse.containsMouse ? root.cAccent : root.cSub; font.pixelSize: 12; textFormat: Text.PlainText
                                    MouseArea { id: tr3Mouse; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.act("openPath", [root.job.dir + "/transcript-ts.txt"]) }
                                }
                            }
                            Text {
                                Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                                color: root.cSub; font.pixelSize: 13
                                text: (root.spkLetters.length - 1) + " voices. Give each a name, as it should show on the reels, and say where they sit in the picture. "
                                    + "The names also tell the agent who is who."
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 16
                                // One frame of the video, to see who sits where.
                                Rectangle {
                                    visible: !!root.job && !!root.job.frame
                                    Layout.alignment: Qt.AlignTop
                                    Layout.preferredWidth: 320
                                    Layout.preferredHeight: root.job && root.job.width > 0 ? 320 * root.job.height / root.job.width : 180
                                    radius: 6; color: "#000"; clip: true
                                    Image {
                                        anchors.fill: parent
                                        source: root.job && root.job.frame ? root.fileUrl(root.job.frame) : ""
                                        fillMode: Image.PreserveAspectFit
                                        asynchronous: true
                                        cache: false
                                    }
                                    Text { anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 6; text: "left"; color: "#fff"; font.pixelSize: 11; style: Text.Outline; styleColor: "#000"; textFormat: Text.PlainText }
                                    Text { anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.margins: 6; visible: root.spkLetters.length > 3; text: "middle"; color: "#fff"; font.pixelSize: 11; style: Text.Outline; styleColor: "#000"; textFormat: Text.PlainText }
                                    Text { anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 6; text: "right"; color: "#fff"; font.pixelSize: 11; style: Text.Outline; styleColor: "#000"; textFormat: Text.PlainText }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignTop
                                    spacing: 14
                                    Repeater {
                                        id: spkRep
                                        model: root.spkLetters.slice(1)
                                        delegate: ColumnLayout {
                                            id: spkRow
                                            required property string modelData
                                            required property int index
                                            readonly property string letter: modelData
                                            readonly property var spk: root.job && root.job.speakers ? root.job.speakers : ({})
                                            readonly property string nameText: nameField.text.trim()
                                            readonly property string side: root.sideValues[sideBox.currentIndex] || ""
                                            // What is stored: put in the fields when it changes, not on every poll.
                                            readonly property string seedName: spk.names && spk.names[letter] ? spk.names[letter] : ""
                                            readonly property string seedSide: spk.sides && spk.sides[letter] ? spk.sides[letter] : ""
                                            onSeedNameChanged: nameField.text = seedName
                                            onSeedSideChanged: sideBox.currentIndex = Math.max(0, root.sideValues.indexOf(seedSide))
                                            Component.onCompleted: { nameField.text = seedName; sideBox.currentIndex = Math.max(0, root.sideValues.indexOf(seedSide)) }
                                            Layout.fillWidth: true
                                            spacing: 6
                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 10
                                                Rectangle {
                                                    width: 26; height: 26; radius: 13
                                                    color: root.cRaised; border.width: 2; border.color: root.speakerColour(spkRow.letter)
                                                    Text { anchors.centerIn: parent; text: spkRow.letter; color: root.speakerColour(spkRow.letter); font.pixelSize: 13; font.bold: true; textFormat: Text.PlainText }
                                                }
                                                Text {
                                                    text: {
                                                        var t = spkRow.spk.talk ? spkRow.spk.talk[spkRow.letter] || 0 : 0
                                                        return t >= 60 ? Math.round(t / 60) + " min" : Math.round(t) + " s"
                                                    }
                                                    color: root.cFaint; font.pixelSize: 12; font.features: { "tnum": 1 }; Layout.preferredWidth: 46; textFormat: Text.PlainText
                                                }
                                                Field {
                                                    id: nameField
                                                    Layout.fillWidth: true
                                                    Layout.preferredHeight: 34
                                                    placeholderText: "Name of " + spkRow.letter
                                                }
                                                ComboBox {
                                                    id: sideBox
                                                    Layout.preferredWidth: 150
                                                    model: ["Sits: not sure", "On the left", "In the middle", "On the right"]
                                                    // Two people: one side says the other.
                                                    onActivated: {
                                                        if (spkRep.count !== 2 || (currentIndex !== 1 && currentIndex !== 3)) return
                                                        var other = spkRep.itemAt(1 - spkRow.index)
                                                        if (other) other.setSide(currentIndex === 1 ? 3 : 1)
                                                    }
                                                }
                                            }
                                            function setSide(i) { sideBox.currentIndex = i }
                                            Repeater {
                                                model: spkRow.spk.samples && spkRow.spk.samples[spkRow.letter] ? spkRow.spk.samples[spkRow.letter] : []
                                                delegate: Text {
                                                    required property string modelData
                                                    Layout.fillWidth: true
                                                    Layout.leftMargin: 36
                                                    text: (modelData.match(/^\[([^\]]*)\]/) || ["", ""])[1] + "   “" + modelData.replace(/^\[[^\]]*\] /, "") + "”"
                                                    color: root.cSub; font.pixelSize: 12; font.italic: true
                                                    elide: Text.ElideRight; textFormat: Text.PlainText
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10
                                Label2 { text: "Layout" }
                                ComboBox {
                                    id: spkLayout
                                    Layout.preferredWidth: 270
                                    model: root.layouts.map(function (l) { return l.label })
                                    readonly property string seed: root.job ? root.job.layout || "" : ""
                                    onSeedChanged: currentIndex = root.layoutIndex(seed)
                                    Component.onCompleted: currentIndex = root.layoutIndex(seed)
                                }
                                Item { Layout.fillWidth: true }
                                LogosButton { text: "Save"; visible: !!root.job && root.job.step !== "speakers"; onClicked: root.saveSpeakers(false) }
                                LogosButton {
                                    visible: !!root.job && root.job.step === "speakers"
                                    text: "Go on: pick the clips"
                                    onClicked: root.saveSpeakers(true)
                                }
                            }
                            // The wrong count merges two people, or splits one.
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Text { text: "Not " + (root.spkLetters.length - 1) + " people? Count again with"; color: root.cFaint; font.pixelSize: 12; textFormat: Text.PlainText }
                                SpinBox { id: spkCount; from: 1; to: 8; value: Math.max(1, root.spkLetters.length - 1); editable: true; Layout.preferredWidth: 110 }
                                LogosButton {
                                    text: "Count again"
                                    enabled: !!root.job && root.job.status !== "running" && root.job.status !== "queued"
                                    onClicked: root.act("diarizeJob", [root.job.id, spkCount.value], "Telling the speakers apart again")
                                }
                                Item { Layout.fillWidth: true }
                            }
                        }
                    }

                    // the clips: who picks them, the list, the names to fix, cut
                    ColumnLayout {
                        visible: !!root.job && root.stepIndex(root.job.step) >= 3 && !(root.job.status === "running" && root.job.step !== "pick")
                        Layout.fillWidth: true
                        spacing: 10

                        RowLayout {
                            Layout.fillWidth: true
                            Section { text: "CLIPS"; Layout.fillWidth: true }
                            Text {
                                visible: !!root.job && root.job.step === "pick" && root.job.status === "running"
                                text: "Asking " + (root.job ? root.pickerLabel(root.job.agent) : "") + "…"; color: root.cAccent; font.pixelSize: 12; textFormat: Text.PlainText
                            }
                            ComboBox {
                                id: agentBox
                                readonly property var opts: root.pickOpts
                                visible: opts.length > 0
                                Layout.preferredWidth: 300
                                model: opts.map(function (o) { return o.label })
                                currentIndex: {
                                    var want = root.job && root.job.agent ? root.job.agent : (root.st.settings ? root.st.settings.agent : "")
                                    for (var i = 0; i < opts.length; i++) if (opts[i].value === want) return i
                                    return opts.length ? 0 : -1
                                }
                            }
                            LogosButton {
                                visible: agentBox.visible
                                text: root.job && root.job.clips.length ? "Ask again" : "Ask"
                                enabled: !!root.job && root.job.status !== "running" && agentBox.currentIndex >= 0
                                onClicked: {
                                    var who = agentBox.opts[agentBox.currentIndex].value, id = root.job.id
                                    root.setBrief(id, briefJob.text.trim(), function () { root.act("pickClips", [id, who]) })
                                }
                            }
                        }
                        // What the person is looking for: goes with the prompt, to an agent or a chat.
                        ColumnLayout {
                            visible: !!root.job && !(root.job.step === "pick" && root.job.status === "running")
                            Layout.fillWidth: true
                            spacing: 4
                            Label2 { text: "What I'm looking for: topics, people, tone, keywords. It counts more than the general advice." }
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 56
                                radius: 8; color: root.cBg
                                border.width: 1; border.color: briefJob.activeFocus ? root.cAccent : root.cLine
                                ScrollView {
                                    anchors.fill: parent; anchors.margins: 4
                                    TextArea {
                                        id: briefJob
                                        placeholderText: "e.g. privacy and coercion, sharp one-liners from Corey, nothing about the roadmap"
                                        placeholderTextColor: root.cFaint
                                        color: root.cText; font.pixelSize: 12; wrapMode: TextEdit.Wrap; selectByMouse: true
                                        background: null
                                        readonly property string seed: root.job ? root.job.id + "\n" + (root.job.brief || "") : ""
                                        onSeedChanged: text = root.job ? root.job.brief || "" : ""
                                        onActiveFocusChanged: if (!activeFocus && root.job && text.trim() !== (root.job.brief || "")) root.setBrief(root.job.id, text.trim())
                                    }
                                }
                            }
                        }

                        // No clips yet: pick them through any AI chat (no setup), or an agent on the mesh.
                        Rectangle {
                            visible: !!root.job && root.job.clips.length === 0 && !(root.job.step === "pick" && root.job.status === "running")
                            Layout.fillWidth: true
                            implicitHeight: pickCol.implicitHeight + 32
                            radius: 10
                            color: root.cSurface
                            border.width: 1; border.color: root.cLine
                            ColumnLayout {
                                id: pickCol
                                anchors.fill: parent; anchors.margins: 16
                                spacing: 10
                                Text { text: "Pick the moments"; color: root.cText; font.pixelSize: 15; font.bold: true; textFormat: Text.PlainText }
                                Text {
                                    Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                                    color: root.cSub; font.pixelSize: 13
                                    text: "Copy the prompt (instructions and the transcript) into any AI chat, such as Claude or ChatGPT, then paste its answer below."
                                }
                                RowLayout {
                                    spacing: 10
                                    LogosButton { text: "Copy the prompt"; onClicked: root.setBrief(root.job.id, briefJob.text.trim(), root.copyPrompt) }
                                    Text {
                                        text: "Open the transcript"
                                        color: tr2Mouse.containsMouse ? root.cAccent : root.cSub; font.pixelSize: 12; textFormat: Text.PlainText
                                        MouseArea { id: tr2Mouse; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.act("openPath", [root.job.dir + "/transcript-ts.txt"]) }
                                    }
                                }
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 120
                                    radius: 8; color: root.cBg
                                    border.width: 1; border.color: answerEdit.activeFocus ? root.cAccent : root.cLine
                                    ScrollView {
                                        anchors.fill: parent; anchors.margins: 6
                                        TextArea {
                                            id: answerEdit
                                            placeholderText: "Paste the answer here"
                                            placeholderTextColor: root.cFaint
                                            color: root.cText; font.pixelSize: 12; font.family: "monospace"
                                            wrapMode: TextEdit.Wrap; selectByMouse: true
                                            background: null
                                        }
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text {
                                        Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                                        color: root.cFaint; font.pixelSize: 12
                                        text: (root.st.localAgent && root.st.localAgent.ok) || (root.st.agents || []).length
                                            ? "Or ask an agent, above."
                                            : "With shrooms-agent on this computer (no mesh needed), Claude Code or pi picks them for you, no copying."
                                    }
                                    Text {
                                        visible: !(root.st.localAgent && root.st.localAgent.ok)
                                        text: "About shrooms-agent"
                                        color: shMouse.containsMouse ? root.cAccent : root.cSub; font.pixelSize: 12; textFormat: Text.PlainText
                                        MouseArea { id: shMouse; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.act("openUrl", ["https://github.com/vpavlin/shrooms"]) }
                                    }
                                    LogosButton {
                                        text: "Use these clips"
                                        enabled: answerEdit.text.trim() !== ""
                                        onClicked: root.act("setClips", [root.job.id, answerEdit.text], "", function () { answerEdit.text = "" })
                                    }
                                }
                            }
                        }

                        // the candidates, best first: tick the ones to cut
                        RowLayout {
                            visible: !!root.job && root.job.clips.length > 0
                            Layout.fillWidth: true
                            spacing: 14
                            Text {
                                Layout.fillWidth: true
                                text: root.job ? (root.job.chosen || []).length + " of " + root.job.clips.length + " ticked to cut" : ""
                                color: root.cSub; font.pixelSize: 12; textFormat: Text.PlainText
                            }
                            Text {
                                text: "Tick all"
                                color: allMouse.containsMouse ? root.cAccent : root.cSub; font.pixelSize: 12; textFormat: Text.PlainText
                                MouseArea { id: allMouse; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.tickAll(true) }
                            }
                            Text {
                                text: "None"
                                color: noneMouse.containsMouse ? root.cAccent : root.cSub; font.pixelSize: 12; textFormat: Text.PlainText
                                MouseArea { id: noneMouse; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.tickAll(false) }
                            }
                        }
                        Repeater {
                            model: root.clipRows
                            delegate: Rectangle {
                                id: cand
                                required property var modelData
                                readonly property bool picked: !!root.job && (root.job.chosen || []).indexOf(modelData.n) >= 0
                                property bool open: false
                                property string words: ""
                                Layout.fillWidth: true
                                implicitHeight: candCol.implicitHeight + 16
                                radius: 8
                                color: picked ? root.cRaised : "transparent"
                                border.width: 1; border.color: picked ? root.cAccent : root.cLine
                                ColumnLayout {
                                    id: candCol
                                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                                    anchors.margins: 8
                                    spacing: 4
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 10
                                        CheckBox {
                                            id: candBox
                                            checked: cand.picked
                                            onToggled: { root.tick(cand.modelData.n, checked); checked = Qt.binding(function () { return cand.picked }) }
                                        }
                                        Text { text: cand.modelData.n; color: root.cFaint; font.pixelSize: 13; Layout.preferredWidth: 18; textFormat: Text.PlainText }
                                        Text { text: cand.modelData.hook; color: root.cText; font.pixelSize: 14; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true; textFormat: Text.PlainText }
                                        Text { text: cand.modelData.speaker; color: root.cSub; font.pixelSize: 12; elide: Text.ElideRight; Layout.maximumWidth: 140; textFormat: Text.PlainText }
                                        Text { text: Math.round(cand.modelData.seconds) + " s"; color: cand.modelData.seconds > 60 ? root.cBad : root.cSub; font.pixelSize: 12; font.features: { "tnum": 1 }; Layout.preferredWidth: 40; horizontalAlignment: Text.AlignRight; textFormat: Text.PlainText }
                                        Text { text: cand.modelData.start + " – " + cand.modelData.end; color: root.cFaint; font.pixelSize: 12; font.features: { "tnum": 1 }; Layout.preferredWidth: 130; horizontalAlignment: Text.AlignRight; textFormat: Text.PlainText }
                                    }
                                    Text {
                                        visible: !!cand.modelData.summary
                                        Layout.fillWidth: true; Layout.leftMargin: 66
                                        text: cand.modelData.summary || ""
                                        color: root.cSub; font.pixelSize: 12; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                                    }
                                    Text {
                                        Layout.leftMargin: 66
                                        text: cand.open ? "Hide the words" : "Show the words"
                                        color: wordsMouse.containsMouse ? root.cAccent : root.cFaint; font.pixelSize: 11; textFormat: Text.PlainText
                                        MouseArea {
                                            id: wordsMouse; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                if (cand.open) { cand.open = false; return }
                                                root.callVia(root.coreName, "clipText", [root.job.id, cand.modelData.n], function (raw) {
                                                    var o = root.parse(raw)
                                                    cand.words = o && o.ok ? (o.text || "(no lines in that range)") : "Could not read the transcript."
                                                    cand.open = true
                                                })
                                            }
                                        }
                                    }
                                    Text {
                                        visible: cand.open
                                        Layout.fillWidth: true; Layout.leftMargin: 66
                                        text: cand.words
                                        color: root.cText; font.pixelSize: 12; font.family: "monospace"; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                                    }
                                }
                            }
                        }

                        // raw list, editable; the transcript to write it from
                        RowLayout {
                        spacing: 18
                        Text {
                            visible: !!root.job && root.spkLetters.length === 0 && !(root.job.speakers && root.job.speakers.found)
                                     && root.job.status !== "running" && root.job.status !== "queued"
                            text: "Tell the speakers apart"
                            color: daMouse.containsMouse ? root.cAccent : root.cSub; font.pixelSize: 12; textFormat: Text.PlainText
                            MouseArea { id: daMouse; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.act("diarizeJob", [root.job.id, 0], "Telling the speakers apart") }
                        }
                        Text {
                            visible: !!root.job && root.job.clips.length > 0
                            text: clipsArea.visible ? "Hide the list" : "Edit the list"
                            color: editMouse.containsMouse ? root.cAccent : root.cSub; font.pixelSize: 12; textFormat: Text.PlainText
                            MouseArea {
                                id: editMouse; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: { clipsArea.visible = !clipsArea.visible; if (clipsArea.visible) clipsEdit.text = root.job.clipsText }
                            }
                        }
                        Text {
                            visible: !!root.job && root.job.clips.length > 0
                            text: "Open the transcript"
                            color: trMouse.containsMouse ? root.cAccent : root.cSub; font.pixelSize: 12; textFormat: Text.PlainText
                            MouseArea { id: trMouse; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.act("openPath", [root.job.dir + "/transcript-ts.txt"]) }
                        }
                        }
                        ColumnLayout {
                            id: clipsArea
                            visible: false
                            Layout.fillWidth: true
                            spacing: 6
                            Label2 { text: "One clip per line:  N | START H:MM:SS | END H:MM:SS | speaker | hook | until LASTWORD"; font.family: "monospace" }
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 150
                                radius: 8; color: root.cBg
                                border.width: 1; border.color: clipsEdit.activeFocus ? root.cAccent : root.cLine
                                ScrollView {
                                    anchors.fill: parent; anchors.margins: 6
                                    TextArea {
                                        id: clipsEdit
                                        color: root.cText; font.pixelSize: 12; font.family: "monospace"
                                        wrapMode: TextEdit.NoWrap; selectByMouse: true
                                        background: null
                                    }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Item { Layout.fillWidth: true }
                                LogosButton { text: "Save the list"; onClicked: root.act("setClips", [root.job.id, clipsEdit.text], "Saved") }
                            }
                        }
                        // an agent's reply with no clip lines, to copy from
                        Text {
                            visible: !!root.job && root.job.clips.length === 0 && !!root.job.pickReply
                            text: root.job ? root.job.pickReply : ""
                            color: root.cSub; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true; textFormat: Text.PlainText
                        }

                        // the opening and closing cards (a λ, a QR code, a URL)
                        RowLayout {
                            visible: !!root.job && root.job.clips.length > 0
                            Layout.fillWidth: true
                            Layout.topMargin: 6
                            spacing: 14
                            Repeater {
                                model: [{ key: "intro", label: "Opening card" }, { key: "outro", label: "Closing card" }]
                                delegate: RowLayout {
                                    required property var modelData
                                    spacing: 8
                                    readonly property string file: root.job ? root.job[modelData.key] || "" : ""
                                    Rectangle {
                                        width: 34; height: 34; radius: 4; color: "#000"; clip: true
                                        border.width: 1; border.color: root.cLine
                                        Image { anchors.fill: parent; anchors.margins: 1; fillMode: Image.PreserveAspectFit; asynchronous: true; source: root.cardThumb(parent.parent.file) ? root.fileUrl(root.cardThumb(parent.parent.file)) : "" }
                                    }
                                    ColumnLayout {
                                        spacing: 2
                                        Label2 { text: modelData.label }
                                        ComboBox {
                                            id: cardBox
                                            Layout.preferredWidth: 220
                                            Layout.preferredHeight: 32
                                            model: ["None"].concat(root.cards.map(function (c) { return c.name })).concat(["Add an image…"])
                                            readonly property string seed: parent.parent.file + "|" + root.cards.length
                                            onSeedChanged: currentIndex = root.cardIndex(parent.parent.file)
                                            Component.onCompleted: currentIndex = root.cardIndex(parent.parent.file)
                                            onActivated: function (i) {
                                                var key = parent.parent.modelData.key
                                                if (i === count - 1) { currentIndex = root.cardIndex(parent.parent.file); root.cardFor = key; cardDialog.open(); return }
                                                var o = {}; o[key] = i === 0 ? "" : root.cards[i - 1].file
                                                root.act("setJob", [root.job.id, JSON.stringify(o)])
                                            }
                                        }
                                    }
                                }
                            }
                            Item { Layout.fillWidth: true }
                        }

                        // names, then cut
                        RowLayout {
                            visible: !!root.job && root.job.clips.length > 0
                            Layout.fillWidth: true
                            Layout.topMargin: 6
                            spacing: 10
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4
                                Label2 { text: "Misheard names in the captions:  Heard=Meant, one or more" }
                                Field {
                                    id: fixField
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 34
                                    placeholderText: "Sterling=Sterlin, Meakin=Mencken"
                                    // Put in when the stored value changes, not on every poll (that would undo typing).
                                    readonly property string seed: root.job ? root.job.id + "\n" + (root.job.fixes || "") : ""
                                    onSeedChanged: text = root.job ? root.job.fixes || "" : ""
                                    onAccepted: cutBtn.clicked()
                                }
                            }
                            ColumnLayout {
                                spacing: 4
                                Label2 { text: "Layout" }
                                ComboBox {
                                    id: cutLayout
                                    Layout.preferredWidth: 250
                                    Layout.preferredHeight: 34
                                    model: root.layouts.map(function (l) { return l.label })
                                    readonly property string seed: root.job ? root.job.layout || "" : ""
                                    onSeedChanged: currentIndex = root.layoutIndex(seed)
                                    Component.onCompleted: currentIndex = root.layoutIndex(seed)
                                    onActivated: root.act("setSpeakers", [root.job.id, JSON.stringify({ layout: root.layoutValue(currentIndex) }), false])
                                }
                            }
                            LogosButton {
                                id: cutBtn
                                Layout.alignment: Qt.AlignBottom
                                text: {
                                    if (!root.job) return "Cut"
                                    var n = (root.job.chosen || []).length
                                    return n ? "Cut these " + n : "Cut all " + root.job.clips.length
                                }
                                enabled: !!root.job && root.job.status !== "running" && root.job.status !== "queued"
                                onClicked: root.act("cutReels", [root.job.id, fixField.text])
                            }
                        }
                    }
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

    // Core missing or gone.
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
        MenuItem { text: "Copy the address"; enabled: !!root.job && !!root.job.url; onTriggered: { clip.text = root.job.url; clip.selectAll(); clip.copy(); root.toast("Copied", false) } }
        MenuItem { text: "Remove from the list"; onTriggered: root.act("removeJob", [root.job.id, false]) }
        MenuItem { text: "Delete it and its files"; onTriggered: confirmDelete.open() }
    }
    TextEdit { id: clip; visible: false }

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
        component Small: Field { Layout.preferredWidth: 64; Layout.preferredHeight: 32; validator: DoubleValidator { bottom: 0; top: 10 } }
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
            Field { id: crField; Layout.preferredWidth: 150; Layout.preferredHeight: 32; placeholderText: cr.hint; maximumLength: 7 }
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
                Field {
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
                    Field { id: forumAliasField; Layout.preferredWidth: 260; Layout.preferredHeight: 34; maximumLength: 32; placeholderText: "Reelcut" }
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
