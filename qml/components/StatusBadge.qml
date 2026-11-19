import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root
    property bool gpuActive: false
    property string gpuText: "GPU"
    property string memoryText: "-- MB"

    implicitWidth: row.implicitWidth + 16
    implicitHeight: 26
    radius: 13
    color: "#161A26"
    border.color: "#272D40"
    border.width: 1

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 6

        Rectangle {
            width: 7
            height: 7
            radius: 3.5
            color: root.gpuActive ? "#22C55E" : "#F59E0B"
        }

        Text {
            text: root.gpuText
            color: "#CBD5E1"
            font.pixelSize: 11
            font.weight: Font.Medium
        }

        Rectangle {
            width: 1
            height: 10
            color: "#2E354A"
        }

        Text {
            text: root.memoryText
            color: "#94A3B8"
            font.pixelSize: 11
        }
    }
}
