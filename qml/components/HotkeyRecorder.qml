import QtQuick

Rectangle {
    id: root
    property string binding: "F8"
    property bool listening: false
    signal hotkeyChosen(string newBinding)

    implicitWidth: Math.max(88, label.implicitWidth + 22)
    implicitHeight: 28
    radius: 8
    color: listening ? "#2B2238" : (mouseArea.containsMouse ? "#20273B" : "#151925")
    border.color: listening ? "#A855F7" : "#2D354C"
    border.width: 1
    focus: listening

    Behavior on color { ColorAnimation { duration: 120 } }

    Text {
        id: label
        anchors.centerIn: parent
        text: root.listening ? "Press key..." : root.binding
        color: root.listening ? "#E9D5FF" : "#E2E8F0"
        font.pixelSize: 11
        font.weight: Font.DemiBold
        font.family: "Consolas"
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            root.listening = true
            root.forceActiveFocus()
        }
    }

    Keys.onPressed: function(event) {
        if (!root.listening) {
            return
        }
        event.accepted = true

        if (event.key === Qt.Key_Escape) {
            root.listening = false
            return
        }
        if (event.key === Qt.Key_Control || event.key === Qt.Key_Shift ||
            event.key === Qt.Key_Alt || event.key === Qt.Key_Meta) {
            return
        }

        var keyName = ""
        if (event.key >= Qt.Key_F1 && event.key <= Qt.Key_F12) {
            keyName = "F" + (event.key - Qt.Key_F1 + 1)
        } else if (event.key >= Qt.Key_A && event.key <= Qt.Key_Z) {
            keyName = String.fromCharCode(65 + (event.key - Qt.Key_A))
        } else if (event.key >= Qt.Key_0 && event.key <= Qt.Key_9) {
            keyName = String.fromCharCode(48 + (event.key - Qt.Key_0))
        } else if (event.key === Qt.Key_Space) {
            keyName = "Space"
        } else if (event.key === Qt.Key_Tab) {
            keyName = "Tab"
        } else if (event.key === Qt.Key_CapsLock) {
            keyName = "CapsLock"
        } else if (event.key === Qt.Key_Insert) {
            keyName = "Insert"
        } else if (event.key === Qt.Key_Delete) {
            keyName = "Delete"
        } else if (event.key === Qt.Key_Home) {
            keyName = "Home"
        } else if (event.key === Qt.Key_End) {
            keyName = "End"
        } else if (event.key === Qt.Key_PageUp) {
            keyName = "PageUp"
        } else if (event.key === Qt.Key_PageDown) {
            keyName = "PageDown"
        } else if (event.key === Qt.Key_Pause) {
            keyName = "Pause"
        } else if (event.key === Qt.Key_Print) {
            keyName = "PrintScreen"
        } else if (event.key === Qt.Key_QuoteLeft) {
            keyName = "Backquote"
        } else if (event.key === Qt.Key_Minus) {
            keyName = "Minus"
        } else if (event.key === Qt.Key_Equal) {
            keyName = "Equal"
        } else if (event.key === Qt.Key_BracketLeft) {
            keyName = "BracketLeft"
        } else if (event.key === Qt.Key_BracketRight) {
            keyName = "BracketRight"
        }

        if (keyName === "") {
            return
        }

        var parts = []
        if (event.modifiers & Qt.ControlModifier) parts.push("Ctrl")
        if (event.modifiers & Qt.AltModifier) parts.push("Alt")
        if (event.modifiers & Qt.ShiftModifier) parts.push("Shift")
        if (event.modifiers & Qt.MetaModifier) parts.push("Super")
        parts.push(keyName)

        var result = parts.join("+")
        root.listening = false
        root.binding = result
        root.hotkeyChosen(result)
    }
}
