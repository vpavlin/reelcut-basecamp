import QtQuick
import QtQuick.Layouts

// A raised panel holding a column of content.
Rectangle {
    id: c
    property var app
    property color edge: app.cLine
    default property alias content: col.data
    property alias spacing: col.spacing
    Layout.fillWidth: true
    implicitHeight: col.implicitHeight + 32
    radius: 10
    color: app.cSurface
    border.width: 1
    border.color: edge
    ColumnLayout {
        id: col
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        anchors.margins: 16
        spacing: 10
    }
}
