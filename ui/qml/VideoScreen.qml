import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls

// Screen 1: the video coming in: downloading, transcribing, telling the speakers apart (side by
// side), with what each is doing; what failed; the log.
Flickable {
    id: scr
    property var app
    readonly property var job: app.job
    contentWidth: width
    contentHeight: col.implicitHeight + 48
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {}

    // The core says what runs as "A · B · C".
    readonly property var parts: job && job.activity && (job.status === "running" || job.status === "queued") ? String(job.activity).split(" · ") : []

    ColumnLayout {
        id: col
        x: 24; y: 20
        width: scr.width - 48
        spacing: 16

        Card {
            app: scr.app
            visible: !!scr.job && (scr.job.status === "running" || scr.job.status === "queued") && scr.app.stepIndex(scr.job.step) === 0
            Text { text: scr.job && scr.job.status === "queued" ? "Waiting for its turn" : "Working on the video"; color: scr.app.cText; font.pixelSize: 16; font.bold: true; textFormat: Text.PlainText }
            Repeater {
                model: scr.parts
                delegate: RowLayout {
                    required property string modelData
                    Layout.fillWidth: true
                    spacing: 10
                    Rectangle { width: 9; height: 9; radius: 5; color: scr.app.cAccent; opacity: scr.app.pulse ? 1 : 0.2 }
                    Text { text: modelData; color: scr.app.cText; font.pixelSize: 14; textFormat: Text.PlainText; Layout.fillWidth: true }
                }
            }
            // How far transcribing is, when it says.
            Rectangle {
                visible: !!scr.job && !!scr.job.progress && scr.job.progress.n > 0
                Layout.fillWidth: true
                height: 6; radius: 3; color: scr.app.cRaised
                Rectangle {
                    height: parent.height; radius: 3; color: scr.app.cAccent
                    width: scr.job && scr.job.progress && scr.job.progress.n ? parent.width * Math.min(1, scr.job.progress.i / scr.job.progress.n) : 0
                }
            }
            Hint { app: scr.app; text: "Transcribing runs on this computer; nothing leaves it. The clips can be picked once the transcript is done."; Layout.fillWidth: true; color: scr.app.cFaint }
        }

        // what went wrong
        Card {
            app: scr.app
            visible: !!scr.job && !!scr.job.error && scr.job.status !== "running"
            edge: scr.job && scr.job.status === "failed" ? scr.app.cBad : scr.app.cLine
            color: scr.job && scr.job.status === "failed" ? Qt.darker(scr.app.cBad, 2.6) : scr.app.cSurface
            Text { text: scr.job ? scr.job.error : ""; color: scr.app.cText; font.pixelSize: 13; wrapMode: Text.WordWrap; Layout.fillWidth: true; textFormat: Text.PlainText }
            RowLayout {
                visible: !!scr.job && (scr.job.status === "failed" || scr.job.status === "cancelled")
                LogosButton { text: "Retry"; onClicked: scr.app.act("retryJob", [scr.job.id]) }
            }
        }

        // done
        Card {
            app: scr.app
            visible: !!scr.job && scr.app.stepIndex(scr.job.step) > 0
            Text { text: "The video is in"; color: scr.app.cText; font.pixelSize: 16; font.bold: true; textFormat: Text.PlainText }
            Hint {
                app: scr.app
                Layout.fillWidth: true
                text: scr.job ? [scr.job.channel, scr.app.fmtDur(scr.job.duration), scr.job.words ? scr.job.words + " words transcribed" : "",
                                 scr.app.speakers && scr.app.speakers.found >= 2 ? scr.app.speakers.found + " voices told apart" : ""].filter(function (x) { return !!x }).join("  ·  ") : ""
            }
            RowLayout {
                spacing: 18
                Link { app: scr.app; text: "Open the transcript"; onClicked: scr.app.act("openPath", [scr.job.dir + "/transcript-ts.txt"]) }
                Link { app: scr.app; text: "Open the folder"; onClicked: scr.app.act("openPath", [scr.job.dir]) }
            }
        }

        // the log, for when something looks stuck
        RowLayout {
            visible: scr.app.jobLog.length > 0
            Layout.fillWidth: true
            Link { app: scr.app; text: logBox.visible ? "Hide the details" : "Show the details (the log)"; onClicked: logBox.visible = !logBox.visible }
        }
        Rectangle {
            id: logBox
            visible: false
            Layout.fillWidth: true
            implicitHeight: logText.implicitHeight + 20
            radius: 8; color: scr.app.cSurface; border.width: 1; border.color: scr.app.cLine
            Text {
                id: logText
                x: 10; y: 10; width: parent.width - 20
                // Only while it shows: the log changes with every line a step prints.
                text: logBox.visible ? scr.app.jobLog.slice(-40).join("\n") : ""
                color: scr.app.cSub; font.pixelSize: 11; font.family: "monospace"; wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText
            }
        }
    }
}
