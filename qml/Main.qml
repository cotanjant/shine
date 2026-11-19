import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Shine

Window {
    id: root
    width: 540
    height: {
        if (!recorder.firstRunCompleted) {
            return 356
        }
        if (activeDrawer === "settings") {
            return 378
        }
        if (activeDrawer === "clips") {
            return 304
        }
        return 62
    }
    visible: true
    color: "transparent"
    title: "Shine"
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint

    property string activeDrawer: "none"
    property bool clipFlash: false

    Behavior on height {
        NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
    }

    Timer {
        id: flashTimer
        interval: 1600
        repeat: false
        onTriggered: root.clipFlash = false
    }

    Connections {
        target: recorder
        function onClipSaved() {
            root.clipFlash = true
            flashTimer.restart()
        }
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
            spacing: 10

            FirstRunWizard {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: !recorder.firstRunCompleted
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 12
                visible: recorder.firstRunCompleted

                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 38
                    spacing: 8

                    RowLayout {
                        spacing: 6
                        Rectangle {
                            width: 9
                            height: 9
                            radius: 4.5
                            color: recorder.daemonConnected ? "#60A5FA" : "#64748B"
                        }
                        Text {
                            text: "shine"
                            color: "#F8FAFC"
                            font.pixelSize: 13.5
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
                        font.pixelSize: 13
                        font.weight: Font.Medium
                        font.family: "Consolas"
                        Layout.preferredWidth: 44
                    }

                    Rectangle {
                        implicitWidth: clipBtnRow.implicitWidth + 16
                        height: 28
                        radius: 8
                        color: root.clipFlash ? "#1E3A2F" : (clipMouse.containsMouse ? "#28213D" : "#1A182B")
                        border.color: root.clipFlash ? "#22C55E" : (recorder.replayActive ? "#8B5CF6" : "#2D3348")
                        border.width: 1

                        RowLayout {
                            id: clipBtnRow
                            anchors.centerIn: parent
                            spacing: 5
                            Rectangle {
                                width: 6
                                height: 6
                                radius: 3
                                color: root.clipFlash ? "#22C55E" : (recorder.replayActive ? "#A855F7" : "#64748B")
                            }
                            Text {
                                text: root.clipFlash ? "Clipped!" : ("Clip " + recorder.clipHotkey)
                                color: root.clipFlash ? "#BBF7D0" : "#E9D5FF"
                                font.pixelSize: 11
                                font.weight: Font.Medium
                            }
                        }

                        MouseArea {
                            id: clipMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: recorder.saveClipNow()
                        }
                    }

                    StatusBadge {
                        gpuActive: recorder.gpuAccelerated
                        gpuText: recorder.gpuBadgeText
                        memoryText: recorder.memoryText
                    }

                    Item { Layout.fillWidth: true }

                    Rectangle {
                        implicitWidth: clipsBtnTxt.implicitWidth + 14
                        height: 28
                        radius: 8
                        color: clipsBtnMouse.containsMouse || root.activeDrawer === "clips" ? "#1F2536" : "transparent"
                        border.color: root.activeDrawer === "clips" ? "#3B445E" : "#242A3B"
                        border.width: 1

                        Text {
                            id: clipsBtnTxt
                            anchors.centerIn: parent
                            text: "Clips (" + recorder.clips.count + ")"
                            color: "#CBD5E1"
                            font.pixelSize: 11
                        }

                        MouseArea {
                            id: clipsBtnMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (root.activeDrawer === "clips") {
                                    root.activeDrawer = "none"
                                } else {
                                    recorder.refreshClips()
                                    root.activeDrawer = "clips"
                                }
                            }
                        }
                    }

                    Rectangle {
                        width: 28
                        height: 28
                        radius: 8
                        color: gearMouse.containsMouse || root.activeDrawer === "settings" ? "#1F2536" : "transparent"
                        border.color: root.activeDrawer === "settings" ? "#3B445E" : "transparent"
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
                            onClicked: {
                                root.activeDrawer = (root.activeDrawer === "settings") ? "none" : "settings"
                            }
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
                    visible: root.activeDrawer !== "none"
                }

                ClipsPanel {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: root.activeDrawer === "clips"
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 9
                    visible: root.activeDrawer === "settings"

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Text {
                            text: "Display"
                            color: "#94A3B8"
                            font.pixelSize: 12
                            Layout.preferredWidth: 68
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
                        spacing: 8

                        Text {
                            text: "Replay"
                            color: "#94A3B8"
                            font.pixelSize: 12
                            Layout.preferredWidth: 68
                        }

                        Rectangle {
                            implicitWidth: repTxt.implicitWidth + 16
                            height: 26
                            radius: 7
                            color: recorder.replayEnabled ? "#271F3B" : "#151925"
                            border.color: recorder.replayEnabled ? "#8B5CF6" : "#272D40"
                            border.width: 1

                            Text {
                                id: repTxt
                                anchors.centerIn: parent
                                text: recorder.replayEnabled ? "Buffer On" : "Buffer Off"
                                color: recorder.replayEnabled ? "#E9D5FF" : "#94A3B8"
                                font.pixelSize: 11
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: recorder.replayEnabled = !recorder.replayEnabled
                            }
                        }

                        HotkeyRecorder {
                            binding: recorder.clipHotkey
                            onHotkeyChosen: function(newBinding) {
                                recorder.clipHotkey = newBinding
                            }
                        }

                        Repeater {
                            model: [15, 30, 60, 120]
                            delegate: Rectangle {
                                required property int modelData
                                readonly property bool active: recorder.clipLengthSeconds === modelData
                                width: 38
                                height: 26
                                radius: 7
                                color: active ? "#26314A" : "#151925"
                                border.color: active ? "#60A5FA" : "#272D40"
                                border.width: 1

                                Text {
                                    anchors.centerIn: parent
                                    text: modelData + "s"
                                    color: active ? "#F8FAFC" : "#94A3B8"
                                    font.pixelSize: 10.5
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: recorder.clipLengthSeconds = modelData
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Text {
                            text: "Voice"
                            color: "#94A3B8"
                            font.pixelSize: 12
                            Layout.preferredWidth: 68
                        }

                        Rectangle {
                            implicitWidth: vRow.implicitWidth + 14
                            height: 26
                            radius: 7
                            color: recorder.voiceEnabled ? "#1A2B26" : "#151925"
                            border.color: recorder.voiceEnabled ? "#22C55E" : "#272D40"
                            border.width: 1

                            RowLayout {
                                id: vRow
                                anchors.centerIn: parent
                                spacing: 5
                                Rectangle {
                                    width: 7
                                    height: 7
                                    radius: 3.5
                                    color: recorder.voiceEnabled ? "#22C55E" : "#475569"
                                }
                                Text {
                                    text: "Mic in Clips"
                                    color: "#CBD5E1"
                                    font.pixelSize: 11
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: recorder.voiceEnabled = !recorder.voiceEnabled
                            }
                        }

                        Rectangle {
                            visible: recorder.voiceEnabled
                            implicitWidth: pttSetRow.implicitWidth + 14
                            height: 26
                            radius: 7
                            color: recorder.voicePttEnabled ? "#1B2938" : "#151925"
                            border.color: recorder.voicePttEnabled ? "#38BDF8" : "#272D40"
                            border.width: 1

                            RowLayout {
                                id: pttSetRow
                                anchors.centerIn: parent
                                spacing: 5
                                Rectangle {
                                    width: 7
                                    height: 7
                                    radius: 3.5
                                    color: recorder.voicePttEnabled ? "#38BDF8" : "#475569"
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
                                onClicked: recorder.voicePttEnabled = !recorder.voicePttEnabled
                            }
                        }

                        HotkeyRecorder {
                            visible: recorder.voiceEnabled && recorder.voicePttEnabled
                            binding: recorder.voicePttHotkey
                            onHotkeyChosen: function(newBinding) {
                                recorder.voicePttHotkey = newBinding
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Text {
                            text: "Quality"
                            color: "#94A3B8"
                            font.pixelSize: 12
                            Layout.preferredWidth: 68
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

                        Item { Layout.preferredWidth: 6 }

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
                        spacing: 10

                        Rectangle {
                            implicitWidth: cursorRow.implicitWidth + 14
                            height: 26
                            radius: 7
                            color: recorder.captureCursor ? "#1B2938" : "#151925"
                            border.color: recorder.captureCursor ? "#38BDF8" : "#272D40"
                            border.width: 1

                            RowLayout {
                                id: cursorRow
                                anchors.centerIn: parent
                                spacing: 5
                                Rectangle {
                                    width: 7
                                    height: 7
                                    radius: 3.5
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
                            implicitWidth: autoRow.implicitWidth + 14
                            height: 26
                            radius: 7
                            color: recorder.autostartEnabled ? "#1A2B26" : "#151925"
                            border.color: recorder.autostartEnabled ? "#22C55E" : "#272D40"
                            border.width: 1

                            RowLayout {
                                id: autoRow
                                anchors.centerIn: parent
                                spacing: 5
                                Rectangle {
                                    width: 7
                                    height: 7
                                    radius: 3.5
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
                            implicitWidth: folderLabel.implicitWidth + 14
                            height: 26
                            radius: 7
                            color: folderMouse.containsMouse ? "#20273B" : "#161B29"
                            border.color: "#2C354C"
                            border.width: 1

                            Text {
                                id: folderLabel
                                anchors.centerIn: parent
                                text: "Folder"
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
}
