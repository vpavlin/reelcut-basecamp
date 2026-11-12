import QtQuick

// A text that acts as a link: accent on hover, a pointing hand.
Text {
    id: l
    property var app
    property bool strong: false
    signal clicked()
    color: lm.containsMouse ? app.cAccent : strong ? app.cAccent : app.cSub
    font.pixelSize: 12
    textFormat: Text.PlainText
    MouseArea { id: lm; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: l.clicked() }
}
