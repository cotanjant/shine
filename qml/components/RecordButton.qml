import QtQuick

Item {
    id: root
    property bool recording: false
    property bool busy: false
    signal clicked()

    implicitWidth: 38
    implicitHeight: 38

    Rectangle {
        id: pulseHalo
        anchors.centerIn: parent
        width: 38
        height: 38
        radius: 19
        color: "#40FF4D5E"
        visible: root.recording
        opacity: 0.45

        SequentialAnimation on opacity {
            running: root.recording
            loops: Animation.Infinite
            NumberAnimation { from: 0.2; to: 0.65; duration: 750; easing.type: Easing.InOutSine }
            NumberAnimation { from: 0.65; to: 0.2; duration: 750; easing.type: Easing.InOutSine }
        }
    }

    Rectangle {
        anchors.centerIn: parent
        width: 32
        height: 32
        radius: 16
        color: mouseArea.containsMouse ? "#23283A" : "#181C29"
        border.color: root.recording ? "#FF4D5E" : "#353C54"
        border.width: 1

        Behavior on color { ColorAnimation { duration: 120 } }
        Behavior on border.color { ColorAnimation { duration: 160 } }

        Rectangle {
            id: innerShape
            anchors.centerIn: parent
            width: root.recording ? 12 : 16
            height: root.recording ? 12 : 16
            radius: root.recording ? 3 : 8
            color: root.busy ? "#94A3B8" : "#FF4D5E"

            Behavior on width { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
            Behavior on height { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
            Behavior on radius { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
        }
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        enabled: !root.busy
        onClicked: root.clicked()
    }
}
