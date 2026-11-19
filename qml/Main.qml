import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Shine

Window {
    id: root
    width: 470
    height: expanded ? 286 : 62
    visible: true
    color: "transparent"
    title: "Shine"
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint

    property bool expanded: false

    Behavior on height {
        NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
    }

    Rectangle {
        id: frame
        anchors.fill: parent
        radius: 16
        color: "#F00F1117"
        border.color: "#272D40"
        border.width: 1
        clip: true

        DragHandler {
            target: null
            onActiveChanged: {
                if (active) {
                    root.startSystemMove()
                }
            }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 38
                spacing: 10

                RowLayout {
                    spacing: 7
                    Rectangle {
                        width: 10
                        height: 10
                        radius: 5
                        color: recorder.daemonConnected ? "#60A5FA" : "#64748B"
                    }
                    Text {
                        text: "shine"
                        color: "#F8FAFC"
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.4
                    }
                }

                RecordButton {
                    recording: recorder.recording
                    busy: recorder.busy
                    onClicked: recorder.toggleRecording()
                }

                Text {
                    text: recorder.elapsedText
                    color: recorder.recording ? "#F8FAFC" : "#94A3B8"
                    font.pixelSize: 14
                    font.weight: Font.Medium
                    font.family: "Consolas"
                    Layout.preferredWidth: 52
                }

                StatusBadge {
                    gpuActive: recorder.gpuAccelerated
                    gpuText: recorder.gpuBadgeText
                    memoryText: recorder.memoryText
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    width: 28
                    height: 28
                    radius: 8
                    color: gearMouse.containsMouse || root.expanded ? "#1F2536" : "transparent"
                    border.color: root.expanded ? "#3B445E" : "transparent"
                    border.width: 1

                    Column {
                        anchors.centerIn: parent
                        spacing: 3
                        Repeater {
                            model: 3
                            Rectangle {
                                width: 12
                                height: 1.5
                                radius: 1
                                color: "#CBD5E1"
                            }
                        }
                    }

                    MouseArea {
                        id: gearMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.expanded = !root.expanded
                    }
                }

                Rectangle {
                    width: 28
                    height: 28
                    radius: 8
                    color: closeMouse.containsMouse ? "#2B1B22" : "transparent"

                    Canvas {
                        anchors.centerIn: parent
                        width: 10
                        height: 10
                        onPaint: {
                            var ctx = getContext("2d");
                            ctx.clearRect(0, 0, width, height);
                            ctx.strokeStyle = "#94A3B8";
                            ctx.lineWidth = 1.6;
                            ctx.lineCap = "round";
                            ctx.beginPath();
                            ctx.moveTo(1.5, 1.5);
                            ctx.lineTo(8.5, 8.5);
                            ctx.moveTo(8.5, 1.5);
                            ctx.lineTo(1.5, 8.5);
                            ctx.stroke();
                        }
                    }

                    MouseArea {
                        id: closeMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.close()
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: "#22283A"
                visible: root.expanded
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 10
                visible: root.expanded

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Text {
                        text: "Display"
                        color: "#94A3B8"
                        font.pixelSize: 12
                        Layout.preferredWidth: 64
                    }

                    DisplaySelector {
                        Layout.fillWidth: true
                        model: recorder.displays
                        selectedIndex: recorder.selectedDisplay
                        onDisplayChosen: function(idx) {
                            recorder.selectedDisplay = idx
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Text {
                        text: "Framerate"
                        color: "#94A3B8"
                        font.pixelSize: 12
                        Layout.preferredWidth: 64
                    }

                    Repeater {
                        model: [30, 60]
                        delegate: Rectangle {
                            required property int modelData
                            readonly property bool active: recorder.fps === modelData
                            width: 48
                            height: 26
                            radius: 7
                            color: active ? "#26314A" : "#151925"
                            border.color: active ? "#60A5FA" : "#272D40"
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: modelData + " fps"
                                color: active ? "#F8FAFC" : "#94A3B8"
                                font.pixelSize: 11
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: recorder.fps = modelData
                            }
                        }
                    }

                    Item { Layout.preferredWidth: 10 }

                    Text {
                        text: "Bitrate"
                        color: "#94A3B8"
                        font.pixelSize: 12
                    }

                    Repeater {
                        model: [
                            { label: "4M", kbps: 4000 },
                            { label: "8M", kbps: 8000 },
                            { label: "16M", kbps: 16000 }
                        ]
                        delegate: Rectangle {
                            required property var modelData
                            readonly property bool active: recorder.bitrateKbps === modelData.kbps
                            width: 40
                            height: 26
                            radius: 7
                            color: active ? "#26314A" : "#151925"
                            border.color: active ? "#60A5FA" : "#272D40"
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: modelData.label
                                color: active ? "#F8FAFC" : "#94A3B8"
                                font.pixelSize: 11
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: recorder.bitrateKbps = modelData.kbps
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 14

                    Rectangle {
                        implicitWidth: cursorRow.implicitWidth + 16
                        height: 28
                        radius: 8
                        color: recorder.captureCursor ? "#1B2938" : "#151925"
                        border.color: recorder.captureCursor ? "#38BDF8" : "#272D40"
                        border.width: 1

                        RowLayout {
                            id: cursorRow
                            anchors.centerIn: parent
                            spacing: 6
                            Rectangle {
                                width: 8
                                height: 8
                                radius: 4
                                color: recorder.captureCursor ? "#38BDF8" : "#475569"
                            }
                            Text {
                                text: "Cursor"
                                color: "#CBD5E1"
                                font.pixelSize: 11
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: recorder.captureCursor = !recorder.captureCursor
                        }
                    }

                    Rectangle {
                        implicitWidth: autoRow.implicitWidth + 16
                        height: 28
                        radius: 8
                        color: recorder.autostartEnabled ? "#1A2B26" : "#151925"
                        border.color: recorder.autostartEnabled ? "#22C55E" : "#272D40"
                        border.width: 1

                        RowLayout {
                            id: autoRow
                            anchors.centerIn: parent
                            spacing: 6
                            Rectangle {
                                width: 8
                                height: 8
                                radius: 4
                                color: recorder.autostartEnabled ? "#22C55E" : "#475569"
                            }
                            Text {
                                text: "Start on login"
                                color: "#CBD5E1"
                                font.pixelSize: 11
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: recorder.setAutostart(!recorder.autostartEnabled)
                        }
                    }

                    Item { Layout.fillWidth: true }

                    Rectangle {
                        implicitWidth: folderLabel.implicitWidth + 16
                        height: 28
                        radius: 8
                        color: folderMouse.containsMouse ? "#20273B" : "#161B29"
                        border.color: "#2C354C"
                        border.width: 1

                        Text {
                            id: folderLabel
                            anchors.centerIn: parent
                            text: "Recordings"
                            color: "#CBD5E1"
                            font.pixelSize: 11
                        }

                        MouseArea {
                            id: folderMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: recorder.openRecordingsFolder()
                        }
                    }

                    Rectangle {
                        visible: recorder.lastRecordingPath !== ""
                        implicitWidth: playLabel.implicitWidth + 16
                        height: 28
                        radius: 8
                        color: playMouse.containsMouse ? "#23304A" : "#192234"
                        border.color: "#3B82F6"
                        border.width: 1

                        Text {
                            id: playLabel
                            anchors.centerIn: parent
                            text: "Open Last"
                            color: "#E2E8F0"
                            font.pixelSize: 11
                        }

                        MouseArea {
                            id: playMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: recorder.openLastRecording()
                        }
                    }
                }

                Text {
                    visible: recorder.lastError !== ""
                    Layout.fillWidth: true
                    text: recorder.lastError
                    color: "#FDA4AF"
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
            }
        }
    }
}
