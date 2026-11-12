import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import QmlScrcpy 1.0

// 独立的投屏窗口：自绘标题栏 + 画面，不带系统边框。
//
// 为什么要自绘：系统标题栏拖拽会进 Windows 的模态移动循环，GUI 线程被扣住，
// 重绘请求发不出去，拖的时候画面是冻的。自己处理鼠标移动，事件循环一直在转，
// 拖着走画面也在动。
Window {
    id: videoWindow

    property alias fps: videoSurface.fps
    property alias frameSize: videoSurface.frameSize
    property bool applying: false
    property bool ready: false

    width: 380
    height: 808                                                                                 // 780 的画面 + 28 的标题栏
    minimumWidth: 160
    minimumHeight: 268
    visible: true
    title: qsTr("QmlScrcpy — 投屏")
    color: "#000000"
    flags: Qt.Window | Qt.FramelessWindowHint

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            id: titleBar

            Layout.fillWidth: true
            Layout.preferredHeight: 28
            color: "#1c1c1c"

            // 拖动：按屏幕绝对坐标算位移，窗口自己移动不会污染计算
            MouseArea {
                id: dragArea

                anchors.fill: parent
                property point pressGlobal
                property point pressWindow

                onPressed: {
                    pressGlobal = dragArea.mapToGlobal(mouse.x, mouse.y)
                    pressWindow = Qt.point(videoWindow.x, videoWindow.y)
                }
                onPositionChanged: {
                    if (!pressed) {
                        return
                    }
                    const current = dragArea.mapToGlobal(mouse.x, mouse.y)
                    videoWindow.x = pressWindow.x + (current.x - pressGlobal.x)
                    videoWindow.y = pressWindow.y + (current.y - pressGlobal.y)
                }
            }

            Label {
                anchors.left: parent.left
                anchors.leftMargin: 22                                                          // 让开左上角那个缩放把手
                anchors.verticalCenter: parent.verticalCenter
                text: videoWindow.title
                color: "#a8a8a8"
                font.pixelSize: 12
            }

            Row {
                anchors.right: parent.right
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                // 最小化
                Rectangle {
                    width: 22
                    height: 20
                    color: minimizeArea.containsMouse ? "#333333" : "transparent"

                    Text {
                        anchors.centerIn: parent
                        text: "—"
                        color: "#c8c8c8"
                        font.pixelSize: 12
                    }

                    MouseArea {
                        id: minimizeArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: videoWindow.showMinimized()
                    }
                }

                // 关闭：只关这扇窗，控制窗还在
                Rectangle {
                    width: 22
                    height: 20
                    color: closeArea.containsMouse ? "#8c2b2b" : "transparent"

                    Text {
                        anchors.centerIn: parent
                        text: "✕"
                        color: "#c8c8c8"
                        font.pixelSize: 12
                    }

                    MouseArea {
                        id: closeArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: videoWindow.visible = false
                    }
                }
            }
        }

        VideoSurface {
            id: videoSurface
            objectName: "videoSurface"
        focus: true    // 键盘事件只有拿到焦点才收得到
            Layout.fillWidth: true
            Layout.fillHeight: true
            keepAspect: false
        }
    }

    // 右下角拖拽改尺寸
    Rectangle {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 16
        height: 16
        color: "transparent"

        // 三条斜线当把手图案，比纯隐形的热区好找
        Repeater {
            model: 3

            Rectangle {
                width: 11
                height: 1
                color: "#6a6a6a"
                rotation: -45
                x: 3 + index * 4
                y: 11 - index * 4
            }
        }
    }

    MouseArea {
        id: resizeArea

        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 16
        height: 16
        cursorShape: Qt.SizeFDiagCursor
        property point pressGlobal
        property size pressSize

        onPressed: {
            pressGlobal = mapToGlobal(mouse.x, mouse.y)
            pressSize = Qt.size(videoWindow.width, videoWindow.height)
        }
        onPositionChanged: {
            if (!pressed) {
                return
            }
            const current = mapToGlobal(mouse.x, mouse.y)
            videoWindow.width = Math.max(videoWindow.minimumWidth,
                                         pressSize.width + (current.x - pressGlobal.x))
            videoWindow.height = Math.max(videoWindow.minimumHeight,
                                          pressSize.height + (current.y - pressGlobal.y))
        }
    }

    // 左上角也能缩放：右下角保持不动，窗口往左上去长
    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        width: 16
        height: 16
        color: "transparent"

        Repeater {
            model: 3

            Rectangle {
                width: 11
                height: 1
                color: "#6a6a6a"
                rotation: -45
                x: 2 + index * 4
                y: 2 + index * 4
            }
        }
    }

    MouseArea {
        id: resizeTopLeftArea

        anchors.left: parent.left
        anchors.top: parent.top
        width: 16
        height: 16
        cursorShape: Qt.SizeFDiagCursor
        property point pressGlobal
        property rect pressRect

        onPressed: {
            pressGlobal = mapToGlobal(mouse.x, mouse.y)
            pressRect = Qt.rect(videoWindow.x, videoWindow.y, videoWindow.width, videoWindow.height)
        }
        onPositionChanged: {
            if (!pressed) {
                return
            }
            const current = mapToGlobal(mouse.x, mouse.y)
            const wantWidth = pressRect.width - (current.x - pressGlobal.x)
            const wantHeight = pressRect.height - (current.y - pressGlobal.y)
            // 先夹住最小尺寸，再按"右下角不动"反推 x/y，不然拖过头窗口会抖
            videoWindow.width = Math.max(videoWindow.minimumWidth, wantWidth)
            videoWindow.height = Math.max(videoWindow.minimumHeight, wantHeight)
            videoWindow.x = pressRect.x + (pressRect.width - videoWindow.width)
            videoWindow.y = pressRect.y + (pressRect.height - videoWindow.height)
        }
    }

    // 最外层一圈细边框：无边框窗口没有系统边框，不加这个看不出窗口边界在哪
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.color: "#3f3f3f"
        border.width: 1
    }

    Component.onCompleted: {
        deviceController.SetSurface(videoSurface)

        applying = true
        const position = appSettings.VideoPosition("video_pos")
        if (position.x >= 0 && position.y >= 0) {
            x = position.x
            y = position.y
        }
        const saved = appSettings.VideoGeometry("video")
        if (saved.width > 0) {
            width = saved.width
            height = saved.height
        }
        applying = false
        ready = saved.width > 0
    }

    // 第一帧到了、用户又没定过尺寸：把窗口调成和帧一样的物理像素（纹理 1:1 不插值）。
    // 原生分辨率常常比屏幕还大，所以宽和高都要按屏幕可用区域收缩，不能只夹宽度。
    onFrameSizeChanged: {
        if (frameSize.width <= 0 || appSettings.VideoGeometry("video").width > 0) {
            return
        }
        const ratio = Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1
        const frameRatio = frameSize.height / frameSize.width
        const maxWidth = Screen.desktopAvailableWidth * 0.85
        const maxHeight = Screen.desktopAvailableHeight * 0.85 - titleBar.height

        let wantWidth = frameSize.width / ratio
        let wantHeight = wantWidth * frameRatio
        if (wantHeight > maxHeight) {
            wantHeight = maxHeight
            wantWidth = wantHeight / frameRatio
        }
        if (wantWidth > maxWidth) {
            wantWidth = maxWidth
            wantHeight = wantWidth * frameRatio
        }

        applying = true
        width = Math.round(wantWidth)
        height = Math.round(wantHeight) + titleBar.height
        applying = false
        ready = true
    }

    // 位置和尺寸都在"停手 400ms 之后"才写盘。
    // 以前每个 onXxxChanged 都写一次，拖动时一秒上百次 QSettings::sync()，
    // 落盘把 UI 卡住好几秒，看起来就是"窗口不跟手"。
    Timer {
        id: saveTimer

        interval: 400
        repeat: false
        onTriggered: {
            appSettings.SaveVideoPosition("video_pos", videoWindow.x, videoWindow.y)
            if (ready) {
                appSettings.SaveVideoGeometry("video", videoWindow.x, videoWindow.y,
                                              videoWindow.width, videoWindow.height)
            }
        }
    }

    onXChanged: scheduleSave()
    onYChanged: scheduleSave()
    onWidthChanged: scheduleSave()
    onHeightChanged: scheduleSave()

    function scheduleSave() {
        if (applying) {
            return
        }
        saveTimer.restart()
    }
}
