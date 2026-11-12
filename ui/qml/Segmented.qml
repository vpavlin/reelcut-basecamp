import QtQuick
import QtQuick.Controls

// A row of choices, one on: options [{v, l, t}] (value, label, tooltip).
Row {
    id: seg
    property var app
    property var options: []
    property string value: ""
    signal picked(string v)
    spacing: 0
    Repeater {
        model: seg.options
        delegate: Rectangle {
            id: opt
            required property var modelData
            required property int index
            readonly property bool on: seg.value === modelData.v
            width: optT.implicitWidth + 28; height: 32
            radius: 0
            color: on ? seg.app.cAccent : optM.containsMouse ? seg.app.cRaised : seg.app.cSurface
            border.width: 1; border.color: on ? seg.app.cAccent : seg.app.cLine
            Text { id: optT; anchors.centerIn: parent; text: opt.modelData.l; color: opt.on ? "#000" : seg.app.cText; font.pixelSize: 13; font.bold: opt.on; textFormat: Text.PlainText }
            MouseArea { id: optM; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: seg.picked(opt.modelData.v) }
            ToolTip.visible: optM.containsMouse && !!modelData.t; ToolTip.delay: 500; ToolTip.text: modelData.t || ""
        }
    }
}
