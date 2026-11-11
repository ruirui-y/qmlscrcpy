import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15

// 控制窗：设备输入、连接/断开、状态和帧率。画面在另一扇窗（VideoWindow）。
Window {
    id: controlWindow

    width: 460
    height: 200
    minimumWidth: 420
    minimumHeight: 180
    visible: true
    title: qsTr("QmlScrcpy")
    color: "#121212"

    // 投屏窗口独立存在，这里只是把它拉起来
    VideoWindow {
        id: videoWindow
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 12

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
            spacing: 12

            Item { Layout.fillWidth: true }

            Label {
                text: videoWindow.fps > 0 ? videoWindow.fps + " fps" : qsTr("-- fps")
                color: "#8a8a8a"
                font.pixelSize: 13
            }

            Label {
                text: videoWindow.frameSize.width > 0
                      ? videoWindow.frameSize.width + "x" + videoWindow.frameSize.height
                      : ""
                color: "#8a8a8a"
                font.pixelSize: 13
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.fillHeight: true
            verticalAlignment: Text.AlignTop
            text: deviceController.status
            color: "#8a8a8a"
            wrapMode: Text.Wrap
            elide: Text.ElideRight
        }
    }
}
