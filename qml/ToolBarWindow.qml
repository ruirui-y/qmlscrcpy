import QtQuick 2.15
import QtQuick.Window 2.15

// 独立的工具栏窗口：贴在投屏窗右侧，跟着投屏窗移动和显示。
// 不嵌在画面里，所以画面永远是 1:1，不需要为它改窗口尺寸算法。
Window {
    id: toolWindow

    property var target: null    // 投屏窗，没设置就整条栏隐藏
    property bool pinned: false    // 置顶：投屏窗和工具栏一起置顶
    property bool displayOff: false    // 只用来切按钮文字，真实状态在设备那边

    width: 64
    height: target ? target.height : 480
    visible: target !== null && target.visible
    color: "#1c1c1c"
    flags: Qt.Window | Qt.FramelessWindowHint | (pinned ? Qt.WindowStaysOnTopHint : Qt.NoHint)

    // 贴右边；快到屏幕边缘就翻到左边，免得跑到屏幕外面
    x: {
        if (!target) {
            return 0
        }
        const right = target.x + target.width + 6
        return (right + width > Screen.desktopAvailableWidth) ? Math.max(0, target.x - width - 6) : right
    }
    y: target ? target.y : 0

    // 一个按钮：文字 + 选中态配色 + 点击回调
    component ToolButton: Rectangle {
        id: button

        property string label: ""
        property bool active: false
        signal clicked()

        width: parent.width
        height: 30
        radius: 3
        color: area.containsMouse ? "#2f2f2f" : (active ? "#2b4a6f" : "transparent")

        Text {
            anchors.centerIn: parent
            text: button.label
            color: button.active ? "#ffffff" : "#c8c8c8"
            font.pixelSize: 12
        }

        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            onClicked: button.clicked()
        }
    }

    Column {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 4

        ToolButton {
            label: deviceController.wheelMode === "drag" ? qsTr("滑动") : qsTr("滚动")
            active: deviceController.wheelMode === "drag"
            onClicked: deviceController.SetWheelMode(deviceController.wheelMode === "drag" ? "scroll" : "drag")
        }

        ToolButton {
            label: qsTr("音量 +")
            onClicked: deviceController.SendVolumeUp()
        }

        ToolButton {
            label: qsTr("音量 −")
            onClicked: deviceController.SendVolumeDown()
        }

        ToolButton {
            label: qsTr("旋转")
            onClicked: deviceController.RotateDevice()
        }

        ToolButton {
            label: qsTr("截图")
            onClicked: deviceController.SaveScreenshot()
        }

        ToolButton {
            label: toolWindow.displayOff ? qsTr("亮屏") : qsTr("息屏")
            active: toolWindow.displayOff
            onClicked: {
                toolWindow.displayOff = !toolWindow.displayOff
                deviceController.SetDisplayPower(!toolWindow.displayOff)
            }
        }

        ToolButton {
            label: qsTr("置顶")
            active: toolWindow.pinned
            onClicked: {
                toolWindow.pinned = !toolWindow.pinned
                if (toolWindow.target) {
                    toolWindow.target.flags = Qt.Window | Qt.FramelessWindowHint
                            | (toolWindow.pinned ? Qt.WindowStaysOnTopHint : Qt.NoHint)
                }
            }
        }

        ToolButton {
            label: toolWindow.target && toolWindow.target.visibility === Window.FullScreen ? qsTr("还原") : qsTr("全屏")
            active: toolWindow.target && toolWindow.target.visibility === Window.FullScreen
            onClicked: {
                if (!toolWindow.target) {
                    return
                }
                toolWindow.target.visibility = toolWindow.target.visibility === Window.FullScreen
                        ? Window.Windowed : Window.FullScreen
            }
        }
    }
}
