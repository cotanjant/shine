import QtQuick
import QtQuick.Layouts

RowLayout {
    id: root
    property var model
    property int selectedIndex: 0
    signal displayChosen(int index)

    spacing: 6

    Repeater {
        model: root.model

        delegate: Rectangle {
            required property int displayIndex
            required property string label
            required property bool isPrimary

            readonly property bool isSelected: root.selectedIndex === displayIndex

            implicitWidth: pillText.implicitWidth + 18
            implicitHeight: 28
            radius: 8
            color: isSelected ? "#26314A" : (hoverArea.containsMouse ? "#1E2333" : "#151925")
            border.color: isSelected ? "#60A5FA" : "#272D40"
            border.width: 1

            Behavior on color { ColorAnimation { duration: 120 } }

            Text {
                id: pillText
                anchors.centerIn: parent
                text: label
                color: isSelected ? "#F8FAFC" : "#94A3B8"
                font.pixelSize: 11
                font.weight: isSelected ? Font.DemiBold : Font.Normal
            }

            MouseArea {
                id: hoverArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.displayChosen(displayIndex)
            }
        }
    }
}
