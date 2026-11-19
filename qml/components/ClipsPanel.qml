import QtQuick
import QtQuick.Layouts

Item {
    id: root
    implicitHeight: 220

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        RowLayout {
            Layout.fillWidth: true

            Text {
                text: "Saved Clips & Recordings (" + recorder.clips.count + ")"
                color: "#CBD5E1"
                font.pixelSize: 12
                font.weight: Font.Medium
            }

            Item { Layout.fillWidth: true }

            Rectangle {
                implicitWidth: refreshTxt.implicitWidth + 14
                height: 24
                radius: 6
                color: refreshMouse.containsMouse ? "#20273B" : "#151925"
                border.color: "#272D40"
                border.width: 1

                Text {
                    id: refreshTxt
                    anchors.centerIn: parent
                    text: "Refresh"
                    color: "#94A3B8"
                    font.pixelSize: 10.5
                }

                MouseArea {
                    id: refreshMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: recorder.refreshClips()
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: 10
            color: "#121622"
            border.color: "#212738"
            border.width: 1
            clip: true

            Text {
                anchors.centerIn: parent
                visible: recorder.clips.count === 0
                text: "No clips yet — press " + recorder.clipHotkey + " or click Clip"
                color: "#64748B"
                font.pixelSize: 11.5
            }

            ListView {
                id: listView
                anchors.fill: parent
                anchors.margins: 6
                spacing: 6
                model: recorder.clips
                visible: recorder.clips.count > 0

                delegate: Rectangle {
                    required property string filePath
                    required property string fileName
                    required property string title
                    required property string durationText
                    required property string sizeText
                    required property string dateText
                    required property string codec
                    required property bool hasAudio
                    required property bool isClip

                    width: listView.width
                    height: 46
                    radius: 8
                    color: rowHover.containsMouse ? "#1B2133" : "#161A28"
                    border.color: "#252B3D"
                    border.width: 1

                    MouseArea {
                        id: rowHover
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.NoButton
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 8
                        spacing: 8

                        Rectangle {
                            width: 34
                            height: 20
                            radius: 5
                            color: isClip ? "#271F3B" : "#1B2938"
                            border.color: isClip ? "#8B5CF6" : "#38BDF8"
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: isClip ? "CLIP" : "REC"
                                color: isClip ? "#DDD6FE" : "#BAE6FD"
                                font.pixelSize: 9.5
                                font.weight: Font.DemiBold
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1

                            Text {
                                Layout.fillWidth: true
                                text: title
                                color: "#F1F5F9"
                                font.pixelSize: 11.5
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                            }

                            Text {
                                Layout.fillWidth: true
                                text: dateText + " · " + durationText + " · " + sizeText + " · " + codec + (hasAudio ? " · MIC" : "")
                                color: "#94A3B8"
                                font.pixelSize: 10
                                elide: Text.ElideRight
                            }
                        }

                        Rectangle {
                            width: 44
                            height: 24
                            radius: 6
                            color: playMouse.containsMouse ? "#2563EB" : "#1D3461"

                            Text {
                                anchors.centerIn: parent
                                text: "Play"
                                color: "#E2E8F0"
                                font.pixelSize: 10.5
                                font.weight: Font.Medium
                            }

                            MouseArea {
                                id: playMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: recorder.openClip(filePath)
                            }
                        }

                        Rectangle {
                            width: 46
                            height: 24
                            radius: 6
                            color: dirMouse.containsMouse ? "#252B3E" : "#191E2D"
                            border.color: "#2C344A"
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: "Show"
                                color: "#CBD5E1"
                                font.pixelSize: 10.5
                            }

                            MouseArea {
                                id: dirMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: recorder.revealClip(filePath)
                            }
                        }

                        Rectangle {
                            width: 24
                            height: 24
                            radius: 6
                            color: delMouse.containsMouse ? "#3F1D26" : "#1A1E2B"
                            border.color: delMouse.containsMouse ? "#F43F5E" : "#2B3144"
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: "×"
                                color: delMouse.containsMouse ? "#FDA4AF" : "#94A3B8"
                                font.pixelSize: 13
                            }

                            MouseArea {
                                id: delMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: recorder.deleteClip(filePath)
                            }
                        }
                    }
                }
            }
        }
    }
}
