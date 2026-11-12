import QtQuick

// A piece of the video's picture (framing): drag to move, a corner to resize. It keeps the shape of
// where it goes on the reel (aspectN: width/height in fractions of the picture), and snaps to a
// face's middle and to the picture's edges. Positions are fractions of the picture.
Rectangle {
    id: fb
    property var app
    property var box: [0, 0, 1, 1]
    property real aspectN: 1
    property bool free: false      // corners give any shape
    property real pw: 1
    property real ph: 1
    property var faces: []
    property string label: ""
    property color tint: "#ff7a3d"
    property bool selected: false
    property string src: ""
    signal moved(var nb)
    signal picked()
    signal zoomed(real f)   // the wheel: < 1 zooms in (less of the picture), > 1 out
    x: box[0] * pw; y: box[1] * ph; width: box[2] * pw; height: box[3] * ph
    color: "transparent"
    border.width: selected ? 3 : 2; border.color: tint
    z: selected ? 2 : 1
    // Its part of the picture at full brightness, over the dimmed rest.
    Item {
        anchors.fill: parent
        clip: true
        z: -1
        Image { source: fb.src; asynchronous: true; fillMode: Image.Stretch; x: -fb.x; y: -fb.y; width: fb.pw; height: fb.ph }
    }
    function clamp(v, a, b) { return Math.max(a, Math.min(b, v)) }
    function snapX(x, w) {
        var c = x + w / 2
        for (var i = 0; i < fb.faces.length; i++) {
            var f = fb.faces[i], fc = f[0] + f[2] / 2
            if (Math.abs(fc - c) < 0.015) return clamp(fc - w / 2, 0, 1 - w)
        }
        if (Math.abs(x) < 0.012) return 0
        if (Math.abs(x + w - 1) < 0.012) return 1 - w
        if (Math.abs(c - 0.5) < 0.012) return 0.5 - w / 2
        return x
    }
    function snapY(y, h) {
        if (Math.abs(y) < 0.012) return 0
        if (Math.abs(y + h - 1) < 0.012) return 1 - h
        return y
    }
    Rectangle {
        visible: fb.label !== ""
        x: 3; y: 3; width: Math.max(20, lbl.implicitWidth + 10); height: 20; radius: 4; color: fb.tint
        Text { id: lbl; anchors.centerIn: parent; text: fb.label; color: "#000"; font.pixelSize: 12; font.bold: true; textFormat: Text.PlainText }
    }
    // One area for both: pressed near a corner it resizes (the opposite corner stays), elsewhere it moves.
    MouseArea {
        id: fbArea
        anchors.fill: parent; anchors.margins: -9
        hoverEnabled: true
        property real sx: 0
        property real sy: 0
        property var sb: null
        property int corner: -1     // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right; -1 moving
        function cornerAt(x, y) {
            var bx = x - 9, by = y - 9, r = 12
            var l = Math.abs(bx) < r, rt = Math.abs(bx - fb.width) < r, t = Math.abs(by) < r, b = Math.abs(by - fb.height) < r
            return t && l ? 0 : t && rt ? 1 : b && l ? 2 : b && rt ? 3 : -1
        }
        cursorShape: {
            var c = pressed ? corner : cornerAt(mouseX, mouseY)
            return c === 0 || c === 3 ? Qt.SizeFDiagCursor : c === 1 || c === 2 ? Qt.SizeBDiagCursor : pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        }
        onPressed: function (m) {
            var p = mapToItem(fb.parent, m.x, m.y)
            sx = p.x; sy = p.y; sb = fb.box.slice(); corner = cornerAt(m.x, m.y)
            fb.picked()
        }
        onReleased: function (m) { sb = null; corner = -1 }
        onWheel: function (w) { fb.picked(); if (w.angleDelta.y !== 0) fb.zoomed(w.angleDelta.y > 0 ? 0.92 : 1.08) }
        onPositionChanged: function (m) {
            if (!sb || !pressed) return
            var p = mapToItem(fb.parent, m.x, m.y)
            if (corner < 0) {
                var w = sb[2], h = sb[3]
                var nx = fb.clamp(sb[0] + (p.x - sx) / fb.pw, 0, 1 - w)
                var ny = fb.clamp(sb[1] + (p.y - sy) / fb.ph, 0, 1 - h)
                fb.moved([fb.snapX(nx, w), fb.snapY(ny, h), w, h])
                return
            }
            var left = corner === 0 || corner === 2, top = corner < 2
            var mx = p.x / fb.pw, my = p.y / fb.ph
            var ax = left ? sb[0] + sb[2] : sb[0], ay = top ? sb[1] + sb[3] : sb[1]
            if (fb.free) {
                var fx = fb.clamp(mx, 0, 1), fy = fb.clamp(my, 0, 1)
                var x0 = Math.min(ax, fx), x1 = Math.max(ax, fx), y0 = Math.min(ay, fy), y1 = Math.max(ay, fy)
                if (x1 - x0 < 0.03) { if (left) x0 = x1 - 0.03; else x1 = x0 + 0.03 }
                if (y1 - y0 < 0.03) { if (top) y0 = y1 - 0.03; else y1 = y0 + 0.03 }
                fb.moved([x0, y0, x1 - x0, y1 - y0])
                return
            }
            var hh = Math.max(Math.abs(my - ay), Math.abs(mx - ax) / fb.aspectN, 0.04)
            var maxW = left ? ax : 1 - ax, maxH = top ? ay : 1 - ay
            hh = Math.min(hh, maxH, maxW / fb.aspectN)
            var ww = hh * fb.aspectN
            fb.moved([left ? ax - ww : ax, top ? ay - hh : ay, ww, hh])
        }
    }
    Repeater {
        model: fb.selected ? 4 : 0
        delegate: Rectangle {
            required property int index
            width: 14; height: 14; radius: 3; color: fb.tint
            x: index % 2 === 0 ? -7 : fb.width - 7
            y: index < 2 ? -7 : fb.height - 7
        }
    }
}
