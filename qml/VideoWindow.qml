import QtQuick 2.15
import QtQuick.Window 2.15
import QmlScrcpy 1.0

// 独立的投屏窗口：只放画面，不塞任何控件。
//
// 尺寸策略：第一次连上（还没有保存过尺寸）时，按帧的宽高比把窗口调成一样的比例，
// 这样"拉满窗口"就等于"不变形"；之后用户自己调过的大小会记在 userdata.ini 里。
Window {
    id: videoWindow

    property alias fps: videoSurface.fps    // 帧率，控制窗那边显示
    property alias frameSize: videoSurface.frameSize
    property var saved: ({})    // 上次保存的窗口位置和大小

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
        keepAspect: false    // 窗口比例已经跟着帧走了，这里直接拉满
    }

    Component.onCompleted: {
        deviceController.SetSurface(videoSurface)

        // 恢复上次的位置和大小；没有记录就等第一帧来了再按比例定
        videoWindow.saved = appSettings.VideoGeometry("video")
        if (videoWindow.saved.width > 0) {
            x = videoWindow.saved.x
            y = videoWindow.saved.y
            width = videoWindow.saved.width
            height = videoWindow.saved.height
        }
    }

    // 第一帧到了、且没有保存过尺寸时，把窗口调成和帧一样的物理像素：
    // 除以设备像素比，窗口的逻辑尺寸乘回 DPI 正好等于帧宽高，纹理 1:1 不插值，
    // 画面最清楚（在 125% 缩放的屏幕上尤其明显）。
    onFrameSizeChanged: {
        if (videoWindow.saved.width > 0 || frameSize.width <= 0) {
            return
        }
        const ratio = Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1
        width = Math.round(frameSize.width / ratio)
        height = Math.round(frameSize.height / ratio)
    }

    // 用户拖过、缩过的尺寸记下来，下次连接直接用
    onXChanged: appSettings.SaveVideoGeometry("video", x, y, width, height)
    onYChanged: appSettings.SaveVideoGeometry("video", x, y, width, height)
    onWidthChanged: appSettings.SaveVideoGeometry("video", x, y, width, height)
    onHeightChanged: appSettings.SaveVideoGeometry("video", x, y, width, height)
}
