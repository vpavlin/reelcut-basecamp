import QtQuick
import QtQuick.Controls

// A compact button for toolbars; primary: filled with the accent.
Rectangle {
    id: b
    property var app
    property string text: ""
    property string tip: ""
    property bool primary: false
    signal clicked()
    implicitWidth: t.implicitWidth + 24
    implicitHeight: 30
    radius: 6
    opacity: enabled ? 1 : 0.4
    color: primary ? (m.containsMouse ? Qt.lighter(app.cAccent, 1.15) : app.cAccent) : (m.containsMouse ? app.cRaised : "transparent")
    border.width: 1; border.color: primary ? app.cAccent : app.cLine
    Text { id: t; anchors.centerIn: parent; text: b.text; color: b.primary ? "#000" : b.app.cText; font.pixelSize: 12; font.bold: b.primary; textFormat: Text.PlainText }
    MouseArea { id: m; anchors.fill: parent; hoverEnabled: true; enabled: b.enabled; cursorShape: Qt.PointingHandCursor; onClicked: b.clicked() }
    ToolTip.visible: m.containsMouse && b.tip !== ""; ToolTip.delay: 500; ToolTip.text: b.tip
    Accessible.role: Accessible.Button
    Accessible.name: b.text
}
