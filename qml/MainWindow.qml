import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15

// 控制窗：一行操作 + 一行状态。画面和工具栏都在 VideoWindow 里。
// 按钮和输入框都用自绘背景：系统默认样式在不同 DPI 的屏幕上会变成白底方块。
Window {
    id: controlWindow

    width: 600
    height: 250
    minimumWidth: 600
    minimumHeight: 230
    visible: true
    title: qsTr("QmlScrcpy")
    color: "#14161a"

    // 主窗控制整个进程：关掉它就退出（投屏窗和工具栏跟着走）
    onClosing: Qt.quit()

    // 换屏时 DPI 变了，Qt 会重算窗口尺寸；这里保证不缩到装不下内容
    onScreenChanged: {
        width = Math.max(width, minimumWidth)
        height = Math.max(height, minimumHeight)
    }

    // 位置和尺寸记在 window.ini，下次启动回到原处；拖动/缩放时防抖写盘
    property bool applying: false
    property bool ready: false

    Component.onCompleted: {
        applying = true
        const saved = appSettings.VideoGeometry("control_window")
        if (saved.width > 0) {
            x = saved.x
            y = saved.y
            width = Math.max(minimumWidth, saved.width)
            height = Math.max(minimumHeight, saved.height)
        }
        applying = false
        ready = true
        if (saved.width <= 0) {
            // 第一次运行没有记录：把当前（Qt 给的）位置先存下来，下次就在这儿了
            saveTimer.restart()
        }
    }

    Timer {
        id: saveTimer
        interval: 400
        repeat: false
        onTriggered: appSettings.SaveVideoGeometry("control_window", controlWindow.x, controlWindow.y,
                                                   controlWindow.width, controlWindow.height)
    }

    onXChanged: scheduleSave()
    onYChanged: scheduleSave()
    onWidthChanged: scheduleSave()
    onHeightChanged: scheduleSave()

    function scheduleSave() {
        if (applying || !ready) {
            return
        }
        saveTimer.restart()
    }

    // 投屏窗口独立存在，这里只是把它拉起来
    VideoWindow {
        id: videoWindow
    }


    // 一个按钮：自绘背景 + 主次两种配色
    component ActionButton: Rectangle {
        id: button

        property string label: ""
        property bool primary: false
        signal clicked()

        implicitWidth: 72
        implicitHeight: 34
        radius: 6
        opacity: enabled ? 1.0 : 0.5
        color: {
            if (!enabled) {
                return "#23262d"
            }
            if (area.containsMouse) {
                return primary ? "#5aa9ff" : "#2e343e"
            }
            return primary ? "#3f8fdd" : "#242932"
        }
        border.width: 1
        border.color: primary ? "#4f9ae8" : "#333945"

        Text {
            anchors.centerIn: parent
            text: button.label
            color: button.primary ? "#ffffff" : "#cdd4de"
            font.pixelSize: 13
        }

        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            enabled: button.enabled
            cursorShape: Qt.PointingHandCursor
            onClicked: button.clicked()
        }
    }

    // 设备菜单：列在线设备，点一个直接连；最下面一项是用 USB 设备开无线
    Popup {
        id: devicePopup

        width: 300
        padding: 6
        x: 0
        y: 0
        modal: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            radius: 8
            color: "#1b1f26"
            border.width: 1
            border.color: "#2c323c"
        }

        contentItem: Column {
            spacing: 2

            Repeater {
                model: deviceController.deviceLabels

                delegate: Rectangle {
                    width: parent.width
                    height: 30
                    radius: 4
                    color: itemArea.containsMouse ? "#2c313a" : "transparent"

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData
                        color: "#d8dee9"
                        font.pixelSize: 13
                    }

                    MouseArea {
                        id: itemArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            // 只把地址填进输入框，连不连由用户点「连接」决定
                            devicePopup.close()
                            serialField.text = deviceController.devices[index]
                        }
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: "#2c323c"
                visible: deviceController.deviceLabels.length > 0
            }

            Rectangle {
                width: parent.width
                height: 30
                radius: 4
                color: wifiArea.containsMouse ? "#2c313a" : "transparent"

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("用 USB 设备开无线调试")
                    color: "#8fb8ff"
                    font.pixelSize: 13
                }

                MouseArea {
                    id: wifiArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        devicePopup.close()
                        deviceController.EnableWifi()
                    }
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true

            Text {
                text: qsTr("QmlScrcpy")
                color: "#e6ebf2"
                font.pixelSize: 17
                font.bold: true
            }

            Item { Layout.fillWidth: true }

            // 命令行窗口开关：状态记在 window.ini，下次启动照这个来
            Rectangle {
                id: consoleToggle

                Layout.preferredWidth: 88
                Layout.preferredHeight: 26
                radius: 6
                color: deviceController.showConsole ? "#2b4a6f" : (consoleArea.containsMouse ? "#2e343e" : "#242932")
                border.width: 1
                border.color: deviceController.showConsole ? "#3f6f9f" : "#333945"

                Text {
                    anchors.centerIn: parent
                    text: qsTr("命令行")
                    color: deviceController.showConsole ? "#ffffff" : "#cdd4de"
                    font.pixelSize: 12
                }

                MouseArea {
                    id: consoleArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: deviceController.SetShowConsole(!deviceController.showConsole)
                }
            }
        }

        // 唯一的一行操作：输入 → 设备菜单 → 刷新 → 连接 → 断开
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 58
            radius: 10
            color: "#1b1f26"
            border.width: 1
            border.color: "#262c36"

            RowLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                TextField {
                    id: serialField

                    Layout.fillWidth: true
                    Layout.minimumWidth: 140
                    Layout.preferredHeight: 34
                    placeholderText: qsTr("设备序列号 / IP:端口")
                    text: deviceController.proposedSerial
                    selectByMouse: true
                    color: "#d8dee9"
                    font.pixelSize: 13
                    leftPadding: 10
                    placeholderTextColor: "#6b7280"

                    background: Rectangle {
                        radius: 6
                        color: "#151920"
                        border.width: 1
                        border.color: serialField.activeFocus ? "#3f8fdd" : "#2c323c"
                    }
                }

                // 设备菜单开关
                Rectangle {
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 34
                    radius: 6
                    color: menuArea.containsMouse ? "#2e343e" : "#242932"
                    border.width: 1
                    border.color: "#333945"

                    Text {
                        anchors.centerIn: parent
                        text: "▾"
                        color: "#cdd4de"
                        font.pixelSize: 14
                    }

                    MouseArea {
                        id: menuArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            devicePopup.x = serialField.mapToItem(controlWindow.contentItem, 0, 0).x
                            devicePopup.y = serialField.mapToItem(controlWindow.contentItem, 0, 0).y + 40
                            devicePopup.open()
                        }
                    }
                }

                ActionButton {
                    label: qsTr("刷新")
                    onClicked: deviceController.RefreshDevices()
                }

                ActionButton {
                    label: qsTr("连接")
                    primary: true
                    onClicked: deviceController.Connect(serialField.text)
                }

                ActionButton {
                    label: qsTr("断开")
                    onClicked: deviceController.Disconnect()
                }
            }
        }


        // 状态
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Rectangle {
                width: 8
                height: 8
                radius: 4
                color: deviceController.deviceName.length > 0 ? "#3ecf8e" : "#5a6272"
            }

            Text {
                Layout.fillWidth: true
                text: deviceController.status
                color: "#9aa4b2"
                font.pixelSize: 12
                wrapMode: Text.Wrap
                elide: Text.ElideRight
            }
        }
    }
}
