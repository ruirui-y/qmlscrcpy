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
                anchors.leftMargin: 10
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
            Layout.fillWidth: true
            Layout.fillHeight: true
            keepAspect: false
        }
    }

    // 右下角拖拽改尺寸：无边框窗口没有系统的边角
    MouseArea {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 14
        height: 14
        cursorShape: Qt.SizeFDiagCursor
        property point pressGlobal
        property size pressSize_h

        onPressed: {
            pressGlobal = mapToGlobal(mouse.x, mouse.y)
            pressSize_h = Qt.size(videoWindow.width, videoWindow.height)
        }
        onPositionChanged: {
            if (!pressed) {
                return
            }
            const current = mapToGlobal(mouse.x, mouse.y)
            videoWindow.width = Math.max(videoWindow.minimumWidth,
                                         pressSize_h.width + (current.x - pressGlobal.x))
            videoWindow.height = Math.max(videoWindow.minimumHeight,
                                          pressSize_h.height + (current.y - pressGlobal.y))
        }
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

    // 第一帧到了、用户又没定过尺寸：窗口调成和帧一样的物理像素，纹理 1:1 不插值
    onFrameSizeChanged: {
        if (frameSize.width <= 0 || appSettings.VideoGeometry("video").width > 0) {
            return
        }
        const ratio = Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1
        applying = true
        width = Math.round(frameSize.width / ratio)
        height = Math.round(frameSize.height / ratio) + titleBar.height
        applying = false
        ready = true
    }

    onXChanged: savePosition()
    onYChanged: savePosition()
    onWidthChanged: saveSize()
    onHeightChanged: saveSize()

    function savePosition() {
        if (applying) {
            return
        }
        appSettings.SaveVideoPosition("video_pos", x, y)
    }

    function saveSize() {
        if (applying || !ready) {
            return
        }
        appSettings.SaveVideoGeometry("video", x, y, width, height)
    }
}
