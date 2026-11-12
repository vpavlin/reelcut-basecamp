import QtQuick

// Where a piece goes on the reel (framing): its part of the frame, drawn where it will be. Drag to
// move, a corner to resize to any shape; it snaps to the reel's edges, halves and thirds, and to the
// other pieces' edges. Positions are fractions of the reel.
Item {
    id: pb
    property var dst: [0, 0, 1, 1]
    property var srcBox: [0, 0, 1, 1]   // the part of the frame it shows (fractions of the frame)
    property string src: ""
    property real cw: 1                  // the reel's size on screen
    property real ch: 1
    property var others: []              // the other pieces' dst boxes, to snap to
    property string label: ""
    property color tint: "#ff7a3d"
    property bool selected: false
    property bool editable: true
    signal moved(var nd)
    signal picked()
    signal zoomed(real f)   // the wheel: < 1 zooms in (less of the picture), > 1 out
    x: dst[0] * cw; y: dst[1] * ch; width: dst[2] * cw; height: dst[3] * ch
    z: selected ? 2 : 1

    Item {
        anchors.fill: parent
        clip: true
        Image {
            source: pb.src
            asynchronous: true
            fillMode: Image.Stretch
            width: pb.width / Math.max(0.01, pb.srcBox[2])
            height: pb.height / Math.max(0.01, pb.srcBox[3])
            x: -pb.srcBox[0] * width
            y: -pb.srcBox[1] * height
        }
    }
    Rectangle {
        anchors.fill: parent
        visible: pb.editable
        color: "transparent"
        border.width: pb.selected ? 3 : 1
        border.color: pb.tint
        opacity: pb.selected ? 1 : 0.8
    }
    Rectangle {
        visible: pb.editable && pb.label !== ""
        x: 3; y: 3; width: Math.max(20, lbl.implicitWidth + 10); height: 20; radius: 4; color: pb.tint
        Text { id: lbl; anchors.centerIn: parent; text: pb.label; color: "#000"; font.pixelSize: 12; font.bold: true; textFormat: Text.PlainText }
    }

    function clamp(v, a, b) { return Math.max(a, Math.min(b, v)) }
    // The lines to snap to, across (xs) and down (ys).
    function lines(vertical) {
        var l = [0, 1 / 3, 0.5, 2 / 3, 1]
        for (var i = 0; i < pb.others.length; i++) {
            var o = pb.others[i]
            if (vertical) l.push(o[0], o[0] + o[2]); else l.push(o[1], o[1] + o[3])
        }
        return l
    }
    function snap(v, ls, tol) {
        for (var i = 0; i < ls.length; i++) if (Math.abs(v - ls[i]) < tol) return ls[i]
        return null
    }
    MouseArea {
        enabled: pb.editable
        anchors.fill: parent; anchors.margins: -8
        hoverEnabled: true
        property real sx: 0
        property real sy: 0
        property var sb: null
        property int corner: -1
        function cornerAt(x, y) {
            var bx = x - 8, by = y - 8, r = 12
            var l = Math.abs(bx) < r, rt = Math.abs(bx - pb.width) < r, t = Math.abs(by) < r, b = Math.abs(by - pb.height) < r
            return t && l ? 0 : t && rt ? 1 : b && l ? 2 : b && rt ? 3 : -1
        }
        cursorShape: {
            var c = pressed ? corner : cornerAt(mouseX, mouseY)
            return c === 0 || c === 3 ? Qt.SizeFDiagCursor : c === 1 || c === 2 ? Qt.SizeBDiagCursor : pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        }
        onPressed: function (m) {
            var p = mapToItem(pb.parent, m.x, m.y)
            sx = p.x; sy = p.y; sb = pb.dst.slice(); corner = cornerAt(m.x, m.y)
            pb.picked()
        }
        onReleased: { sb = null; corner = -1 }
        onWheel: function (w) { pb.picked(); if (w.angleDelta.y !== 0) pb.zoomed(w.angleDelta.y > 0 ? 0.92 : 1.08) }
        onPositionChanged: function (m) {
            if (!sb || !pressed) return
            var p = mapToItem(pb.parent, m.x, m.y)
            var tx = 8 / pb.cw, ty = 8 / pb.ch
            var xs = pb.lines(true), ys = pb.lines(false)
            if (corner < 0) {
                var w = sb[2], h = sb[3]
                var nx = pb.clamp(sb[0] + (p.x - sx) / pb.cw, 0, 1 - w)
                var ny = pb.clamp(sb[1] + (p.y - sy) / pb.ch, 0, 1 - h)
                var a = pb.snap(nx, xs, tx), b = pb.snap(nx + w, xs, tx), c = pb.snap(nx + w / 2, xs, tx)
                nx = a !== null ? a : b !== null ? b - w : c !== null ? c - w / 2 : nx
                var d = pb.snap(ny, ys, ty), e = pb.snap(ny + h, ys, ty)
                ny = d !== null ? d : e !== null ? e - h : ny
                pb.moved([pb.clamp(nx, 0, 1 - w), pb.clamp(ny, 0, 1 - h), w, h])
                return
            }
            var left = corner === 0 || corner === 2, top = corner < 2
            var mx = pb.clamp(p.x / pb.cw, 0, 1), my = pb.clamp(p.y / pb.ch, 0, 1)
            var sxv = pb.snap(mx, xs, tx), syv = pb.snap(my, ys, ty)
            if (sxv !== null) mx = sxv
            if (syv !== null) my = syv
            var ax = left ? sb[0] + sb[2] : sb[0], ay = top ? sb[1] + sb[3] : sb[1]
            var x0 = Math.min(ax, mx), x1 = Math.max(ax, mx), y0 = Math.min(ay, my), y1 = Math.max(ay, my)
            if (x1 - x0 < 0.05) { if (left) x0 = x1 - 0.05; else x1 = x0 + 0.05 }
            if (y1 - y0 < 0.03) { if (top) y0 = y1 - 0.03; else y1 = y0 + 0.03 }
            pb.moved([x0, y0, x1 - x0, y1 - y0])
        }
    }
    Repeater {
        model: pb.selected && pb.editable ? 4 : 0
        delegate: Rectangle {
            required property int index
            width: 14; height: 14; radius: 3; color: pb.tint
            x: index % 2 === 0 ? -7 : pb.width - 7
            y: index < 2 ? -7 : pb.height - 7
        }
    }
}
