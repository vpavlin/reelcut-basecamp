import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls

// Screen 4: framing by hand. Per clip, per scene (where the picture changes): any number of pieces of
// the picture ("src", on the frame, left), each placed anywhere on the reel ("dst", right) over a
// background (blurred picture, black, or a colour). A piece keeps the shape of its place on the reel.
// Saved per clip as reelcut's "panels" layout; the scenes and their faces come from the core, found
// in the background as soon as the clips are picked.
Item {
    id: scr
    property var app
    property bool active: false
    readonly property var job: app.job

    // ── what is open
    property int clipN: 0
    property int idx: 0           // the scene
    property int sel: 0           // the piece
    property var edits: []        // per scene: {start, bg, panels: [{src, dst}]}
    property bool dirty: false
    property string seededKey: ""

    readonly property var info: app.scenes && app.scenes.clips ? app.scenes.clips[String(clipN)] || null : null
    readonly property var sceneList: info && info.scenes ? info.scenes : []
    readonly property var scene: idx < sceneList.length ? sceneList[idx] : null
    readonly property var edit: idx < edits.length ? edits[idx] : null
    readonly property var faces: scene ? scene.faces || [] : []
    readonly property real vw: job && job.width ? job.width : 16
    readonly property real vh: job && job.height ? job.height : 9
    readonly property real ow: app.jobRatio.w
    readonly property real oh: app.jobRatio.h
    readonly property var clipRow: {
        for (var i = 0; i < app.clips.length; i++) if (app.clips[i].n === clipN) return app.clips[i]
        return null
    }
    // The clips: ticked first.
    readonly property var order: {
        var t = app.clips.filter(function (c) { return app.chosen.indexOf(c.n) >= 0 })
        return t.concat(app.clips.filter(function (c) { return app.chosen.indexOf(c.n) < 0 }))
    }

    // Opening on a clip: the one asked for, else the first ticked; unsaved work saved first.
    onActiveChanged: { if (active) openClip(app.frameClip || (order.length ? order[0].n : 0)); else if (dirty) save(true) }
    Connections { target: scr.app; function onFrameClipChanged() { if (scr.active) scr.openClip(scr.app.frameClip) } }
    function openClip(n) {
        if (!n || n === clipN && seededKey !== "") return
        if (dirty) save(true)
        clipN = n; idx = 0; sel = 0; seededKey = ""; edits = []; dirty = false
        seed()
        var sc = app.scenes || {}
        if (sc.state !== "running" && !info && job) app.act("findScenes", [job.id])
    }
    onSceneListChanged: seed()
    onOwChanged: refitAll()
    onOhChanged: refitAll()

    // ── geometry: boxes are [x, y, w, h], src in fractions of the frame, dst of the reel
    function clamp(v, a, b) { return Math.max(a, Math.min(b, v)) }
    // A piece's shape on the frame: its place's shape on the reel, in frame fractions.
    function srcAspect(dst) { return (dst[2] * ow) / Math.max(1e-6, dst[3] * oh) * vh / vw }
    function fitSrc(src, dst) {
        var a = srcAspect(dst)
        var cx = src[0] + src[2] / 2, cy = src[1] + src[3] / 2, h = src[3], w = h * a
        if (w > 1) { w = 1; h = w / a }
        if (h > 1) { h = 1; w = h * a }
        return [clamp(cx - w / 2, 0, 1 - w), clamp(cy - h / 2, 0, 1 - h), w, h]
    }
    // Around a face: k face-heights tall, the face a little above the middle.
    function faceSrc(f, dst, k) {
        var a = srcAspect(dst)
        var h = Math.min(1, f[3] * k), w = h * a
        if (w > 1) { w = 1; h = w / a }
        var cx = f[0] + f[2] / 2, cy = f[1] + f[3] / 2
        return [clamp(cx - w / 2, 0, 1 - w), clamp(cy - 0.42 * h, 0, 1 - h), w, h]
    }
    // The whole picture across the reel's width.
    function wholeDst() {
        var h = Math.min(1, (ow * vh / vw) / oh)
        return [0, (1 - h) / 2, 1, h]
    }
    // The biggest piece of the given shape inside a region of the frame.
    function biggestIn(r, dst) {
        var a = srcAspect(dst), w = r[2], h = w / a
        if (h > r[3]) { h = r[3]; w = h * a }
        return [r[0] + (r[2] - w) / 2, r[1] + (r[3] - h) / 2, w, h]
    }

    // ── starting points
    function preset(kind, sc) {
        var fs = (sc && sc.faces ? sc.faces : []).slice()
        if (kind === "whole" || (kind !== "whole" && !fs.length)) return [{ src: [0, 0, 1, 1], dst: wholeDst() }]
        if (kind === "one") {
            var d1 = [0, 0, 1, 1]
            return [{ src: faceSrc(fs[0], d1, 3.2), dst: d1 }]
        }
        if (kind === "faces") {
            var n = Math.min(6, fs.length)
            fs = fs.slice(0, n).sort(function (a, b) { return Math.abs(a[1] - b[1]) > 0.15 ? a[1] - b[1] : a[0] - b[0] })
            var cols = n <= 3 ? 1 : 2, rows = Math.ceil(n / cols), out = []
            for (var i = 0; i < n; i++) {
                var c = i % cols, r = Math.floor(i / cols)
                var d = [c / cols, r / rows, 1 / cols, 1 / rows]
                out.push({ src: faceSrc(fs[i], d, n === 1 ? 3.2 : 2.4), dst: d })
            }
            return out
        }
        if (kind === "screen") {
            // A face on top, the rest of the picture (a shared screen, a slide) under it.
            var f = fs[0], top = [0, 0, 1, 0.42], bottom = [0, 0.42, 1, 0.58]
            var left = f[0], right = 1 - f[0] - f[2]
            var region = right > left ? [f[0] + f[2] + 0.01, 0, 1 - f[0] - f[2] - 0.01, 1] : [0, 0, Math.max(0.05, f[0] - 0.01), 1]
            return [{ src: faceSrc(f, top, 2.2), dst: top }, { src: biggestIn(region, bottom), dst: bottom }]
        }
        return [{ src: [0, 0, 1, 1], dst: wholeDst() }]
    }
    function guess(sc) {
        var fs = sc && sc.faces ? sc.faces : []
        if (!fs.length) return "whole"
        var small = fs.every(function (f) { return f[2] < 0.14 })
        var oneSide = fs.every(function (f) { return (f[0] + f[2] / 2 < 0.5) === (fs[0][0] + fs[0][2] / 2 < 0.5) })
        if (small && oneSide) return "screen"
        if (fs.length <= 4) return "faces"
        return "whole"
    }
    // A saved scene in any of reelcut's layouts, as pieces.
    function asPanels(e) {
        if (e.layout === "panels") return (e.panels || []).map(function (p) { return { src: p.src.slice(), dst: p.dst.slice() } })
        var b = e.boxes || []
        if (e.layout === "crop" && b.length) return [{ src: b[0].slice(), dst: [0, 0, 1, 1] }]
        if (e.layout === "split" && b.length > 1) return [{ src: b[0].slice(), dst: [0, 0, 1, 0.5] }, { src: b[1].slice(), dst: [0, 0.5, 1, 0.5] }]
        return [{ src: [0, 0, 1, 1], dst: wholeDst() }]
    }
    function refit(e) { return { start: e.start, bg: e.bg, panels: e.panels.map(function (p) { return { src: fitSrc(p.src, p.dst), dst: p.dst } }) } }
    // From what was saved (each scene takes what was in force at its start), or a guess per scene.
    function seed() {
        var key = clipN + "|" + sceneList.map(function (x) { return x.start }).join(",")
        if (!sceneList.length || key === seededKey) return
        seededKey = key
        var saved = app.framing[String(clipN)] ? app.framing[String(clipN)].scenes || [] : []
        var out = []
        for (var i = 0; i < sceneList.length; i++) {
            var sc = sceneList[i], e = null
            for (var j = 0; j < saved.length; j++) if (saved[j].start <= sc.start + 0.05) e = saved[j]
            if (!e && saved.length) e = saved[0]
            out.push(refit(e ? { start: sc.start, bg: e.bg || "blur", panels: asPanels(e) }
                             : { start: sc.start, bg: "blur", panels: preset(guess(sc), sc) }))
        }
        edits = out
        if (idx >= out.length) idx = 0
        sel = 0
        dirty = false
    }
    function refitAll() { if (edits.length) { edits = edits.map(refit); dirty = true } }
    function put(e) { var a = edits.slice(); a[idx] = e; edits = a; dirty = true }
    function usePreset(kind) {
        if (!edit) return
        put(refit({ start: edit.start, bg: edit.bg, panels: preset(kind, scene) }))
        sel = 0
    }
    function moveSrc(k, nb) {
        var ps = edit.panels.slice()
        ps[k] = { src: nb, dst: ps[k].dst }
        put({ start: edit.start, bg: edit.bg, panels: ps })
    }
    function moveDst(k, nd) {
        var ps = edit.panels.slice()
        ps[k] = { src: fitSrc(ps[k].src, nd), dst: nd }
        put({ start: edit.start, bg: edit.bg, panels: ps })
    }
    // Less (f < 1) or more (f > 1) of the picture in a box, around its middle: zooming out makes faces smaller.
    function zoom(k, f) {
        if (!edit || k >= edit.panels.length) return
        var p = edit.panels[k], cx = p.src[0] + p.src[2] / 2, cy = p.src[1] + p.src[3] / 2
        var h = clamp(p.src[3] * f, 0.03, 1), w = p.src[2] * f
        var ps = edit.panels.slice()
        ps[k] = { src: fitSrc([cx - w / 2, cy - h / 2, w, h], p.dst), dst: p.dst }
        put({ start: edit.start, bg: edit.bg, panels: ps })
    }
    function placeDst(k, nd) { if (edit && k < edit.panels.length) moveDst(k, nd) }
    function toFront(k) {
        if (!edit || k >= edit.panels.length - 1) return
        var ps = edit.panels.slice(), p = ps.splice(k, 1)[0]
        ps.push(p)
        put({ start: edit.start, bg: edit.bg, panels: ps })
        sel = ps.length - 1
    }
    function addPanel() {
        if (!edit || edit.panels.length >= 12) return
        var d = [0.25, 0.35, 0.5, 0.3]
        var f = faces.length > edit.panels.length ? faces[edit.panels.length] : null
        var ps = edit.panels.concat([{ src: f ? faceSrc(f, d, 2.4) : fitSrc([0.3, 0.3, 0.4, 0.4], d), dst: d }])
        put({ start: edit.start, bg: edit.bg, panels: ps })
        sel = ps.length - 1
    }
    function removePanel() {
        if (!edit || edit.panels.length <= 1) return
        var ps = edit.panels.slice(); ps.splice(sel, 1)
        put({ start: edit.start, bg: edit.bg, panels: ps })
        sel = Math.max(0, sel - 1)
    }
    function setBg(v) { if (edit) put({ start: edit.start, bg: v, panels: edit.panels }) }
    function copyOf(e, start) { return { start: start, bg: e.bg, panels: e.panels.map(function (p) { return { src: p.src.slice(), dst: p.dst.slice() } }) } }
    function toFollowing() {
        var a = edits.slice()
        for (var i = idx + 1; i < a.length; i++) a[i] = copyOf(edit, a[i].start)
        edits = a; dirty = true
        app.toast("Scenes " + (idx + 2) + "–" + a.length + " framed as this one", false)
    }
    function likeBefore() { if (idx > 0) put(copyOf(edits[idx - 1], edit.start)) }
    function save(quiet, then) {
        if (!job || !edits.length) return
        var n = clipN
        var body = JSON.stringify({ scenes: edits.map(function (e) { return { start: e.start, layout: "panels", bg: e.bg || "blur", panels: e.panels } }) })
        dirty = false
        app.act("setFraming", [job.id, n, body], then ? "" : quiet ? "Saved the framing of clip " + n : "Saved. It's used the next time this clip is cut.", then)
    }
    function saveAndCut() {
        var n = clipN
        if (dirty || !app.framedByHand(n)) save(true, function () { app.cutClips([n]) })
        else app.cutClips([n])
    }
    function automatic() {
        var n = clipN
        app.act("setFraming", [job.id, n, ""], "Clip " + n + " is framed automatically again", function () { scr.seededKey = ""; scr.seed() })
    }
    function nextClip() {
        for (var i = 0; i < order.length - 1; i++) if (order[i].n === clipN) { openClip(order[i + 1].n); return }
    }
    function rel(t) { var d = Math.round(Math.max(0, t - (info ? info.start : 0))); return d > 0 ? app.fmtDur(d) : "0:00" }
    readonly property string bgValue: edit ? edit.bg || "blur" : "blur"
    readonly property string frameUrl: scene && scene.frame ? app.pic(scene.frame) : ""

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ── the clips
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 240
            color: scr.app.cSurface
            ListView {
                anchors.fill: parent; anchors.margins: 8
                clip: true
                spacing: 4
                model: scr.order
                boundsBehavior: Flickable.StopAtBounds
                header: Text { text: "CLIPS"; color: scr.app.cFaint; font.pixelSize: 11; font.letterSpacing: 1.2; textFormat: Text.PlainText; height: 24 }
                delegate: Rectangle {
                    id: cRow
                    required property var modelData
                    readonly property bool on: modelData.n === scr.clipN
                    readonly property bool ticked: scr.app.chosen.indexOf(modelData.n) >= 0
                    width: ListView.view.width
                    height: 50
                    radius: 8
                    color: on ? scr.app.cRaised : cM.containsMouse ? Qt.darker(scr.app.cRaised, 1.15) : "transparent"
                    border.width: on ? 1 : 0; border.color: scr.app.cAccent
                    opacity: ticked ? 1 : 0.6
                    MouseArea { id: cM; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: scr.openClip(cRow.modelData.n) }
                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 8
                        spacing: 2
                        Text { text: cRow.modelData.n + "  " + cRow.modelData.hook; color: scr.app.cText; font.pixelSize: 12; font.bold: cRow.on; elide: Text.ElideRight; Layout.fillWidth: true; textFormat: Text.PlainText }
                        Text {
                            text: (cRow.ticked ? "" : "not ticked · ") + (scr.app.framedByHand(cRow.modelData.n) ? "framed by hand" : "automatic") + " · " + Math.round(cRow.modelData.seconds) + " s"
                            color: scr.app.framedByHand(cRow.modelData.n) ? scr.app.cAccent : scr.app.cFaint; font.pixelSize: 11; textFormat: Text.PlainText
                        }
                    }
                }
            }
        }
        Rectangle { Layout.fillHeight: true; width: 1; color: scr.app.cLine }

        // ── the editor
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 16
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Text {
                    Layout.fillWidth: true
                    text: scr.clipRow ? "Clip " + scr.clipN + ":  " + scr.clipRow.hook : "Pick a clip on the left"
                    color: scr.app.cText; font.pixelSize: 16; font.bold: true; elide: Text.ElideRight; textFormat: Text.PlainText
                }
                Text { visible: scr.dirty; text: "not saved"; color: scr.app.cWord; font.pixelSize: 12; textFormat: Text.PlainText }
            }
            Flow {
                Layout.fillWidth: true
                spacing: 8
                Link { app: scr.app; height: 30; verticalAlignment: Text.AlignVCenter; visible: scr.app.framedByHand(scr.clipN); text: "Back to automatic"; onClicked: scr.automatic() }
                ComboBox {
                    width: 140; height: 30
                    model: scr.app.ratios.map(function (r) { return r.label })
                    currentIndex: scr.app.ratioIndex(scr.app.jobRatio.value)
                    onActivated: function (i) { scr.app.act("setJob", [scr.job.id, JSON.stringify({ ratio: scr.app.ratios[i].value })]) }
                    ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "The reels' shape, for the whole video"
                }
                SmallButton { app: scr.app; primary: true; text: "Save"; enabled: scr.edits.length > 0 && scr.dirty; onClicked: scr.save(false) }
                SmallButton {
                    app: scr.app
                    text: "Save and cut this clip"
                    tip: "Cut only this clip again; the other reels stay as they are"
                    enabled: scr.edits.length > 0 && !scr.app.jobRunning
                    onClicked: scr.saveAndCut()
                }
                SmallButton {
                    app: scr.app
                    text: "Save, next clip"
                    enabled: scr.edits.length > 0 && scr.order.length > 1 && scr.order[scr.order.length - 1].n !== scr.clipN
                    onClicked: { if (scr.dirty) scr.save(true); scr.nextClip() }
                }
            }

            // Before the scenes are there.
            ColumnLayout {
                visible: !scr.info
                Layout.fillWidth: true; Layout.fillHeight: true
                spacing: 10
                Item { Layout.fillHeight: true }
                Rectangle { Layout.alignment: Qt.AlignHCenter; visible: scr.app.scenes.state === "running"; width: 12; height: 12; radius: 6; color: scr.app.cAccent; opacity: scr.app.pulse ? 1 : 0.2 }
                Text {
                    Layout.alignment: Qt.AlignHCenter; Layout.maximumWidth: 560
                    horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                    color: scr.app.scenes.state === "failed" ? scr.app.cBad : scr.app.cSub; font.pixelSize: 14
                    text: scr.app.scenes.state === "running" ? "Finding the scenes of the clips" + (scr.app.scenes.n ? " (" + (scr.app.scenes.i || 0) + " of " + scr.app.scenes.n + " clips)" : "") + ": where the picture changes, a frame of each, the faces in it."
                        : scr.app.scenes.state === "failed" ? (scr.app.scenes.error || "Could not find the scenes.")
                        : !scr.app.clips.length ? "No clips yet."
                        : "This clip's scenes aren't there yet."
                }
                LogosButton {
                    visible: scr.app.scenes.state !== "running" && scr.app.clips.length > 0
                    Layout.alignment: Qt.AlignHCenter
                    text: "Find the scenes"
                    onClicked: scr.app.act("findScenes", [scr.job.id])
                }
                Item { Layout.fillHeight: true }
            }

            // the tools
            Flow {
                visible: !!scr.info
                Layout.fillWidth: true
                spacing: 8
                Text { text: "Quick layouts"; height: 30; verticalAlignment: Text.AlignVCenter; color: scr.app.cSub; font.pixelSize: 12; textFormat: Text.PlainText }
                Repeater {
                    model: [
                        { k: "whole", l: "Whole picture", t: "The whole picture across the reel" },
                        { k: "one", l: "One face", t: "The biggest face, filling the reel" },
                        { k: "faces", l: "Each face", t: "Every face found, one above the other (two columns from four)" },
                        { k: "screen", l: "Face + screen", t: "A face on top, the rest of the picture (a shared screen) under it" }
                    ]
                    delegate: Rectangle {
                        id: pBtn
                        required property var modelData
                        readonly property bool can: modelData.k === "whole" || scr.faces.length > 0
                        width: pT.implicitWidth + 22; height: 30; radius: 6
                        color: pM.containsMouse && can ? scr.app.cRaised : "transparent"
                        border.width: 1; border.color: scr.app.cLine
                        opacity: can ? 1 : 0.4
                        Text { id: pT; anchors.centerIn: parent; text: pBtn.modelData.l + (pBtn.modelData.k === "faces" && scr.faces.length ? " (" + Math.min(6, scr.faces.length) + ")" : ""); color: scr.app.cText; font.pixelSize: 12; textFormat: Text.PlainText }
                        MouseArea { id: pM; anchors.fill: parent; hoverEnabled: true; enabled: pBtn.can; cursorShape: Qt.PointingHandCursor; onClicked: scr.usePreset(pBtn.modelData.k) }
                        ToolTip.visible: pM.containsMouse; ToolTip.delay: 500; ToolTip.text: modelData.t
                    }
                }
                Rectangle { width: 1; height: 30; color: scr.app.cLine }
                SmallButton { app: scr.app; text: "+ Box"; tip: "Another box: the next face, or the middle"; enabled: !!scr.edit && scr.edit.panels.length < 12; onClicked: scr.addPanel() }
                Rectangle { width: 1; height: 30; color: scr.app.cLine }
                Text { text: "Background"; height: 30; verticalAlignment: Text.AlignVCenter; color: scr.app.cSub; font.pixelSize: 12; textFormat: Text.PlainText }
                Segmented {
                    app: scr.app
                    value: /^#/.test(scr.bgValue) ? "colour" : scr.bgValue
                    options: [{ v: "blur", l: "Blur", t: "The picture itself, blurred and darker" }, { v: "black", l: "Black" }, { v: "colour", l: "Colour", t: "A colour of your own (#RRGGBB)" }]
                    onPicked: function (v) { scr.setBg(v === "colour" ? (/^#/.test(scr.bgValue) ? scr.bgValue : "#1c1c20") : v) }
                }
                Field {
                    app: scr.app
                    visible: /^#/.test(scr.bgValue)
                    width: 96; height: 32
                    maximumLength: 7
                    text: scr.bgValue
                    onEditingFinished: { var c = text.trim(); if (/^#[0-9a-fA-F]{6}$/.test(c)) scr.setBg(c.toLowerCase()); else text = scr.bgValue }
                }
            }

            // the selected box
            Flow {
                visible: !!scr.info && !!scr.edit && scr.sel < scr.edit.panels.length
                Layout.fillWidth: true
                spacing: 8
                Rectangle { width: 22; height: 22; y: 4; radius: 4; color: scr.app.panelColour(scr.sel); Text { anchors.centerIn: parent; text: scr.sel + 1; color: "#000"; font.bold: true; font.pixelSize: 12 } }
                Text { text: "Box " + (scr.sel + 1) + ":"; height: 30; verticalAlignment: Text.AlignVCenter; color: scr.app.cSub; font.pixelSize: 12; textFormat: Text.PlainText }
                SmallButton { app: scr.app; text: "Zoom out"; tip: "More of the picture in the box (faces smaller). Or the mouse wheel on the box."; onClicked: scr.zoom(scr.sel, 1.12) }
                SmallButton { app: scr.app; text: "Zoom in"; tip: "Less of the picture (faces bigger)"; onClicked: scr.zoom(scr.sel, 0.89) }
                SmallButton { app: scr.app; text: "Full width"; tip: "Across the whole reel, as tall as it is"; onClicked: { var d = scr.edit.panels[scr.sel].dst; scr.placeDst(scr.sel, [0, d[1], 1, d[3]]) } }
                SmallButton { app: scr.app; text: "Full reel"; onClicked: scr.placeDst(scr.sel, [0, 0, 1, 1]) }
                SmallButton { app: scr.app; text: "Centre"; onClicked: { var d = scr.edit.panels[scr.sel].dst; scr.placeDst(scr.sel, [(1 - d[2]) / 2, (1 - d[3]) / 2, d[2], d[3]]) } }
                SmallButton { app: scr.app; text: "To front"; enabled: !!scr.edit && scr.sel < scr.edit.panels.length - 1; onClicked: scr.toFront(scr.sel) }
                SmallButton { app: scr.app; text: "Remove"; enabled: !!scr.edit && scr.edit.panels.length > 1; onClicked: scr.removePanel() }
            }

            // the frame with its pieces, and the reel
            RowLayout {
                visible: !!scr.info
                Layout.fillWidth: true; Layout.fillHeight: true
                spacing: 18
                ColumnLayout {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    Layout.preferredWidth: 1; Layout.minimumWidth: 200
                    spacing: 4
                    Text { text: "The picture: what goes in each box"; color: scr.app.cSub; font.pixelSize: 12; textFormat: Text.PlainText }
                    Item {
                        id: stage
                        Layout.fillWidth: true; Layout.fillHeight: true
                        Image {
                            id: frameImg
                            anchors.fill: parent
                            source: scr.frameUrl
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                        }
                        Item {
                            id: pic
                            x: (frameImg.width - frameImg.paintedWidth) / 2
                            y: (frameImg.height - frameImg.paintedHeight) / 2
                            width: frameImg.paintedWidth; height: frameImg.paintedHeight
                            Rectangle { anchors.fill: parent; color: "#000"; opacity: 0.45 }
                            Repeater {
                                model: scr.faces
                                delegate: Rectangle {
                                    required property var modelData
                                    x: modelData[0] * pic.width; y: modelData[1] * pic.height
                                    width: modelData[2] * pic.width; height: modelData[3] * pic.height
                                    color: "transparent"; radius: 4
                                    border.width: 1; border.color: "#ffffff"; opacity: 0.7
                                }
                            }
                            Repeater {
                                model: scr.edit ? scr.edit.panels.length : 0
                                delegate: FrameBox {
                                    required property int index
                                    app: scr.app
                                    box: scr.edit && index < scr.edit.panels.length ? scr.edit.panels[index].src : [0, 0, 0.1, 0.1]
                                    aspectN: scr.edit && index < scr.edit.panels.length ? scr.srcAspect(scr.edit.panels[index].dst) : 1
                                    pw: pic.width; ph: pic.height
                                    faces: scr.faces
                                    label: String(index + 1)
                                    tint: scr.app.panelColour(index)
                                    selected: index === scr.sel
                                    src: scr.frameUrl
                                    onPicked: scr.sel = index
                                    onMoved: function (nb) { scr.moveSrc(index, nb) }
                                    onZoomed: function (f) { scr.zoom(index, f) }
                                }
                            }
                        }
                    }
                }
                ColumnLayout {
                    Layout.fillHeight: true
                    spacing: 4
                    Text { text: "The reel, as it will look: drag a box anywhere, a corner to any size"; color: scr.app.cSub; font.pixelSize: 12; textFormat: Text.PlainText; Layout.maximumWidth: reelArea.width; elide: Text.ElideRight }
                    Item {
                        id: reelArea
                        Layout.fillHeight: true
                        Layout.preferredWidth: Math.min(height * scr.ow / scr.oh, scr.width * 0.4)
                        Rectangle {
                            id: canvas
                            readonly property real r: scr.ow / scr.oh
                            width: Math.min(reelArea.width, reelArea.height * r)
                            height: width / r
                            color: /^#/.test(scr.bgValue) ? scr.bgValue : "#000"
                            clip: true
                            border.width: 1; border.color: scr.app.cLine
                            // Blur: the picture, filling the reel, darker (reelcut blurs it too).
                            Image {
                                visible: scr.bgValue === "blur"
                                anchors.fill: parent
                                source: scr.frameUrl
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                opacity: 0.35
                                sourceSize.width: 64
                                smooth: true
                            }
                            Repeater {
                                model: scr.edit ? scr.edit.panels.length : 0
                                delegate: PanelBox {
                                    required property int index
                                    readonly property var p: scr.edit && index < scr.edit.panels.length ? scr.edit.panels[index] : { src: [0, 0, 1, 1], dst: [0, 0, 0.1, 0.1] }
                                    dst: p.dst
                                    srcBox: p.src
                                    src: scr.frameUrl
                                    cw: canvas.width; ch: canvas.height
                                    others: scr.edit ? scr.edit.panels.filter(function (q, i) { return i !== index }).map(function (q) { return q.dst }) : []
                                    label: String(index + 1)
                                    tint: scr.app.panelColour(index)
                                    selected: index === scr.sel
                                    onPicked: scr.sel = index
                                    onMoved: function (nd) { scr.moveDst(index, nd) }
                                    onZoomed: function (f) { scr.zoom(index, f) }
                                }
                            }
                            // Where the captions go (reelcut's default: lower with one box, the middle with more).
                            Rectangle {
                                readonly property int margin: scr.app.st.settings && scr.app.st.settings.captionMargin >= 0 ? scr.app.st.settings.captionMargin
                                                              : (scr.edit && scr.edit.panels.length > 1 ? 860 : 470)
                                x: 0; width: parent.width; height: parent.height * 110 / 1920
                                y: parent.height * (1 - (margin + 110) / 1920)
                                color: "transparent"; border.width: 1; border.color: "#80ffffff"
                                z: 5
                                Text { anchors.centerIn: parent; text: "captions"; color: "#c0ffffff"; font.pixelSize: 11; textFormat: Text.PlainText }
                            }
                        }
                    }
                }
            }

            // the scenes of the clip
            RowLayout {
                visible: !!scr.info
                Layout.fillWidth: true
                spacing: 10
                Flickable {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 78
                    contentWidth: sceneRow.implicitWidth
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    Row {
                        id: sceneRow
                        spacing: 8
                        Repeater {
                            model: scr.sceneList
                            delegate: Rectangle {
                                id: sc
                                required property var modelData
                                required property int index
                                width: 118; height: 78; radius: 6
                                color: scr.app.cBg
                                border.width: index === scr.idx ? 2 : 1
                                border.color: index === scr.idx ? scr.app.cAccent : scr.app.cLine
                                Image {
                                    x: 3; y: 3; width: parent.width - 6; height: 54
                                    source: sc.modelData.frame ? scr.app.pic(sc.modelData.frame) : ""
                                    fillMode: Image.PreserveAspectFit; asynchronous: true
                                    sourceSize.width: 224
                                }
                                Text {
                                    x: 6; y: 59
                                    text: "Scene " + (sc.index + 1) + " · " + scr.rel(sc.modelData.start)
                                    color: scr.app.cSub; font.pixelSize: 10; textFormat: Text.PlainText
                                }
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { scr.idx = sc.index; scr.sel = 0 } }
                            }
                        }
                    }
                }
                ColumnLayout {
                    spacing: 6
                    SmallButton { app: scr.app; Layout.preferredWidth: 180; text: "Like the scene before"; enabled: scr.idx > 0; onClicked: scr.likeBefore() }
                    SmallButton { app: scr.app; Layout.preferredWidth: 180; text: "Use for the next scenes"; enabled: scr.idx < scr.sceneList.length - 1; onClicked: scr.toFollowing() }
                }
            }
            Text {
                visible: !!scr.info
                Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                color: scr.app.cFaint; font.pixelSize: 12
                text: (scr.sceneList.length > 1 ? "Scene " + (scr.idx + 1) + " of " + scr.sceneList.length + ", from " + scr.rel(scr.scene ? scr.scene.start : 0) + ". " : "One scene. ")
                    + "Any number of boxes, anywhere, any shape: + Box adds one. On the reel, drag a box to place it and a corner to size it; on the picture, choose what it shows. The mouse wheel on a box zooms. Boxes snap to faces (white), edges, halves and thirds; a later box covers an earlier one."
            }
        }
    }
}
