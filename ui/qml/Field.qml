import QtQuick
import QtQuick.Controls

// A text field in Reelcut's colours.
TextField {
    property var app
    color: app ? app.cText : "#ececf1"
    placeholderTextColor: app ? app.cFaint : "#74747f"
    font.pixelSize: 14
    selectByMouse: true
    background: Rectangle {
        radius: 8
        color: app ? app.cBg : "#141416"
        border.width: 1
        border.color: parent.activeFocus ? (app ? app.cAccent : "#ff7a3d") : (app ? app.cLine : "#2e2e36")
    }
}
