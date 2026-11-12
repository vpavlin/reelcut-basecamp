import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls

// Screen 3: the clips. Who picks them (an agent, or any AI chat), what to look for, the list to
// tick, each clip's words; then on to framing or straight to cutting.
Item {
    id: scr
    property var app
    readonly property var job: app.job
    readonly property bool picking: !!job && job.step === "pick" && job.status === "running"

    // A tick changes the list for everyone at once: the chosen numbers.
    function tick(n, on) {
        var c = app.chosen.filter(function (x) { return x !== n })
        if (on) c.push(n)
        c.sort(function (a, b) { return a - b })
        app.act("setJob", [job.id, JSON.stringify({ chosen: c })])
    }
    function tickAll(on) { app.act("setJob", [job.id, JSON.stringify({ chosen: on ? app.clips.map(function (c) { return c.n }) : [] })]) }
    function ask() {
        var who = agentBox.opts[agentBox.currentIndex].value, id = job.id
        app.setBrief(id, briefJob.text.trim(), function () { app.act("pickClips", [id, who]) })
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: col.implicitHeight + 40
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            ColumnLayout {
                id: col
                x: 24; y: 20
                width: flick.width - 48
                spacing: 14

                // who picks, and what to look for
                Card {
                    app: scr.app
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Text { text: scr.picking ? "Asking " + scr.app.pickerLabel(scr.job.agent) + "…" : "Pick the moments"; color: scr.app.cText; font.pixelSize: 16; font.bold: true; textFormat: Text.PlainText; Layout.fillWidth: true }
                        Rectangle { visible: scr.picking; width: 9; height: 9; radius: 5; color: scr.app.cAccent; opacity: scr.app.pulse ? 1 : 0.2 }
                        ComboBox {
                            id: agentBox
                            readonly property var opts: scr.app.pickOpts
                            visible: opts.length > 0 && !scr.picking
                            Layout.preferredWidth: 320
                            model: opts.map(function (o) { return o.label })
                            currentIndex: {
                                var want = scr.job && scr.job.agent ? scr.job.agent : (scr.app.st.settings ? scr.app.st.settings.agent : "")
                                for (var i = 0; i < opts.length; i++) if (opts[i].value === want) return i
                                return opts.length ? 0 : -1
                            }
                        }
                        LogosButton {
                            visible: agentBox.visible
                            text: scr.app.clips.length ? "Ask again" : "Ask"
                            enabled: !!scr.job && !scr.app.jobRunning && agentBox.currentIndex >= 0 && scr.app.stepIndex(scr.job.step) > 0
                            onClicked: scr.ask()
                        }
                        LogosButton { visible: scr.picking; text: "Stop"; onClicked: scr.app.act("cancelJob", [scr.job.id]) }
                    }
                    Hint { app: scr.app; text: "What I'm looking for: topics, people, tone, keywords. It counts more than the general advice."; Layout.fillWidth: true }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 56
                        radius: 8; color: scr.app.cBg
                        border.width: 1; border.color: briefJob.activeFocus ? scr.app.cAccent : scr.app.cLine
                        ScrollView {
                            anchors.fill: parent; anchors.margins: 4
                            TextArea {
                                id: briefJob
                                placeholderText: "e.g. privacy and coercion, sharp one-liners from Corey, nothing about the roadmap"
                                placeholderTextColor: scr.app.cFaint
                                color: scr.app.cText; font.pixelSize: 12; wrapMode: TextEdit.Wrap; selectByMouse: true
                                background: null
                                readonly property string seed: scr.job ? scr.job.id + "\n" + (scr.job.brief || "") : ""
                                onSeedChanged: text = scr.job ? scr.job.brief || "" : ""
                                onActiveFocusChanged: if (!activeFocus && scr.job && text.trim() !== (scr.job.brief || "")) scr.app.setBrief(scr.job.id, text.trim())
                            }
                        }
                    }
                    // Or by hand: any AI chat.
                    RowLayout {
                        visible: !scr.picking
                        spacing: 16
                        Link { app: scr.app; text: pasteBox.visible ? "Hide" : "Or use any AI chat: copy the prompt, paste the answer"; onClicked: pasteBox.visible = !pasteBox.visible }
                        Link { app: scr.app; text: "Open the transcript"; onClicked: scr.app.act("openPath", [scr.job.dir + "/transcript-ts.txt"]) }
                        Link { app: scr.app; visible: scr.app.clips.length > 0; text: listBox.visible ? "Hide the list" : "Edit the list as text"; onClicked: { listBox.visible = !listBox.visible; if (listBox.visible) clipsEdit.text = scr.job.clipsText } }
                    }
                    ColumnLayout {
                        id: pasteBox
                        visible: scr.app.clips.length === 0 && scr.app.pickOpts.length === 0
                        Layout.fillWidth: true
                        spacing: 8
                        RowLayout {
                            spacing: 10
                            LogosButton { text: "Copy the prompt"; onClicked: scr.app.setBrief(scr.job.id, briefJob.text.trim(), scr.app.copyPrompt) }
                            Hint { app: scr.app; text: "Paste it into Claude, ChatGPT or any chat, then its answer here:" }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 110
                            radius: 8; color: scr.app.cBg
                            border.width: 1; border.color: answerEdit.activeFocus ? scr.app.cAccent : scr.app.cLine
                            ScrollView {
                                anchors.fill: parent; anchors.margins: 6
                                TextArea {
                                    id: answerEdit
                                    placeholderText: "Paste the answer here"
                                    placeholderTextColor: scr.app.cFaint
                                    color: scr.app.cText; font.pixelSize: 12; font.family: "monospace"
                                    wrapMode: TextEdit.Wrap; selectByMouse: true
                                    background: null
                                }
                            }
                        }
                        LogosButton {
                            Layout.alignment: Qt.AlignRight
                            text: "Use these clips"
                            enabled: answerEdit.text.trim() !== ""
                            onClicked: scr.app.act("setClips", [scr.job.id, answerEdit.text], "", function () { answerEdit.text = ""; pasteBox.visible = false })
                        }
                    }
                    ColumnLayout {
                        id: listBox
                        visible: false
                        Layout.fillWidth: true
                        spacing: 6
                        Hint { app: scr.app; text: "One clip per line:  N | START H:MM:SS | END H:MM:SS | speaker | hook | until LASTWORD"; font.family: "monospace" }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 150
                            radius: 8; color: scr.app.cBg
                            border.width: 1; border.color: clipsEdit.activeFocus ? scr.app.cAccent : scr.app.cLine
                            ScrollView {
                                anchors.fill: parent; anchors.margins: 6
                                TextArea { id: clipsEdit; color: scr.app.cText; font.pixelSize: 12; font.family: "monospace"; wrapMode: TextEdit.NoWrap; selectByMouse: true; background: null }
                            }
                        }
                        LogosButton { Layout.alignment: Qt.AlignRight; text: "Save the list"; onClicked: scr.app.act("setClips", [scr.job.id, clipsEdit.text], "Saved") }
                    }
                    Text {
                        visible: !!scr.job && scr.app.clips.length === 0 && !!scr.job.pickReply && !scr.picking
                        text: scr.job ? scr.job.pickReply || "" : ""
                        color: scr.app.cSub; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true; textFormat: Text.PlainText
                    }
                    Text {
                        visible: !!scr.job && !!scr.job.error && scr.job.step === "clips"
                        text: scr.job ? scr.job.error || "" : ""
                        color: scr.app.cBad; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true; textFormat: Text.PlainText
                    }
                }

                // the clips, best first: tick the ones to cut
                RowLayout {
                    visible: scr.app.clips.length > 0
                    Layout.fillWidth: true
                    spacing: 14
                    Text { Layout.fillWidth: true; text: scr.app.chosen.length + " of " + scr.app.clips.length + " ticked"; color: scr.app.cSub; font.pixelSize: 13; textFormat: Text.PlainText }
                    Link { app: scr.app; text: "Tick all"; onClicked: scr.tickAll(true) }
                    Link { app: scr.app; text: "None"; onClicked: scr.tickAll(false) }
                }
                Repeater {
                    model: scr.app.clips
                    delegate: Rectangle {
                        id: cand
                        required property var modelData
                        readonly property bool picked: scr.app.chosen.indexOf(modelData.n) >= 0
                        readonly property bool byHand: scr.app.framedByHand(modelData.n)
                        property bool open: false
                        property string words: ""
                        Layout.fillWidth: true
                        implicitHeight: candCol.implicitHeight + 16
                        radius: 8
                        color: picked ? scr.app.cRaised : "transparent"
                        border.width: 1; border.color: picked ? scr.app.cAccent : scr.app.cLine
                        ColumnLayout {
                            id: candCol
                            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                            anchors.margins: 8
                            spacing: 4
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10
                                CheckBox {
                                    checked: cand.picked
                                    onToggled: { scr.tick(cand.modelData.n, checked); checked = Qt.binding(function () { return cand.picked }) }
                                }
                                Text { text: cand.modelData.n; color: scr.app.cFaint; font.pixelSize: 13; Layout.preferredWidth: 18; textFormat: Text.PlainText }
                                Text { text: cand.modelData.hook; color: scr.app.cText; font.pixelSize: 14; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true; textFormat: Text.PlainText }
                                Text { text: cand.modelData.speaker; color: scr.app.cSub; font.pixelSize: 12; elide: Text.ElideRight; Layout.maximumWidth: 140; textFormat: Text.PlainText }
                                Text { text: Math.round(cand.modelData.seconds) + " s"; color: cand.modelData.seconds > 60 ? scr.app.cBad : scr.app.cSub; font.pixelSize: 12; font.features: { "tnum": 1 }; Layout.preferredWidth: 40; horizontalAlignment: Text.AlignRight; textFormat: Text.PlainText }
                                Text { text: cand.modelData.start + " – " + cand.modelData.end; color: scr.app.cFaint; font.pixelSize: 12; font.features: { "tnum": 1 }; Layout.preferredWidth: 130; horizontalAlignment: Text.AlignRight; textFormat: Text.PlainText }
                                // Frame it: a real button, not a faint link.
                                Rectangle {
                                    Layout.preferredWidth: frT.implicitWidth + 22; Layout.preferredHeight: 28; radius: 14
                                    color: frM.containsMouse ? scr.app.cAccent : cand.byHand ? Qt.darker(scr.app.cAccent, 2.2) : scr.app.cRaised
                                    border.width: 1; border.color: scr.app.cAccent
                                    Text { id: frT; anchors.centerIn: parent; text: cand.byHand ? "Framed ✓" : "Frame"; color: frM.containsMouse ? "#000" : scr.app.cText; font.pixelSize: 12; font.bold: true; textFormat: Text.PlainText }
                                    MouseArea { id: frM; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: scr.app.reframe(cand.modelData.n) }
                                }
                            }
                            Text {
                                visible: !!cand.modelData.summary
                                Layout.fillWidth: true; Layout.leftMargin: 66
                                text: cand.modelData.summary || ""
                                color: scr.app.cSub; font.pixelSize: 12; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                            }
                            Link {
                                Layout.leftMargin: 66
                                app: scr.app
                                text: cand.open ? "Hide the words" : "Show the words"
                                font.pixelSize: 11
                                onClicked: {
                                    if (cand.open) { cand.open = false; return }
                                    scr.app.callVia(scr.app.coreName, "clipText", [scr.job.id, cand.modelData.n], function (raw) {
                                        var o = scr.app.parse(raw)
                                        cand.words = o && o.ok ? (o.text || "(no lines in that range)") : "Could not read the transcript."
                                        cand.open = true
                                    })
                                }
                            }
                            Text {
                                visible: cand.open
                                Layout.fillWidth: true; Layout.leftMargin: 66
                                text: cand.words
                                color: scr.app.cText; font.pixelSize: 12; font.family: "monospace"; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                            }
                        }
                    }
                }
            }
        }

        // what next
        Rectangle {
            visible: scr.app.clips.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: 60
            color: scr.app.cSurface
            Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: scr.app.cLine }
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 24; anchors.rightMargin: 24
                spacing: 12
                Hint {
                    app: scr.app
                    Layout.fillWidth: true
                    text: scr.app.chosen.length === 0 ? "Tick the clips to make reels of."
                        : scr.app.scenes.state === "running" ? "Finding the scenes of the clips meanwhile, for framing."
                        : "Frame them by hand, or let Reelcut frame them and cut."
                }
                LogosButton {
                    text: "Frame the ticked clips"
                    enabled: scr.app.chosen.length > 0
                    onClicked: scr.app.reframe(scr.app.chosen[0])
                }
                LogosButton {
                    text: "Cut " + (scr.app.chosen.length || "") + " now"
                    enabled: scr.app.chosen.length > 0 && !scr.app.jobRunning
                    onClicked: scr.app.act("cutReels", [scr.job.id, scr.job.fixes || ""], "", function () { scr.app.recut = []; scr.app.show(4) })
                }
            }
        }
    }
}
