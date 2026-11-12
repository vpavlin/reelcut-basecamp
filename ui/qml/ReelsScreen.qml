import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls

// Screen 5: cutting and the reels. What the cut uses (names to fix, shape, the automatic layout,
// the cards), the reels, then YouTube texts, a forum, Logos Storage.
Flickable {
    id: scr
    property var app
    readonly property var job: app.job
    readonly property var reels: app.reels
    readonly property bool cutting: !!job && (job.step === "cut" || job.step === "thumbs") && (job.status === "running" || job.status === "queued")
    contentWidth: width
    contentHeight: col.implicitHeight + 48
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {}

    // The forum target, and the threads read for its forum (if any).
    readonly property var forumT: job && job.forum && job.forum.target ? job.forum.target : ({})
    readonly property string forumApp: forumT.app || ""
    readonly property string forumName: forumT.forum || (forumApp === "forumer" ? "public" : forumApp ? "Logos Forum" : "")
    readonly property var forumList: forumApp && app.st.forumThreads ? (app.st.forumThreads[forumApp + "|" + forumName] || null) : null
    readonly property int forumPosted: reels.filter(function (r) { return !!r.forumPost }).length
    function setForum(obj) { app.act("setForum", [job.id, JSON.stringify(obj)]) }
    readonly property bool anyYoutube: reels.some(function (r) { return !!r.youtube })
    readonly property bool ytAsking: !!job && !!job.youtube && job.youtube.state === "asking"
    // Logos Storage: on in Settings, how many reels are not there yet, what failed.
    readonly property bool storeOn: !!app.st.settings && !!app.st.settings.storeTo
    readonly property int storeMissing: reels.filter(function (r) { return !r.cid }).length
    readonly property bool storeFailed: reels.some(function (r) { return r.storing && r.storing !== "uploading" }) || (!!job && !!job.storage && /^failed/.test(job.storage.setStoring || ""))
    readonly property string storeStatus: {
        if (!storeOn || !reels.length) return ""
        var up = reels.filter(function (r) { return r.storing === "uploading" }).length
        if (up) return "Putting " + up + (up === 1 ? " reel" : " reels") + " in Logos Storage…"
        if (storeFailed) return (job.storage && /^failed: /.test(job.storage.setStoring || "") ? job.storage.setStoring.replace(/^failed: /, "") : "Some reels didn't make it to Logos Storage.")
        if (job.storage && job.storage.setStoring === "uploading") return "Putting the set in Logos Storage…"
        if (job.storage && job.storage.set) return "In Logos Storage"
        return ""
    }
    function cardIndex(file) {
        for (var i = 0; i < app.cards.length; i++) if (app.cards[i].file === file) return i + 1
        return 0
    }
    function cardThumb(file) {
        for (var i = 0; i < app.cards.length; i++) if (app.cards[i].file === file) return app.cards[i].thumb
        return ""
    }
    function cut() { app.act("cutReels", [job.id, fixField.text], "", function () { app.recut = [] }) }

    ColumnLayout {
        id: col
        x: 24; y: 20
        width: scr.width - 48
        spacing: 16

        // cutting now
        Card {
            app: scr.app
            visible: scr.cutting
            RowLayout {
                spacing: 10
                Rectangle { width: 9; height: 9; radius: 5; color: scr.app.cAccent; opacity: scr.app.pulse ? 1 : 0.2 }
                Text {
                    text: scr.job && scr.job.status === "queued" ? "Waiting for its turn"
                        : scr.job && scr.job.progress && scr.job.progress.n ? "Cutting reel " + Math.min(scr.job.progress.i + 1, scr.job.progress.n) + " of " + scr.job.progress.n
                        : scr.job && scr.job.activity ? scr.job.activity : "Cutting"
                    color: scr.app.cText; font.pixelSize: 16; font.bold: true; textFormat: Text.PlainText
                }
                Item { Layout.fillWidth: true }
                LogosButton { text: "Stop"; onClicked: scr.app.act("cancelJob", [scr.job.id]) }
            }
            Rectangle {
                visible: !!scr.job && !!scr.job.progress && scr.job.progress.n > 0
                Layout.fillWidth: true
                height: 6; radius: 3; color: scr.app.cRaised
                Rectangle {
                    height: parent.height; radius: 3; color: scr.app.cAccent
                    width: scr.job && scr.job.progress && scr.job.progress.n ? parent.width * Math.min(1, scr.job.progress.i / scr.job.progress.n) : 0
                }
            }
            Hint { app: scr.app; color: scr.app.cFaint; Layout.fillWidth: true; text: "At a low priority, so the computer stays usable; each reel shows up here when it's done." }
        }
        Card {
            app: scr.app
            visible: !!scr.job && !!scr.job.error && (scr.job.step === "cut" || scr.job.step === "thumbs") && scr.job.status !== "running"
            edge: scr.app.cBad
            color: Qt.darker(scr.app.cBad, 2.6)
            Text { text: scr.job ? scr.job.error || "" : ""; color: scr.app.cText; font.pixelSize: 13; wrapMode: Text.WordWrap; Layout.fillWidth: true; textFormat: Text.PlainText }
            LogosButton { text: "Retry"; onClicked: scr.app.act("retryJob", [scr.job.id]) }
        }

        // what the cut uses
        Card {
            app: scr.app
            visible: scr.app.clips.length > 0
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Text {
                    Layout.fillWidth: true
                    text: scr.app.recut.length ? "Framed by hand since the last cut: clip " + scr.app.recut.join(", ") + ". Cut again to see it."
                        : scr.app.chosen.length ? "Cut the " + scr.app.chosen.length + " ticked clips" + (scr.app.framedCount ? ", " + scr.app.chosen.filter(function (n) { return scr.app.framedByHand(n) }).length + " framed by hand" : "")
                        : "No clips ticked: all " + scr.app.clips.length + " are cut"
                    color: scr.app.recut.length ? scr.app.cWord : scr.app.cText; font.pixelSize: 14; font.bold: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                }
                LogosButton {
                    text: scr.reels.length ? "Cut again" : "Cut"
                    enabled: !scr.app.jobRunning
                    onClicked: scr.cut()
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    Hint { app: scr.app; text: "Misheard names in the captions:  Heard=Meant, one or more" }
                    Field {
                        id: fixField
                        app: scr.app
                        Layout.fillWidth: true
                        Layout.preferredHeight: 34
                        placeholderText: "Sterling=Sterlin, Meakin=Mencken"
                        readonly property string seed: scr.job ? scr.job.id + "\n" + (scr.job.fixes || "") : ""
                        onSeedChanged: text = scr.job ? scr.job.fixes || "" : ""
                    }
                }
                ColumnLayout {
                    spacing: 4
                    Hint { app: scr.app; text: "Shape" }
                    ComboBox {
                        Layout.preferredWidth: 150; Layout.preferredHeight: 34
                        model: scr.app.ratios.map(function (r) { return r.label })
                        readonly property string seed: scr.job ? scr.job.ratio || "9:16" : "9:16"
                        onSeedChanged: currentIndex = scr.app.ratioIndex(seed)
                        Component.onCompleted: currentIndex = scr.app.ratioIndex(seed)
                        onActivated: scr.app.act("setJob", [scr.job.id, JSON.stringify({ ratio: scr.app.ratios[currentIndex].value })])
                    }
                }
                ColumnLayout {
                    spacing: 4
                    Hint { app: scr.app; text: "Clips not framed by hand" }
                    ComboBox {
                        Layout.preferredWidth: 250; Layout.preferredHeight: 34
                        model: scr.app.layouts.map(function (l) { return l.label })
                        readonly property string seed: scr.job ? scr.job.layout || "" : ""
                        onSeedChanged: currentIndex = scr.app.layoutIndex(seed)
                        Component.onCompleted: currentIndex = scr.app.layoutIndex(seed)
                        onActivated: scr.app.act("setSpeakers", [scr.job.id, JSON.stringify({ layout: scr.app.layoutValue(currentIndex) }), false])
                    }
                }
            }
            // the opening and closing cards (a λ, a QR code, a URL)
            RowLayout {
                Layout.fillWidth: true
                spacing: 14
                Repeater {
                    model: [{ key: "intro", label: "Opening card" }, { key: "outro", label: "Closing card" }]
                    delegate: RowLayout {
                        id: cardRow
                        required property var modelData
                        spacing: 8
                        readonly property string file: scr.job ? scr.job[modelData.key] || "" : ""
                        Rectangle {
                            width: 34; height: 34; radius: 4; color: "#000"; clip: true
                            border.width: 1; border.color: scr.app.cLine
                            Image { anchors.fill: parent; anchors.margins: 1; fillMode: Image.PreserveAspectFit; asynchronous: true; source: scr.cardThumb(cardRow.file) ? scr.app.fileUrl(scr.cardThumb(cardRow.file)) : "" }
                        }
                        ColumnLayout {
                            spacing: 2
                            Hint { app: scr.app; text: cardRow.modelData.label }
                            ComboBox {
                                Layout.preferredWidth: 220
                                Layout.preferredHeight: 32
                                model: ["None"].concat(scr.app.cards.map(function (c) { return c.name })).concat(["Add an image…"])
                                readonly property string seed: cardRow.file + "|" + scr.app.cards.length
                                onSeedChanged: currentIndex = scr.cardIndex(cardRow.file)
                                Component.onCompleted: currentIndex = scr.cardIndex(cardRow.file)
                                onActivated: function (i) {
                                    var key = cardRow.modelData.key
                                    if (i === count - 1) { currentIndex = scr.cardIndex(cardRow.file); scr.app.openCardDialog(key); return }
                                    var o = {}; o[key] = i === 0 ? "" : scr.app.cards[i - 1].file
                                    scr.app.act("setJob", [scr.job.id, JSON.stringify(o)])
                                }
                            }
                        }
                    }
                }
                Item { Layout.fillWidth: true }
            }
        }

        // the reels
        RowLayout {
            visible: scr.reels.length > 0
            Layout.fillWidth: true
            spacing: 10
            Text { text: "REELS"; color: scr.app.cFaint; font.pixelSize: 11; font.letterSpacing: 1.2; textFormat: Text.PlainText; Layout.fillWidth: true }
            Text {
                visible: scr.storeStatus !== ""
                text: scr.storeStatus
                color: scr.storeFailed ? scr.app.cBad : scr.app.cSub; font.pixelSize: 12; textFormat: Text.PlainText
                Layout.maximumWidth: 360; elide: Text.ElideRight
            }
            LogosButton { visible: !!scr.job && !!scr.job.storage && !!scr.job.storage.set; text: "Copy the set's CID"; onClicked: scr.app.copyText(scr.job.storage.set) }
            LogosButton {
                visible: scr.storeOn && !!scr.job && scr.job.status === "done" && (scr.storeMissing > 0 || scr.storeFailed)
                text: scr.storeFailed ? "Try Storage again" : "Put in Storage"
                onClicked: scr.app.act("storeReels", [scr.job.id], "Putting the reels in Logos Storage")
            }
            LogosButton { visible: !!scr.job && !!scr.job.page; text: "Watch in the browser"; onClicked: scr.app.act("openPath", [scr.job.page]) }
        }
        Flow {
            visible: scr.reels.length > 0
            Layout.fillWidth: true
            spacing: 14
            Repeater {
                model: scr.reels
                delegate: Rectangle {
                    id: reel
                    required property var modelData
                    readonly property real thumbH: 300
                    width: Math.round(thumbH * scr.app.jobRatio.w / scr.app.jobRatio.h)
                    height: thumbH + 84 + (scr.storeOn ? 22 : 0) + (scr.anyYoutube ? 22 : 0)
                    radius: 10
                    color: scr.app.cSurface
                    border.width: 1; border.color: rMouse.containsMouse ? scr.app.cAccent : scr.app.cLine
                    clip: true
                    Rectangle {
                        id: thumbBox
                        x: 0; y: 0; width: parent.width; height: reel.thumbH
                        color: "#000"
                        Image { anchors.fill: parent; source: reel.modelData.thumb ? scr.app.fileUrl(reel.modelData.thumb) : ""; fillMode: Image.PreserveAspectFit; asynchronous: true; cache: false }
                        Rectangle {
                            anchors.centerIn: parent
                            width: 52; height: 52; radius: 26
                            color: Qt.rgba(0, 0, 0, rMouse.containsMouse ? 0.7 : 0.45)
                            Text { anchors.centerIn: parent; anchors.horizontalCenterOffset: 2; text: "▶"; color: "#fff"; font.pixelSize: 20; textFormat: Text.PlainText }
                        }
                        Rectangle {
                            anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 8
                            visible: reel.modelData.seconds > 0
                            width: durT.implicitWidth + 10; height: 18; radius: 4
                            color: Qt.rgba(0, 0, 0, 0.7)
                            Text { id: durT; anchors.centerIn: parent; text: Math.round(reel.modelData.seconds) + " s"; color: "#fff"; font.pixelSize: 11; textFormat: Text.PlainText }
                        }
                    }
                    // The browser plays them reliably (Basecamp has no video player yet).
                    MouseArea {
                        id: rMouse; anchors.fill: thumbBox; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: scr.app.act("openPath", [scr.job.page ? scr.job.page : reel.modelData.file])
                    }
                    // Frame it by hand: a clear button on the picture.
                    Rectangle {
                        visible: reel.modelData.n > 0
                        anchors.left: parent.left; anchors.top: parent.top; anchors.margins: 8
                        width: rfT.implicitWidth + 18; height: 26; radius: 13
                        color: rfMouse.containsMouse ? scr.app.cAccent : Qt.rgba(0, 0, 0, 0.75)
                        border.width: 1; border.color: scr.app.cAccent
                        Text {
                            id: rfT; anchors.centerIn: parent
                            text: scr.app.framedByHand(reel.modelData.n) ? "Framed ✓" : "Frame"
                            color: rfMouse.containsMouse ? "#000" : "#fff"; font.pixelSize: 12; font.bold: true; textFormat: Text.PlainText
                        }
                        MouseArea { id: rfMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: scr.app.reframe(reel.modelData.n) }
                    }
                    Text {
                        x: 10; y: reel.thumbH + 8; width: parent.width - 20
                        text: reel.modelData.name; color: scr.app.cText; font.pixelSize: 13; font.bold: true
                        wrapMode: Text.WordWrap; maximumLineCount: 2; elide: Text.ElideRight; textFormat: Text.PlainText
                    }
                    Row {
                        visible: scr.anyYoutube
                        x: 10; y: parent.height - 46 - (scr.storeOn ? 22 : 0)
                        spacing: 10
                        Text { text: "YouTube:"; color: scr.app.cFaint; font.pixelSize: 11; textFormat: Text.PlainText; visible: !!reel.modelData.youtube }
                        Text { text: "not written"; color: scr.app.cFaint; font.pixelSize: 11; textFormat: Text.PlainText; visible: !reel.modelData.youtube }
                        Repeater {
                            model: reel.modelData.youtube ? [
                                { label: "Title", text: reel.modelData.youtube.title || "" },
                                { label: "Text", text: reel.modelData.youtube.description || "" },
                                { label: "Tags", text: (reel.modelData.youtube.tags || []).join(", ") }
                            ] : []
                            delegate: Link {
                                required property var modelData
                                app: scr.app
                                text: modelData.label
                                font.pixelSize: 11
                                onClicked: if (modelData.text) scr.app.copyText(modelData.text)
                            }
                        }
                    }
                    Text {
                        visible: scr.storeOn
                        x: 10; y: parent.height - 46; width: parent.width - 20
                        text: reel.modelData.cid ? "CID " + reel.modelData.cid
                            : reel.modelData.storing === "uploading" ? "Putting in Storage…"
                            : reel.modelData.storing ? reel.modelData.storing.replace(/^failed: /, "Not in Storage: ") : ""
                        color: reel.modelData.storing && reel.modelData.storing !== "uploading" ? scr.app.cBad : cidMouse.containsMouse ? scr.app.cAccent : scr.app.cFaint
                        font.pixelSize: 11; elide: Text.ElideMiddle; textFormat: Text.PlainText
                        MouseArea { id: cidMouse; anchors.fill: parent; enabled: !!reel.modelData.cid; hoverEnabled: true; cursorShape: reel.modelData.cid ? Qt.PointingHandCursor : Qt.ArrowCursor; onClicked: scr.app.copyText(reel.modelData.cid) }
                    }
                    Link {
                        x: 10; y: parent.height - 24
                        app: scr.app
                        text: "Open in the video player"
                        font.pixelSize: 11
                        onClicked: scr.app.act("openPath", [reel.modelData.file])
                    }
                }
            }
        }
        Hint {
            visible: scr.reels.length === 0 && !scr.cutting
            app: scr.app
            Layout.fillWidth: true
            text: scr.app.clips.length ? "No reels yet. Tick clips on the Clips screen, frame them if you like, then cut." : "No clips yet."
        }

        // YouTube texts for the reels (title, description, tags each): an agent writes them, or any AI chat.
        Rectangle {
            visible: !!scr.job && scr.reels.length > 0 && scr.job.status === "done"
            Layout.fillWidth: true
            implicitHeight: ytCol.implicitHeight + 32
            radius: 10
            color: scr.app.cSurface
            border.width: 1; border.color: scr.app.cLine
            ColumnLayout {
                id: ytCol
                anchors.fill: parent; anchors.margins: 16
                spacing: 10
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Text { text: "YouTube titles and descriptions"; color: scr.app.cText; font.pixelSize: 15; font.bold: true; textFormat: Text.PlainText; Layout.fillWidth: true }
                    Text {
                        visible: scr.ytAsking
                        text: "Asking " + (scr.job && scr.job.youtube ? scr.job.youtube.who : "") + "…"
                        color: scr.app.cAccent; font.pixelSize: 12; textFormat: Text.PlainText
                    }
                    LogosButton { visible: scr.ytAsking; text: "Stop"; onClicked: scr.app.act("stopYoutube", [scr.job.id]) }
                    ComboBox {
                        id: ytAgentBox
                        readonly property var opts: scr.app.pickOpts
                        visible: opts.length > 0 && !scr.ytAsking
                        Layout.preferredWidth: 260
                        model: opts.map(function (o) { return o.label })
                        currentIndex: {
                            var want = scr.job && scr.job.agent ? scr.job.agent : (scr.app.st.settings ? scr.app.st.settings.agent : "")
                            for (var i = 0; i < opts.length; i++) if (opts[i].value === want) return i
                            return opts.length ? 0 : -1
                        }
                    }
                    LogosButton {
                        visible: ytAgentBox.visible
                        text: scr.anyYoutube ? "Write them again" : "Write them"
                        enabled: ytAgentBox.currentIndex >= 0
                        onClicked: scr.app.act("writeYoutube", [scr.job.id, ytAgentBox.opts[ytAgentBox.currentIndex].value])
                    }
                }
                Text {
                    Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                    color: scr.app.cSub; font.pixelSize: 13
                    text: "A keyword-minded title, a description and tags for each reel, from what is said in it. "
                        + (scr.anyYoutube ? "Copy them from each reel: Title, Text, Tags." : (ytAgentBox.visible ? "Ask an agent, or copy the prompt into any AI chat and paste its answer below." : "Copy the prompt into any AI chat, such as Claude or ChatGPT, and paste its answer below."))
                }
                Text {
                    visible: !!scr.job && !!scr.job.youtube && !!scr.job.youtube.error
                    Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                    color: scr.app.cBad; font.pixelSize: 12
                    text: scr.job && scr.job.youtube ? scr.job.youtube.error : ""
                }
                RowLayout {
                    spacing: 10
                    LogosButton { text: "Copy the prompt"; onClicked: scr.app.copyYoutubePrompt() }
                    Text { text: "then paste the answer:"; color: scr.app.cFaint; font.pixelSize: 12; textFormat: Text.PlainText }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 90
                    radius: 8; color: scr.app.cBg
                    border.width: 1; border.color: ytAnswer.activeFocus ? scr.app.cAccent : scr.app.cLine
                    ScrollView {
                        anchors.fill: parent; anchors.margins: 6
                        TextArea {
                            id: ytAnswer
                            placeholderText: "Paste the answer here (=== REEL 1 === …)"
                            placeholderTextColor: scr.app.cFaint
                            color: scr.app.cText; font.pixelSize: 12; font.family: "monospace"
                            wrapMode: TextEdit.Wrap; selectByMouse: true
                            background: null
                        }
                    }
                }
                LogosButton {
                    Layout.alignment: Qt.AlignRight
                    text: "Use this answer"
                    enabled: ytAnswer.text.trim() !== ""
                    onClicked: scr.app.act("setYoutube", [scr.job.id, ytAnswer.text], "Saved the YouTube texts", function () { ytAnswer.text = "" })
                }
            }
        }

        // The reels as replies in a forum thread: Forumer or Logos Forum.
        Rectangle {
            visible: !!scr.job && scr.reels.length > 0 && scr.job.status === "done"
            Layout.fillWidth: true
            implicitHeight: forumCol.implicitHeight + 32
            radius: 10
            color: scr.app.cSurface
            border.width: 1; border.color: scr.app.cLine
            ColumnLayout {
                id: forumCol
                anchors.fill: parent; anchors.margins: 16
                spacing: 10
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Text { text: "Post the reels to a forum"; color: scr.app.cText; font.pixelSize: 15; font.bold: true; textFormat: Text.PlainText; Layout.fillWidth: true }
                    ComboBox {
                        id: forumAppBox
                        Layout.preferredWidth: 160
                        model: ["Choose…", "Forumer", "Logos Forum"]
                        currentIndex: scr.forumApp === "forumer" ? 1 : scr.forumApp === "logos_forum" ? 2 : 0
                        onActivated: (i) => { if (i > 0) scr.setForum({ app: i === 1 ? "forumer" : "logos_forum" }) }
                    }
                    Field { app: scr.app;
                        id: forumNameField
                        visible: !!scr.forumApp
                        Layout.preferredWidth: 160; Layout.preferredHeight: 34
                        readonly property string seed: scr.job ? scr.job.id + "|" + scr.forumName : ""
                        onSeedChanged: text = scr.forumName
                        placeholderText: scr.forumApp === "forumer" ? "public" : "Logos Forum"
                        onEditingFinished: if (text.trim() !== scr.forumName) scr.setForum({ forum: text.trim() })
                        ToolTip.visible: hovered; ToolTip.delay: 500
                        ToolTip.text: scr.forumApp === "forumer" ? "The forum: \"public\" is Forumer's open forum." : "The forum's name: \"Logos Forum\" is the app's own."
                    }
                }
                Text {
                    visible: !scr.forumApp
                    Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                    color: scr.app.cSub; font.pixelSize: 13
                    text: "Each reel as a reply in a thread (say, the post with the full video): its YouTube title and description, and where to get it from Logos Storage. Reelcut signs the posts with its own key."
                }
                // the thread
                ColumnLayout {
                    visible: !!scr.forumApp
                    Layout.fillWidth: true
                    spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Text {
                            Layout.fillWidth: true; elide: Text.ElideRight; textFormat: Text.PlainText; font.pixelSize: 13
                            color: scr.forumT.thread ? scr.app.cText : scr.app.cFaint
                            text: scr.forumT.thread ? "Thread: " + (scr.forumT.threadTitle || scr.forumT.thread) : "No thread yet: pick one, paste a link, or start one."
                        }
                        LogosButton { visible: !!scr.forumT.thread; text: "Change"; onClicked: scr.setForum({ thread: "" }) }
                    }
                    ColumnLayout {
                        visible: !scr.forumT.thread
                        Layout.fillWidth: true
                        spacing: 8
                        RowLayout {
                            visible: scr.forumApp === "forumer"
                            Layout.fillWidth: true
                            spacing: 8
                            Field { app: scr.app; id: threadLinkField; Layout.fillWidth: true; Layout.preferredHeight: 34; placeholderText: "Paste a Forumer thread link (Share on the thread)" }
                            LogosButton { text: "Use it"; enabled: threadLinkField.text.trim() !== ""; onClicked: { scr.setForum({ thread: threadLinkField.text.trim() }); threadLinkField.text = "" } }
                        }
                        RowLayout {
                            spacing: 10
                            LogosButton {
                                text: scr.forumList && scr.forumList.state === "loading" ? "Reading the threads…" : "Pick a recent thread"
                                enabled: !(scr.forumList && scr.forumList.state === "loading")
                                onClicked: scr.app.act("loadForumThreads", [scr.forumApp, scr.forumName])
                            }
                            LogosButton { text: "Start a new thread"; onClicked: newThreadBox.visible = !newThreadBox.visible }
                            Text {
                                visible: !!scr.app.st.delivery && scr.app.st.delivery.state === "starting"
                                text: "Connecting to Logos Delivery…"; color: scr.app.cAccent; font.pixelSize: 12; textFormat: Text.PlainText
                            }
                        }
                        Text {
                            visible: !!scr.forumList && !!scr.forumList.error
                            Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                            color: scr.app.cFaint; font.pixelSize: 12
                            text: scr.forumList ? scr.forumList.error + " The list holds what the network's store nodes still keep" + (scr.forumApp === "forumer" ? "; an older thread: paste its link." : ".") : ""
                        }
                        // the threads read back, newest first
                        Repeater {
                            model: scr.forumList && scr.forumList.threads ? scr.forumList.threads.slice(0, 30) : []
                            delegate: Rectangle {
                                required property var modelData
                                Layout.fillWidth: true
                                implicitHeight: 34
                                radius: 6
                                color: thMouse.containsMouse ? scr.app.cRaised : "transparent"
                                RowLayout {
                                    anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8
                                    spacing: 10
                                    Text { text: modelData.title; color: scr.app.cText; font.pixelSize: 13; elide: Text.ElideRight; Layout.fillWidth: true; textFormat: Text.PlainText }
                                    Text { text: modelData.author + " · " + new Date(modelData.ts).toLocaleDateString(Qt.locale(), Locale.ShortFormat); color: scr.app.cFaint; font.pixelSize: 11; textFormat: Text.PlainText }
                                }
                                MouseArea { id: thMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: scr.setForum({ thread: modelData.id, threadTitle: modelData.title }) }
                            }
                        }
                        // a new thread: the full video's
                        ColumnLayout {
                            id: newThreadBox
                            visible: false
                            Layout.fillWidth: true
                            spacing: 6
                            Field { app: scr.app; id: ntTitle; Layout.fillWidth: true; Layout.preferredHeight: 34; placeholderText: "Title"; text: scr.job ? scr.job.title || "" : "" }
                            Rectangle {
                                Layout.fillWidth: true; Layout.preferredHeight: 80
                                radius: 8; color: scr.app.cBg; border.width: 1; border.color: ntBody.activeFocus ? scr.app.cAccent : scr.app.cLine
                                ScrollView {
                                    anchors.fill: parent; anchors.margins: 6
                                    TextArea {
                                        id: ntBody
                                        color: scr.app.cText; font.pixelSize: 12; wrapMode: TextEdit.Wrap; selectByMouse: true; background: null
                                        placeholderText: "What the thread is about"; placeholderTextColor: scr.app.cFaint
                                        text: scr.job && scr.job.url ? "Full video: " + scr.job.url : ""
                                    }
                                }
                            }
                            Field { app: scr.app; id: ntTags; visible: scr.forumApp === "forumer"; Layout.fillWidth: true; Layout.preferredHeight: 34; placeholderText: "Topic tags, comma-separated (Forumer needs one), e.g. dev club, video" }
                            LogosButton {
                                Layout.alignment: Qt.AlignRight
                                text: "Start it and post the reels"
                                enabled: ntTitle.text.trim() !== "" && (scr.forumApp !== "forumer" || ntTags.text.trim() !== "")
                                onClicked: { scr.app.act("startForumThread", [scr.job.id, ntTitle.text.trim(), ntBody.text, ntTags.text, true]); newThreadBox.visible = false }
                            }
                        }
                    }
                }
                // posting
                RowLayout {
                    visible: !!scr.forumT.thread
                    Layout.fillWidth: true
                    spacing: 10
                    CheckBox {
                        id: forumAuto
                        checked: !!scr.forumT.auto
                        onToggled: scr.setForum({ auto: checked })
                    }
                    Text {
                        Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText; color: scr.app.cSub; font.pixelSize: 12
                        text: "Post the reels by themselves once they are in Logos Storage (with their CIDs)"
                    }
                    Text {
                        text: scr.job && scr.job.forum && scr.job.forum.state === "waiting" ? "Storing the reels first, then posting…"
                            : scr.job && scr.job.forum && scr.job.forum.state === "posting" ? "Posting… " + scr.forumPosted + " of " + scr.reels.length
                            : scr.forumPosted ? scr.forumPosted + " of " + scr.reels.length + " posted" : ""
                        color: scr.app.cAccent; font.pixelSize: 12; textFormat: Text.PlainText
                    }
                    LogosButton {
                        visible: !!scr.job && !!scr.job.forum && (scr.job.forum.state === "posting" || scr.job.forum.state === "waiting")
                        text: "Stop"; onClicked: scr.app.act("stopForum", [scr.job.id])
                    }
                    LogosButton {
                        visible: !!scr.job && (!scr.job.forum || (scr.job.forum.state !== "posting" && scr.job.forum.state !== "waiting")) && scr.forumPosted < scr.reels.length
                        text: scr.forumPosted ? "Post the rest" : "Post the reels"
                        onClicked: scr.app.act("postToForum", [scr.job.id])
                    }
                }
                Text {
                    visible: !!scr.job && !!scr.job.forum && !!scr.job.forum.error
                    Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                    color: scr.app.cBad; font.pixelSize: 12
                    text: scr.job && scr.job.forum ? scr.job.forum.error : ""
                }
            }
        }

    }
}
