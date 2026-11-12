import QtQuick

// Small secondary text.
Text {
    property var app
    color: app ? app.cSub : "#a8a8b3"
    font.pixelSize: 12
    wrapMode: Text.WordWrap
    textFormat: Text.PlainText
}
