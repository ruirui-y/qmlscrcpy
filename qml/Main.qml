import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15

// P0 的验收点只有三个：qrc 里的 QML 能加载、Controls 2 能渲染、窗口能弹出来。
// 中间那块占位区是给 P2 的视频画面（SceneGraph 自定义节点）预留的位置。
Window {
    id: window

    width: 420
    height: 760
    minimumWidth: 320
    minimumHeight: 480
    visible: true
    title: qsTr("QmlScrcpy")
    color: "#121212"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 16

        Label {
            text: qsTr("QmlScrcpy")
            color: "#e8e8e8"
            font.pixelSize: 28
            font.weight: Font.DemiBold
            Layout.alignment: Qt.AlignHCenter
        }

        Label {
            text: qsTr("v%1 · 等待接入视频渲染").arg(Qt.application.version)
            color: "#8a8a8a"
            font.pixelSize: 13
            Layout.alignment: Qt.AlignHCenter
        }

        Rectangle {
            id: videoPlaceholder

            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#1c1c1c"
            border.color: "#2e2e2e"
            border.width: 1
            radius: 6

            Label {
                anchors.centerIn: parent
                text: qsTr("P0 · 骨架")
                color: "#4a4a4a"
                font.pixelSize: 16
            }
        }
    }
}
