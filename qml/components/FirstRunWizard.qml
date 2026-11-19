import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: root
    spacing: 14

    property string chosenHotkey: recorder.clipHotkey
    property int chosenLength: recorder.clipLengthSeconds
    property bool chosenVoice: recorder.voiceEnabled
    property bool chosenPtt: recorder.voicePttEnabled
    property string chosenPttKey: recorder.voicePttHotkey

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 4

        RowLayout {
            spacing: 8
            Rectangle {
                width: 10
                height: 10
                radius: 5
                color: "#60A5FA"
            }
            Text {
                text: "Welcome to Shine"
                color: "#F8FAFC"
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }
        }

        Text {
            Layout.fillWidth: true
            text: "Choose your clip hotkey and buffer length. Shine runs silently offline and saves the last moments whenever you press it."
            color: "#94A3B8"
            font.pixelSize: 11.5
            wrapMode: Text.WordWrap
        }
    }

    Rectangle {
        Layout.fillWidth: true
        height: 1
        color: "#22283A"
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 10

        Text {
            text: "Clip Hotkey"
            color: "#CBD5E1"
            font.pixelSize: 12
            Layout.preferredWidth: 88
        }

        HotkeyRecorder {
            binding: root.chosenHotkey
            onHotkeyChosen: function(newBinding) {
                root.chosenHotkey = newBinding
            }
        }

        Text {
            text: "Click pill & press key"
            color: "#64748B"
            font.pixelSize: 11
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Text {
            text: "Clip Length"
            color: "#CBD5E1"
            font.pixelSize: 12
            Layout.preferredWidth: 88
        }

        Repeater {
            model: [15, 30, 60, 120]
            delegate: Rectangle {
                required property int modelData
                readonly property bool active: root.chosenLength === modelData
                width: 48
                height: 28
                radius: 8
                color: active ? "#26314A" : "#151925"
                border.color: active ? "#60A5FA" : "#272D40"
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: modelData + "s"
                    color: active ? "#F8FAFC" : "#94A3B8"
                    font.pixelSize: 11
                    font.weight: active ? Font.DemiBold : Font.Normal
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.chosenLength = modelData
                }
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 10

        Text {
            text: "Microphone"
            color: "#CBD5E1"
            font.pixelSize: 12
            Layout.preferredWidth: 88
        }

        Rectangle {
            implicitWidth: micRow.implicitWidth + 16
            height: 28
            radius: 8
            color: root.chosenVoice ? "#1A2B26" : "#151925"
            border.color: root.chosenVoice ? "#22C55E" : "#272D40"
            border.width: 1

            RowLayout {
                id: micRow
                anchors.centerIn: parent
                spacing: 6
                Rectangle {
                    width: 8
                    height: 8
                    radius: 4
                    color: root.chosenVoice ? "#22C55E" : "#475569"
                }
                Text {
                    text: root.chosenVoice ? "Voice On" : "Voice Off"
                    color: "#CBD5E1"
                    font.pixelSize: 11
                }
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: root.chosenVoice = !root.chosenVoice
            }
        }

        Rectangle {
            visible: root.chosenVoice
            implicitWidth: pttRow.implicitWidth + 14
            height: 28
            radius: 8
            color: root.chosenPtt ? "#1B2938" : "#151925"
            border.color: root.chosenPtt ? "#38BDF8" : "#272D40"
            border.width: 1

            RowLayout {
                id: pttRow
                anchors.centerIn: parent
                spacing: 6
                Rectangle {
                    width: 7
                    height: 7
                    radius: 3.5
                    color: root.chosenPtt ? "#38BDF8" : "#475569"
                }
                Text {
                    text: "Push-to-Talk"
                    color: "#CBD5E1"
                    font.pixelSize: 11
                }
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: root.chosenPtt = !root.chosenPtt
            }
        }

        HotkeyRecorder {
            visible: root.chosenVoice && root.chosenPtt
            binding: root.chosenPttKey
            onHotkeyChosen: function(newBinding) {
                root.chosenPttKey = newBinding
            }
        }
    }

    Item { Layout.fillHeight: true }

    RowLayout {
        Layout.fillWidth: true

        Item { Layout.fillWidth: true }

        Rectangle {
            implicitWidth: ctaText.implicitWidth + 32
            height: 34
            radius: 10
            color: ctaMouse.containsMouse ? "#3B82F6" : "#2563EB"

            Behavior on color { ColorAnimation { duration: 120 } }

            Text {
                id: ctaText
                anchors.centerIn: parent
                text: "Save & Start Clipping"
                color: "#FFFFFF"
                font.pixelSize: 12.5
                font.weight: Font.DemiBold
            }

            MouseArea {
                id: ctaMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    recorder.completeFirstRun(
                        root.chosenHotkey,
                        root.chosenLength,
                        root.chosenVoice,
                        root.chosenPtt,
                        root.chosenPttKey)
                }
            }
        }
    }
}
