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
        act("startJob", [u], "", function (o) {
            urlField.text = ""
            if (o.jobs && o.jobs.length) root.selectedId = o.jobs[0].id
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

    // The prompt for any AI chat, to the clipboard.
    function copyPrompt() {
        callVia(coreName, "getPrompt", [root.job.id], function (raw) {
            var o = parse(raw)
            if (!o || o.ok === false) { toast(o && o.error ? o.error : "Could not build the prompt.", true); return }
            clip.text = o.text; clip.selectAll(); clip.copy(); clip.deselect(); clip.text = ""
            toast("Copied the prompt. Paste it into an AI chat, then paste the answer here.", false)
        })
    }
    function importUrl(u) {
        act("importFile", [String(u)], "", function (o) { if (o.jobs && o.jobs.length) root.selectedId = o.jobs[0].id })
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
                    Text {
                        text: "⚙"; color: gearMouse.containsMouse ? root.cAccent : root.cSub; font.pixelSize: 17; textFormat: Text.PlainText
                        MouseArea { id: gearMouse; anchors.fill: parent; anchors.margins: -6; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: settings.openIt() }
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
                                    height: 320 + 84
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
                                            readonly property string side: ["", "left", "right"][sideBox.currentIndex] || ""
                                            // What is stored: put in the fields when it changes, not on every poll.
                                            readonly property string seedName: spk.names && spk.names[letter] ? spk.names[letter] : ""
                                            readonly property string seedSide: spk.sides && spk.sides[letter] ? spk.sides[letter] : ""
                                            onSeedNameChanged: nameField.text = seedName
                                            onSeedSideChanged: sideBox.currentIndex = Math.max(0, ["", "left", "right"].indexOf(seedSide))
                                            Component.onCompleted: { nameField.text = seedName; sideBox.currentIndex = Math.max(0, ["", "left", "right"].indexOf(seedSide)) }
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
                                                    model: ["Sits: not sure", "On the left", "On the right"]
                                                    // Two people: one side says the other.
                                                    onActivated: {
                                                        if (spkRep.count !== 2 || currentIndex === 0) return
                                                        var other = spkRep.itemAt(1 - spkRow.index)
                                                        if (other) other.setSide(currentIndex === 1 ? 2 : 1)
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
                                onClicked: root.act("pickClips", [root.job.id, agentBox.opts[agentBox.currentIndex].value])
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
                                    LogosButton { text: "Copy the prompt"; onClicked: root.copyPrompt() }
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

                        // parsed list
                        Repeater {
                            model: root.job ? root.job.clips : []
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 12
                                Text { text: modelData.n; color: root.cFaint; font.pixelSize: 13; Layout.preferredWidth: 18; textFormat: Text.PlainText }
                                Text { text: modelData.start + " – " + modelData.end; color: root.cSub; font.pixelSize: 12; font.features: { "tnum": 1 }; Layout.preferredWidth: 140; textFormat: Text.PlainText }
                                Text { text: Math.round(modelData.seconds) + " s"; color: modelData.seconds > 60 ? root.cBad : root.cFaint; font.pixelSize: 12; Layout.preferredWidth: 40; textFormat: Text.PlainText }
                                Text { text: modelData.hook; color: root.cText; font.pixelSize: 13; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true; textFormat: Text.PlainText }
                                Text { text: modelData.speaker; color: root.cSub; font.pixelSize: 12; elide: Text.ElideRight; Layout.preferredWidth: 120; textFormat: Text.PlainText }
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
                                text: root.job && root.job.reels.length ? "Cut again" : "Cut " + (root.job ? root.job.clips.length : 0) + " reels"
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
            countBox.value = s.count || 6
            diarizeBox.checked = s.diarize !== false
            autoCutBox.checked = s.autoCut !== false
            var vals = setAgent.opts.map(function (o) { return o.value })
            var want = s.agent === undefined ? "auto" : s.agent
            setAgent.currentIndex = Math.max(0, vals.indexOf(want))
            runnerBox.currentIndex = Math.max(0, ["auto", "podman", "docker", "local"].indexOf(s.runner || "auto"))
            open()
            root.callVia(root.coreName, "listAgents", [], function (raw) { var o = root.parse(raw); if (o && o.jobs) root.apply(o) })
        }
        function save() {
            var agent = setAgent.currentIndex >= 0 ? setAgent.opts[setAgent.currentIndex].value : ""
            root.act("setSettings", [JSON.stringify({ agent: agent, count: countBox.value, autoCut: autoCutBox.checked, diarize: diarizeBox.checked,
                runner: ["auto", "podman", "docker", "local"][runnerBox.currentIndex] })], "Saved")
            close()
        }
        anchors.centerIn: parent
        width: 460
        modal: true
        padding: 20
        background: Rectangle { radius: 12; color: root.cSurface; border.width: 1; border.color: root.cLine }
        contentItem: ColumnLayout {
            spacing: 8
            Text { text: "Settings"; color: root.cText; font.pixelSize: 17; font.bold: true; textFormat: Text.PlainText }
            Label2 { text: "Who picks the clips"; Layout.topMargin: 6 }
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
                Label2 { text: "Reels per video"; Layout.fillWidth: true }
                SpinBox { id: countBox; from: 1; to: 20; editable: true }
            }
            CheckBox {
                id: autoCutBox
                text: "Cut as soon as the agent answers"
                contentItem: Text { text: autoCutBox.text; color: root.cText; font.pixelSize: 13; leftPadding: autoCutBox.indicator.width + 6; verticalAlignment: Text.AlignVCenter; textFormat: Text.PlainText }
            }
            CheckBox {
                id: diarizeBox
                text: "Tell the speakers apart (about 7 min per hour of video)"
                contentItem: Text { text: diarizeBox.text; color: root.cText; font.pixelSize: 13; leftPadding: diarizeBox.indicator.width + 6; verticalAlignment: Text.AlignVCenter; textFormat: Text.PlainText }
            }
            Text {
                Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                color: root.cFaint; font.pixelSize: 12
                text: "For interviews and calls: you name the people once, the reels show who speaks, and two people on a call are cut one above the other."
            }
            Label2 { text: "Run reelcut with"; Layout.topMargin: 6 }
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
                    if (r.kind) return "Using " + r.kind + " with " + r.image + (r.imagePresent ? "." : " (downloaded on the first job, about 1 GB).")
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
