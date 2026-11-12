import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls

// Screen 2: who speaks. A name for each voice (shown on the reels, and telling the agent who is
// who), where each sits, the automatic layout for clips not framed by hand.
Flickable {
    id: scr
    property var app
    readonly property var job: app.job
    readonly property var spk: app.speakers || ({})
    readonly property var sideValues: ["", "left", "middle", "right"]
    contentWidth: width
    contentHeight: col.implicitHeight + 48
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {}

    // The voices ([letter]), kept as is between polls so their fields keep what is typed.
    property var letters: []
    onSpkChanged: {
        var n = (spk.found || 0) >= 2 ? Object.keys(spk.talk || {}).sort() : []
        if (JSON.stringify(n) !== JSON.stringify(letters)) letters = n
    }
    readonly property bool diarizing: !!job && job.status === "running" && (job.activity || "").indexOf("speakers apart") >= 0
    function save(proceed) {
        var names = {}, sides = {}
        for (var i = 0; i < spkRep.count; i++) {
            var it = spkRep.itemAt(i)
            if (!it) continue
            names[it.letter] = it.nameText
            if (it.side) sides[it.letter] = it.side
        }
        app.act("setSpeakers", [job.id, JSON.stringify({ names: names, sides: sides, layout: app.layoutValue(spkLayout.currentIndex) }), !!proceed],
                proceed ? "" : "Saved. Names and places apply to the next cut; ask for clips again for the agent to use them.")
    }

    ColumnLayout {
        id: col
        x: 24; y: 20
        width: scr.width - 48
        spacing: 16

        Card {
            app: scr.app
            visible: scr.letters.length === 0
            Text {
                text: scr.diarizing ? "Telling the speakers apart…" : scr.app.stepIndex(scr.job ? scr.job.step : "") === 0 ? "Not yet" : "One voice"
                color: scr.app.cText; font.pixelSize: 16; font.bold: true; textFormat: Text.PlainText
            }
            Hint {
                app: scr.app; Layout.fillWidth: true
                text: scr.diarizing ? "Beside the transcript, about 7 minutes per hour of video."
                    : scr.app.stepIndex(scr.job ? scr.job.step : "") === 0 ? "The voices are told apart while the video is transcribed."
                    : "Reelcut heard one voice (or telling them apart is off in Settings). For an interview or a call, count them:"
            }
            RowLayout {
                visible: !scr.diarizing && !!scr.job && scr.app.stepIndex(scr.job.step) > 0
                spacing: 8
                SpinBox { id: firstCount; from: 0; to: 8; value: 2; editable: true; Layout.preferredWidth: 110 }
                Hint { app: scr.app; text: "people (0: let it guess)" }
                LogosButton {
                    text: "Tell the speakers apart"
                    enabled: !scr.app.jobRunning
                    onClicked: scr.app.act("diarizeJob", [scr.job.id, firstCount.value], "Telling the speakers apart")
                }
            }
        }

        Card {
            app: scr.app
            visible: scr.letters.length > 0
            edge: scr.job && scr.job.step === "speakers" ? scr.app.cWord : scr.app.cLine
            spacing: 12
            Text { text: "Who speaks"; color: scr.app.cText; font.pixelSize: 16; font.bold: true; textFormat: Text.PlainText }
            Hint {
                app: scr.app; Layout.fillWidth: true
                text: scr.letters.length + " voices. Give each a name, as it should show on the reels, and say where they sit in the picture. The names also tell the agent who is who."
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 18
                Rectangle {
                    visible: !!scr.job && !!scr.job.frame
                    Layout.alignment: Qt.AlignTop
                    Layout.preferredWidth: 380
                    Layout.preferredHeight: scr.job && scr.job.width > 0 ? 380 * scr.job.height / scr.job.width : 214
                    radius: 6; color: "#000"; clip: true
                    Image {
                        anchors.fill: parent
                        source: scr.job && scr.job.frame ? scr.app.pic(scr.job.frame) : ""
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        cache: false
                    }
                    Text { anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 6; text: "left"; color: "#fff"; font.pixelSize: 11; style: Text.Outline; styleColor: "#000"; textFormat: Text.PlainText }
                    Text { anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.margins: 6; visible: scr.letters.length > 2; text: "middle"; color: "#fff"; font.pixelSize: 11; style: Text.Outline; styleColor: "#000"; textFormat: Text.PlainText }
                    Text { anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 6; text: "right"; color: "#fff"; font.pixelSize: 11; style: Text.Outline; styleColor: "#000"; textFormat: Text.PlainText }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    spacing: 14
                    Repeater {
                        id: spkRep
                        model: scr.letters
                        delegate: ColumnLayout {
                            id: spkRow
                            required property string modelData
                            required property int index
                            readonly property string letter: modelData
                            readonly property string nameText: nameField.text.trim()
                            readonly property string side: scr.sideValues[sideBox.currentIndex] || ""
                            // What is stored: put in the fields when it changes, not on every poll.
                            readonly property string seedName: scr.spk.names && scr.spk.names[letter] ? scr.spk.names[letter] : ""
                            readonly property string seedSide: scr.spk.sides && scr.spk.sides[letter] ? scr.spk.sides[letter] : ""
                            onSeedNameChanged: nameField.text = seedName
                            onSeedSideChanged: sideBox.currentIndex = Math.max(0, scr.sideValues.indexOf(seedSide))
                            Component.onCompleted: { nameField.text = seedName; sideBox.currentIndex = Math.max(0, scr.sideValues.indexOf(seedSide)) }
                            function setSide(i) { sideBox.currentIndex = i }
                            Layout.fillWidth: true
                            spacing: 6
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10
                                Rectangle {
                                    width: 26; height: 26; radius: 13
                                    color: scr.app.cRaised; border.width: 2; border.color: scr.app.speakerColour(spkRow.letter)
                                    Text { anchors.centerIn: parent; text: spkRow.letter; color: scr.app.speakerColour(spkRow.letter); font.pixelSize: 13; font.bold: true; textFormat: Text.PlainText }
                                }
                                Text {
                                    text: {
                                        var t = scr.spk.talk ? scr.spk.talk[spkRow.letter] || 0 : 0
                                        return t >= 60 ? Math.round(t / 60) + " min" : Math.round(t) + " s"
                                    }
                                    color: scr.app.cFaint; font.pixelSize: 12; font.features: { "tnum": 1 }; Layout.preferredWidth: 46; textFormat: Text.PlainText
                                }
                                Field { id: nameField; app: scr.app; Layout.fillWidth: true; Layout.preferredHeight: 34; placeholderText: "Name of " + spkRow.letter }
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
                            Repeater {
                                model: scr.spk.samples && scr.spk.samples[spkRow.letter] ? scr.spk.samples[spkRow.letter] : []
                                delegate: Text {
                                    required property string modelData
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 36
                                    text: (modelData.match(/^\[([^\]]*)\]/) || ["", ""])[1] + "   “" + modelData.replace(/^\[[^\]]*\] /, "") + "”"
                                    color: scr.app.cSub; font.pixelSize: 12; font.italic: true
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
                Label { text: "Clips not framed by hand"; color: scr.app.cSub; font.pixelSize: 12 }
                ComboBox {
                    id: spkLayout
                    Layout.preferredWidth: 270
                    model: scr.app.layouts.map(function (l) { return l.label })
                    readonly property string seed: scr.job ? scr.job.layout || "" : ""
                    onSeedChanged: currentIndex = scr.app.layoutIndex(seed)
                    Component.onCompleted: currentIndex = scr.app.layoutIndex(seed)
                }
                Item { Layout.fillWidth: true }
                LogosButton { text: "Save"; visible: !!scr.job && scr.job.step !== "speakers"; onClicked: scr.save(false) }
                LogosButton { visible: !!scr.job && scr.job.step === "speakers"; text: "Go on: pick the clips"; onClicked: scr.save(true) }
            }
            // The wrong count merges two people, or splits one.
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Hint { app: scr.app; text: "Not " + scr.letters.length + " people? Count again with"; color: scr.app.cFaint }
                SpinBox { id: spkCount; from: 1; to: 8; value: Math.max(1, scr.letters.length); editable: true; Layout.preferredWidth: 110 }
                LogosButton {
                    text: "Count again"
                    enabled: !scr.app.jobRunning
                    onClicked: scr.app.act("diarizeJob", [scr.job.id, spkCount.value], "Telling the speakers apart again")
                }
                Item { Layout.fillWidth: true }
            }
        }
    }
}
