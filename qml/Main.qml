import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import QmlScrcpy 1.0

// 主窗口：上面一行是设备输入 + 连接/断开，下面是视频画布。
Window {
    id: window

    width: 420
    height: 760
    minimumWidth: 360
    minimumHeight: 520
    visible: true
    title: qsTr("QmlScrcpy")
    color: "#121212"

    // 刷新出来的第一台设备自动填进输入框，省得手打序列号
    // （绑定在 TextField.text 上，这里不用 Connections）

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                id: serialField
                Layout.fillWidth: true
                placeholderText: qsTr("设备序列号 / IP:端口")
                // 刷新后自动填第一台在线设备；用户一改就断开绑定
                text: deviceController.proposedSerial
                selectByMouse: true
            }

            Button {
                text: qsTr("刷新")
                onClicked: deviceController.RefreshDevices()
            }

            Button {
                text: qsTr("连接")
                onClicked: deviceController.Connect(serialField.text)
            }

            Button {
                text: qsTr("断开")
                onClicked: deviceController.Disconnect()
            }
        }

        RowLayout {
            Layout.fillWidth: true

            Label {
                text: deviceController.status
                color: "#8a8a8a"
                font.pixelSize: 13
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            Label {
                // 画布报上来的实际帧尺寸，旋转后会变
                text: videoSurface.frameSize.width > 0
                      ? videoSurface.frameSize.width + "x" + videoSurface.frameSize.height
                      : ""
                color: "#8a8a8a"
                font.pixelSize: 13
            }
        }

        // C++ 侧按 objectName 找这个对象，把解出来的帧送进来
        VideoSurface {
            id: videoSurface
            objectName: "videoSurface"
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }
}
