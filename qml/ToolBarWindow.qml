import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15

// 独立的工具栏窗口：默认贴在投屏窗右侧，跟着投屏窗移动和显示。
// 按住空白处拖动它，等于拖动投屏窗，两者一起走。
// 不嵌在画面里，所以画面永远是 1:1，不需要为它改窗口尺寸算法。
Window {
    id: toolWindow

    property var target: null    // 投屏窗，没设置就整条栏隐藏
    property var control: null    // 控制窗，给"主窗"按钮把窗口提到前台用
    property bool pinned: false    // 置顶：投屏窗和工具栏一起置顶
    property bool displayOff: false    // 只用来切按钮文字，真实状态在设备那边

    width: 64
    height: target ? target.height : 480
    visible: target !== null && target.visible
    color: "#1c1c1c"
    flags: Qt.Window | Qt.FramelessWindowHint | (pinned ? Qt.WindowStaysOnTopHint : Qt.NoHint)

    // 贴紧投屏窗右侧；快到屏幕边缘就翻到左边
    function followX() {
        if (!target) {
            return 0
        }
        const right = target.x + target.width
        return (right + width > Screen.desktopAvailableWidth) ? Math.max(0, target.x - width) : right
    }

    function followY() {
        return target ? target.y : 0
    }

    x: followX()
    y: followY()

    // 背景拖动区：按住空白处拖动，等于拖动投屏窗，工具栏靠绑定自动跟过去
    MouseArea {
        id: dragArea

        anchors.fill: parent
        property point pressGlobal
        property point pressWindow

        onPressed: {
            pressGlobal = dragArea.mapToGlobal(mouse.x, mouse.y)
            pressWindow = toolWindow.target ? Qt.point(toolWindow.target.x, toolWindow.target.y)
                                            : Qt.point(0, 0)
        }
        onPositionChanged: {
            if (!pressed || !toolWindow.target) {
                return
            }
            const current = dragArea.mapToGlobal(mouse.x, mouse.y)
            toolWindow.target.x = pressWindow.x + (current.x - pressGlobal.x)
            toolWindow.target.y = pressWindow.y + (current.y - pressGlobal.y)
        }
    }

    // 一个按钮：文字 + 选中态配色 + 点击回调
    component ToolButton: Rectangle {
        id: button

        property string label: ""
        property string tip: ""    // 鼠标悬停时显示的说明
        property bool active: false
        signal clicked()

        width: parent.width
        height: 32
        radius: 3
        color: area.containsMouse ? "#2f2f2f" : (active ? "#2b4a6f" : "transparent")

        Text {
            anchors.centerIn: parent
            text: button.label
            color: button.active ? "#ffffff" : "#c8c8c8"
            font.pixelSize: 13
            font.bold: true
        }

        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            onClicked: button.clicked()
        }

        ToolTip.visible: area.containsMouse && button.tip.length > 0
        ToolTip.text: button.tip
        ToolTip.delay: 400
    }

    Column {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 4


        ToolButton {
            label: qsTr("返回")
            tip: qsTr("返回上一级（设备息屏时还会点亮屏幕）")
            onClicked: deviceController.SendBack()
        }

        ToolButton {
            label: qsTr("任务")
            tip: qsTr("打开最近任务列表（相当于手机的方框键）")
            onClicked: deviceController.SendRecents()
        }

        ToolButton {
            label: deviceController.wheelMode === "drag"
                   ? qsTr("滑动 %1").arg(deviceController.dragPixels) : qsTr("滚动")
            active: deviceController.wheelMode === "drag"
            tip: deviceController.wheelMode === "drag"
                 ? qsTr("滚轮当前是滑动模式（模拟手指拖动），点一下切回滚动事件")
                 : qsTr("滚轮当前是滚动模式（原生滚动事件），点一下切到模拟滑动")
            onClicked: deviceController.SetWheelMode(deviceController.wheelMode === "drag" ? "scroll" : "drag")
        }

        // 滑动手感：只在滑动模式下有用，范围 10~300 像素
        // 上面一组粗调 ±5，下面一组微调 ±1
        Row {
            width: parent.width
            spacing: 4
            visible: deviceController.wheelMode === "drag"

            ToolButton {
                width: (parent.width - parent.spacing) / 2
                label: qsTr("−5")
                onClicked: deviceController.AdjustDragPixels(-5)
            }

            ToolButton {
                width: (parent.width - parent.spacing) / 2
                label: qsTr("+5")
                onClicked: deviceController.AdjustDragPixels(5)
            }
        }

        Row {
            width: parent.width
            spacing: 4
            visible: deviceController.wheelMode === "drag"

            ToolButton {
                width: (parent.width - parent.spacing) / 2
                label: qsTr("−1")
                onClicked: deviceController.AdjustDragPixels(-1)
            }

            ToolButton {
                width: (parent.width - parent.spacing) / 2
                label: qsTr("+1")
                onClicked: deviceController.AdjustDragPixels(1)
            }
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
            label: qsTr("截图")
            tip: qsTr("保存当前画面到 screenshots 目录，并复制到剪贴板")
            onClicked: deviceController.SaveScreenshot()
        }

        ToolButton {
            label: toolWindow.displayOff ? qsTr("亮屏") : qsTr("息屏")
            tip: qsTr("只关/开屏幕显示，不动锁屏和电源状态")
            active: toolWindow.displayOff
            onClicked: {
                toolWindow.displayOff = !toolWindow.displayOff
                deviceController.SetDisplayPower(!toolWindow.displayOff)
            }
        }

        ToolButton {
            label: qsTr("置顶")
            tip: qsTr("让投屏窗一直显示在其它窗口前面")
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
            label: qsTr("全屏")
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
