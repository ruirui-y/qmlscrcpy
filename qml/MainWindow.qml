import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15

// 控制窗：设备输入、连接/断开、状态。画面在 VideoWindow，常用操作在 ToolBarWindow。
// 按钮和输入框都用自绘背景：系统默认样式在不同 DPI 的屏幕上会变成白底方块。
Window {
    id: controlWindow

    width: 520
    height: 300
    minimumWidth: 480
    minimumHeight: 280
    visible: true
    title: qsTr("QmlScrcpy")
    color: "#14161a"

    // 投屏窗口独立存在，这里只是把它拉起来
    VideoWindow {
        id: videoWindow
    }

    // 工具栏也是一扇独立窗口，贴在投屏窗右侧；把控制窗自己也交给它，用于"主窗"按钮
    ToolBarWindow {
        target: videoWindow
        control: controlWindow
    }

    // 一个按钮：自绘背景 + 主次两种配色
    component ActionButton: Rectangle {
        id: button

        property string label: ""
        property bool primary: false
        signal clicked()

        implicitWidth: 78
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
            onClicked: button.clicked()
        }
    }

    // 一张卡片的底：圆角 + 细边框，把内容从背景里分出来
    component Card: Rectangle {
        default property alias content: inner.data

        Layout.fillWidth: true
        implicitHeight: inner.implicitHeight + 24
        radius: 10
        color: "#1b1f26"
        border.width: 1
        border.color: "#262c36"

        ColumnLayout {
            id: inner
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        // 顶部：标题 + 帧率/分辨率
        RowLayout {
            Layout.fillWidth: true

            Text {
                text: qsTr("QmlScrcpy")
                color: "#e6ebf2"
                font.pixelSize: 17
                font.bold: true
            }

            Item { Layout.fillWidth: true }

            Text {
                text: videoWindow.fps > 0 ? videoWindow.fps + " fps" : qsTr("-- fps")
                color: "#7c8698"
                font.pixelSize: 12
            }

            Text {
                text: videoWindow.frameSize.width > 0
                      ? videoWindow.frameSize.width + " x " + videoWindow.frameSize.height
                      : ""
                color: "#7c8698"
                font.pixelSize: 12
            }
        }

        // 中间：串号 + 三个动作
        Card {
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                TextField {
                    id: serialField

                    Layout.fillWidth: true
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

        // 底部：状态
        Card {
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
}
