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
    minimumHeight: 250
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

            // 画质：帧率和码率是 server 启动参数，改完自动重连
            Rectangle {
                id: videoToggle

                Layout.preferredWidth: 118
                Layout.preferredHeight: 26
                radius: 6
                color: videoArea.containsMouse ? "#2e343e" : "#242932"
                border.width: 1
                border.color: "#333945"

                Text {
                    anchors.centerIn: parent
                    text: qsTr("画质 %1M · %2")
                              .arg((deviceController.videoBitRate / 1000000).toFixed(0))
                              .arg(deviceController.videoProfile === "wireless" ? qsTr("无线") : qsTr("有线"))
                    color: "#cdd4de"
                    font.pixelSize: 12
                }

                MouseArea {
                    id: videoArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: videoPopup.open()
                }
            }

            // 恢复默认配置：清 window.ini、删 config.ini，重启后回到出厂
            Rectangle {
                id: resetToggle

                Layout.preferredWidth: 56
                Layout.preferredHeight: 26
                radius: 6
                color: resetArea.containsMouse ? "#3a2b2b" : "#242932"
                border.width: 1
                border.color: "#4a3434"

                Text {
                    anchors.centerIn: parent
                    text: qsTr("重置")
                    color: "#e0b4b4"
                    font.pixelSize: 12
                }

                MouseArea {
                    id: resetArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: resetPopup.open()
                }
            }

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

            // 画质面板：帧率和码率都能自己填，填完点应用会断开重连生效
            Popup {
                id: videoPopup

                width: 260
                padding: 8
                // 居中到主窗，靠上放：父对象是头部 RowLayout，左上角在窗口内 16px 处。
                // Popup 画在窗口内部，放太低会把底部的「应用」按钮裁掉
                x: Math.round((controlWindow.width - width) / 2) - 16
                y: 18
                modal: true
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

                background: Rectangle {
                    radius: 8
                    color: "#1b1f26"
                    border.width: 1
                    border.color: "#2c323c"
                }

                contentItem: Column {
                    spacing: 6

                    Text {
                        text: qsTr("帧率上限（0 = 不限，一般不用改）")
                        color: "#8b93a3"
                        font.pixelSize: 11
                    }

                    TextField {
                        id: fpsField

                        width: parent.width
                        height: 26
                        text: String(deviceController.videoMaxFps)
                        color: "#d8dee9"
                        font.pixelSize: 13
                        leftPadding: 10
                        selectByMouse: true
                        validator: IntValidator { bottom: 0; top: 240 }

                        background: Rectangle {
                            radius: 6
                            color: "#151920"
                            border.width: 1
                            border.color: "#2c323c"
                        }
                    }

                    Text {
                        text: qsTr("码率（Mbps，可填小数） · 只影响%1")
                                  .arg(deviceController.videoProfile === "wireless" ? qsTr("无线") : qsTr("有线"))
                        color: "#8b93a3"
                        font.pixelSize: 11
                    }

                    TextField {
                        id: bitrateField

                        width: parent.width
                        height: 26
                        text: (deviceController.videoBitRate / 1000000).toFixed(1)
                        color: "#d8dee9"
                        font.pixelSize: 13
                        leftPadding: 10
                        selectByMouse: true
                        validator: DoubleValidator { bottom: 0.5; top: 50.0; decimals: 1 }

                        background: Rectangle {
                            radius: 6
                            color: "#151920"
                            border.width: 1
                            border.color: "#2c323c"
                        }
                    }

                    Text {
                        width: parent.width
                        text: qsTr("改完自动断开重连（1~2 秒）：码率/帧率是 server 启动参数")
                        color: "#6b7280"
                        font.pixelSize: 9
                    }

                    Rectangle {
                        width: parent.width
                        height: 26
                        radius: 6
                        color: applyArea.containsMouse ? "#5aa9ff" : "#3f8fdd"

                        Text {
                            anchors.centerIn: parent
                            text: qsTr("应用并重连")
                            color: "#ffffff"
                            font.pixelSize: 13
                        }

                        MouseArea {
                            id: applyArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                const fps = parseInt(fpsField.text) || 0
                                const mbps = parseFloat(bitrateField.text) || 8.0
                                videoPopup.close()
                                deviceController.ApplyVideoSettings(Math.round(mbps * 1000000), fps)
                            }
                        }
                    }
                }
            }

            // 恢复默认配置的确认框：会删掉 config.ini 和 window.ini 的内容
            Popup {
                id: resetPopup

                width: 280
                padding: 12
                // 同上：居中靠上（父对象是头部 RowLayout，减掉 16px 边距）
                x: Math.round((controlWindow.width - width) / 2) - 16
                y: 18
                modal: true
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

                background: Rectangle {
                    radius: 8
                    color: "#1b1f26"
                    border.width: 1
                    border.color: "#4a3434"
                }

                contentItem: Column {
                    spacing: 10

                    Text {
                        width: parent.width
                        text: qsTr("恢复默认配置？")
                        color: "#e6ebf2"
                        font.pixelSize: 14
                        font.bold: true
                    }

                    Text {
                        width: parent.width
                        text: qsTr("会清空窗口位置尺寸、上次设备、画质参数、命令行开关，并重新生成默认的 config.ini。当前连接不会断开，重启程序后全部生效。")
                        color: "#8b93a3"
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                    }

                    Row {
                        width: parent.width
                        spacing: 8

                        Rectangle {
                            width: (parent.width - parent.spacing) / 2
                            height: 30
                            radius: 6
                            color: cancelArea.containsMouse ? "#2e343e" : "#242932"
                            border.width: 1
                            border.color: "#333945"

                            Text {
                                anchors.centerIn: parent
                                text: qsTr("取消")
                                color: "#cdd4de"
                                font.pixelSize: 13
                            }

                            MouseArea {
                                id: cancelArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: resetPopup.close()
                            }
                        }

                        Rectangle {
                            width: (parent.width - parent.spacing) / 2
                            height: 30
                            radius: 6
                            color: confirmResetArea.containsMouse ? "#c0504d" : "#a63d3a"

                            Text {
                                anchors.centerIn: parent
                                text: qsTr("恢复")
                                color: "#ffffff"
                                font.pixelSize: 13
                            }

                            MouseArea {
                                id: confirmResetArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    resetPopup.close()
                                    deviceController.ResetAllConfig()
                                }
                            }
                        }
                    }
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
