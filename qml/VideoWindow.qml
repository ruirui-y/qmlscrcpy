import QtQuick 2.15
import QtQuick.Window 2.15
import QmlScrcpy 1.0

// 独立的投屏窗口：只放画面，不塞任何控件。
// 控制按钮在 MainWindow 里，两边通过 QML 的属性绑定联动。
Window {
    id: videoWindow

    property bool freeScale: false    // true 时不保持宽高比，拉满整块窗口
    property alias fps: videoSurface.fps    // 帧率，控制窗那边显示
    property alias frameSize: videoSurface.frameSize

    width: 380
    height: 780
    minimumWidth: 160
    minimumHeight: 240
    visible: true
    title: qsTr("QmlScrcpy — 投屏")
    color: "#000000"

    VideoSurface {
        id: videoSurface
        objectName: "videoSurface"
        anchors.fill: parent
        keepAspect: !videoWindow.freeScale
    }

    // 把画布交给控制器，连接成功后的帧就送这里
    Component.onCompleted: deviceController.SetSurface(videoSurface)
}
