import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import QmlScrcpy 1.0

// 投屏窗：一扇窗口里水平排两块 —— 左边是"视频窗（标题栏 + 画面）"，右边是工具栏列。
//
// 为什么不拆成两个窗口：拆开时位置要绑定跟随、最小化要手动同步、transientParent 还会把两扇窗
// 绑成 owner 关系（主窗最小化把它们一起藏掉）。放在同一扇窗口里，这些全靠布局保证。
//
// 尺寸按"横屏 / 竖屏"分开记：横屏看电影、竖屏刷手机，用户对两种姿势的窗口大小要求不一样。
Window {
    id: videoWindow

    property alias fps: videoSurface.fps
    property alias frameSize: videoSurface.frameSize
    property bool applying: false    // 正在由代码改窗口，此时的 onXxxChanged 不记录
    property bool ready: false    // 首帧处理完之前不记尺寸
    property bool landscape: false    // 当前帧是不是横向
    property bool pinned: false    // 置顶
    property bool displayOff: false    // 只用来切按钮文字，真实状态在设备那边
    property string hint: ""    // 工具栏按钮悬停时的说明，显示在标题栏

    readonly property int toolBarWidth: 64    // 右侧工具栏列宽

    width: 460
    height: 900
    minimumWidth: toolBarWidth + 200
    minimumHeight: 268
    // 启动时只显示主窗；连上会话才把投屏窗亮出来，断开就收起
    visible: false
    // 标题显示当前设备：有线是序列号，无线是 IP:端口
    title: deviceController.deviceName.length > 0
           ? "QmlScrcpy — " + deviceController.deviceName : qsTr("QmlScrcpy — 投屏")
    color: "#000000"
    flags: Qt.Window | Qt.FramelessWindowHint | (pinned ? Qt.WindowStaysOnTopHint : Qt.NoHint)

    // 横屏和竖屏各记一套几何和位置
    function geometryKey() {
        return landscape ? "video_window_landscape" : "video_window"
    }

    function positionKey() {
        return landscape ? "video_pos_landscape" : "video_pos"
    }

    // 一个按钮：文字 + 选中态配色 + 悬停提示 + 点击回调
    component ToolButton: Rectangle {
        id: button

        property string label: ""
        property string tip: ""    // 悬停时显示在标题栏的说明
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
            cursorShape: Qt.PointingHandCursor
            onClicked: button.clicked()
            onEntered: videoWindow.hint = button.tip
            onExited: videoWindow.hint = ""
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // 视频窗：标题栏 + 画面，竖向排
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                id: titleBar

                Layout.fillWidth: true
                Layout.preferredHeight: 30
                color: "#1c1c1c"

                // 拖动：按屏幕绝对坐标算位移，窗口自己移动不会污染计算
                MouseArea {
                    id: dragArea

                    anchors.fill: parent
                    cursorShape: Qt.SizeAllCursor
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

                // 默认显示设备名 + 帧率 + 分辨率；有悬停提示时临时顶替
                Label {
                    anchors.left: parent.left
                    anchors.leftMargin: 22    // 让开左上角那个缩放把手
                    anchors.right: windowButtons.left
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    text: {
                        let text = deviceController.deviceName.length > 0
                                ? deviceController.deviceName : qsTr("未连接")
                        if (videoWindow.fps > 0) {
                            text += "   " + videoWindow.fps + " fps"
                        }
                        if (videoWindow.frameSize.width > 0) {
                            text += "   " + videoWindow.frameSize.width + "x" + videoWindow.frameSize.height
                        }
                        return text
                    }
                    color: "#d0d6de"
                    font.pixelSize: 14
                    font.bold: true
                }

                Row {
                    id: windowButtons

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
                            cursorShape: Qt.PointingHandCursor
                            onClicked: videoWindow.showMinimized()
                        }
                    }

                    // 关闭：只关这扇窗（主窗还在，进程不退出）
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
                            cursorShape: Qt.PointingHandCursor
                            // 关窗就等于断开：只藏窗口的话主窗还显示"已连接"，再点连接也叫不回来
                            onClicked: {
                                if (deviceController.connected) {
                                    deviceController.Disconnect()
                                }
                                videoWindow.visible = false
                            }
                        }
                    }
                }
            }

            // 画面 + 右下角缩放把手放同一个 Item：把手坐标不依赖工具栏，不会算错到左边
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                VideoSurface {
                    id: videoSurface
                    objectName: "videoSurface"
                    anchors.fill: parent
                    keepAspect: false    // 窗口比例已经跟着帧走了，这里直接拉满
                }

                // 文件拖放：把文件拖进画面就推到手机 Download/，拖动时给个高亮提示
    DropArea {
        id: fileDrop

        anchors.fill: parent

        // 拖动时只画边框 + 一行提示，不铺整块半透明底：整块混合会让拖拽过程明显掉帧
        Rectangle {
            anchors.fill: parent
            color: "transparent"
            visible: fileDrop.containsDrag
            border.width: 2
            border.color: "#5aa9ff"

            Rectangle {
                anchors.centerIn: parent
                width: hintText.implicitWidth + 24
                height: 34
                radius: 6
                color: "#0f2033"
                border.width: 1
                border.color: "#5aa9ff"

                Text {
                    id: hintText
                    anchors.centerIn: parent
                    text: qsTr("松手传到手机 Download/")
                    color: "#dce9f8"
                    font.pixelSize: 13
                }
            }
        }

        onDropped: {
            if (drop.hasUrls) {
                deviceController.PushFiles(drop.urls)
            }
        }
    }

    // 右下角拖拽改尺寸：底色 + 三条 2px 斜线，一眼能看见
                Rectangle {
                    id: resizeGrip

                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    width: 18
                    height: 18
                    color: "#2a2a2a"

                    Repeater {
                        model: 3

                        Rectangle {
                            width: 13
                            height: 2
                            color: "#b8b8b8"
                            rotation: -45
                            x: 3 + index * 4
                            y: 12 - index * 4
                        }
                    }
                }

                MouseArea {
                    id: resizeArea

                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    width: 18
                    height: 18
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
            }
        }

        // 视频区和工具栏之间的分隔线，全屏收起工具栏时一起隐藏
        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            color: "#2a2a2a"
        }

        // 工具栏：全屏时收起来，别挡画面
        Rectangle {
            id: toolBar

            Layout.preferredWidth: videoWindow.toolBarWidth
            Layout.fillHeight: true
            color: "#1c1c1c"

            // 按钮之间的空地也能拖窗口（按钮自己的 MouseArea 会先吃掉点击）
            MouseArea {
                id: toolBarDragArea

                anchors.fill: parent
                cursorShape: Qt.SizeAllCursor
                property point pressGlobal
                property point pressWindow

                onPressed: {
                    pressGlobal = toolBarDragArea.mapToGlobal(mouse.x, mouse.y)
                    pressWindow = Qt.point(videoWindow.x, videoWindow.y)
                }
                onPositionChanged: {
                    if (!pressed) {
                        return
                    }
                    const current = toolBarDragArea.mapToGlobal(mouse.x, mouse.y)
                    videoWindow.x = pressWindow.x + (current.x - pressGlobal.x)
                    videoWindow.y = pressWindow.y + (current.y - pressGlobal.y)
                }
            }

            // 按钮多、横屏窗口又只有 400 高，包一层 Flickable 才不会把下面的按钮裁掉
            Flickable {
                anchors.fill: parent
                anchors.margins: 4
                // 顶部 30px 留给标题栏的悬停提示条，不然第一个按钮（返回）会被挡住
                anchors.topMargin: 34
                contentWidth: width
                contentHeight: toolColumn.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                Column {
                    id: toolColumn

                    width: parent.width
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
                    label: qsTr("桌面")
                    tip: qsTr("回到手机桌面（相当于手机的 Home 键）")
                    onClicked: deviceController.SendHome()
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

                // 滑动手感：只在滑动模式下有用，上面一组粗调 ±5，下面一组微调 ±1
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
                    tip: qsTr("设备音量加一档")
                    onClicked: deviceController.SendVolumeUp()
                }

                ToolButton {
                    label: qsTr("音量 −")
                    tip: qsTr("设备音量减一档")
                    onClicked: deviceController.SendVolumeDown()
                }

                ToolButton {
                    label: qsTr("截图")
                    tip: qsTr("保存当前画面到 screenshots 目录，并复制到剪贴板")
                    onClicked: deviceController.SaveScreenshot()
                }

                ToolButton {
                    label: qsTr("发给手机")
                    tip: qsTr("把电脑剪贴板里的文字写进手机剪贴板，并自动粘贴到当前输入框")
                    onClicked: deviceController.SendClipboardToDevice()
                }

                ToolButton {
                    label: qsTr("取回电脑")
                    tip: qsTr("读取手机剪贴板，写进电脑剪贴板")
                    onClicked: deviceController.PullClipboardFromDevice()
                }

                ToolButton {
                    label: qsTr("目录")
                    tip: qsTr("在手机上打开文件传输的目录（Download/QmlScrcpy）")
                    onClicked: deviceController.OpenUploadDir()
                }

                ToolButton {
                    label: videoWindow.displayOff ? qsTr("亮屏") : qsTr("息屏")
                    tip: qsTr("只关/开屏幕显示，不动锁屏和电源状态")
                    active: videoWindow.displayOff
                    onClicked: {
                        videoWindow.displayOff = !videoWindow.displayOff
                        deviceController.SetDisplayPower(!videoWindow.displayOff)
                    }
                }

                ToolButton {
                    label: qsTr("电源")
                    tip: qsTr("按一下手机电源键：锁屏或点亮屏幕（「息屏」只关显示，不锁屏）")
                    onClicked: deviceController.SendPower()
                }

                ToolButton {
                    label: qsTr("置顶")
                    tip: qsTr("让投屏窗一直显示在其它窗口前面")
                    active: videoWindow.pinned
                    onClicked: videoWindow.pinned = !videoWindow.pinned
                }

                ToolButton {
                    label: videoWindow.visibility === Window.FullScreen ? qsTr("还原") : qsTr("全屏")
                    tip: qsTr("投屏窗全屏／还原")
                    active: videoWindow.visibility === Window.FullScreen
                    onClicked: videoWindow.visibility = videoWindow.visibility === Window.FullScreen
                                                  ? Window.Windowed : Window.FullScreen
                }

                ToolButton {
                    label: qsTr("复位")
                    tip: qsTr("把窗口尺寸恢复成默认（横屏 900x400，竖屏按画面比例）")
                    onClicked: videoWindow.resetToDefault()
                }
            }
            }
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

    // 悬停提示条：整窗宽盖在标题栏上，标题栏自己塞不下长句
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 30
        color: "#12233a"
        visible: videoWindow.hint.length > 0
        z: 10

        Text {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            verticalAlignment: Text.AlignVCenter
            text: videoWindow.hint
            color: "#9cc4ff"
            font.pixelSize: 13
            elide: Text.ElideRight
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
        // 主窗最小化不该带走这扇窗：Qt 的 componentComplete 会把 transientParent 设回父窗口，这里清掉
        transientParent = null

        // QML 加载时可能已经连上了（自启动连上次设备），按当前状态决定露不露脸
        visible = deviceController.connected
        deviceController.RegisterWindow(videoWindow)
        deviceController.SetSurface(videoSurface)

        applying = true
        const position = appSettings.VideoPosition("video_pos")
        if (position.x >= 0 && position.y >= 0) {
            x = position.x
            y = position.y
        }
        applying = false
    }

    // 用系统方式关窗（Alt+F4）也要断开，保持"没有窗口 = 没有连接"
    onClosing: {
        if (deviceController.connected) {
            deviceController.Disconnect()
        }
    }

    // 登记给老板键：设置窗开着时按老板键也要一起藏起来    // 窗口级快捷键：先于画面里的按键处理，所以这几个组合不会再发给设备。
    // 组合来自设置窗，留空表示禁用（binding 会取到空串，Shortcut 自动失效）
    Shortcut {
        sequence: deviceController.shortcutBack
        enabled: deviceController.shortcutBack.length > 0
        onActivated: deviceController.SendBack()
    }

    Shortcut {
        sequence: deviceController.shortcutHome
        enabled: deviceController.shortcutHome.length > 0
        onActivated: deviceController.SendHome()
    }

    Shortcut {
        sequence: deviceController.shortcutScreenshot
        enabled: deviceController.shortcutScreenshot.length > 0
        onActivated: deviceController.SaveScreenshot()
    }

    Shortcut {
        sequence: deviceController.shortcutFullscreen
        enabled: deviceController.shortcutFullscreen.length > 0
        onActivated: videoWindow.visibility = videoWindow.visibility === Window.FullScreen
                                      ? Window.Windowed : Window.FullScreen
    }

    // 会话连通状态一变就跟着显示/收起；用户手动关窗（visible=false）不会被强制拉回来，
    // 下次连接才重新出现
    Connections {
        target: deviceController

        function onConnectedChanged() {
            videoWindow.visible = deviceController.connected
        }
    }

    // 按当前帧比例把窗口调成"刚好装下画面 + 工具栏"
    function fitToFrame() {
        if (frameSize.width <= 0) {
            return
        }
        const ratio = Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1
        const frameRatio = frameSize.height / frameSize.width
        const maxWidth = Screen.desktopAvailableWidth * 0.9
        const maxHeight = Screen.desktopAvailableHeight * 0.85 - titleBar.height

        let wantWidth = frameSize.width / ratio + toolBarWidth
        let wantHeight = (wantWidth - toolBarWidth) * frameRatio
        if (wantHeight > maxHeight) {
            wantHeight = maxHeight
            wantWidth = wantHeight / frameRatio + toolBarWidth
        }
        if (wantWidth > maxWidth) {
            wantWidth = maxWidth
            wantHeight = (wantWidth - toolBarWidth) * frameRatio
        }

        applying = true
        width = Math.max(minimumWidth, Math.round(wantWidth))
        height = Math.max(minimumHeight, Math.round(wantHeight) + titleBar.height)
        applying = false
    }

    // 复位：把这个姿势的尺寸记录删掉，立刻套回默认值（横屏 900x400，竖屏按帧比例）
    function resetToDefault() {
        appSettings.ResetVideoGeometry(geometryKey())

        if (frameSize.width <= 0) {
            return
        }

        applying = true
        if (landscape) {
            width = Math.min(900, Math.round(Screen.desktopAvailableWidth * 0.9))
            height = Math.min(400 + titleBar.height, Math.round(Screen.desktopAvailableHeight * 0.9))
            applying = false
        } else {
            applying = false
            fitToFrame()
        }
        saveTimer.restart()
    }

    // 帧尺寸变化：首次定尺寸；横竖屏翻转时切到那一边记住的尺寸
    onFrameSizeChanged: {
        if (frameSize.width <= 0) {
            return
        }

        const nowLandscape = frameSize.width > frameSize.height
        if (!ready) {
            landscape = nowLandscape
            const saved = appSettings.VideoGeometry(geometryKey())
            if (saved.width > 0) {
                applying = true
                width = Math.max(minimumWidth, saved.width)
                height = Math.max(minimumHeight, saved.height)
                applying = false
            } else if (nowLandscape) {
                // 横屏第一次出现时的默认尺寸：900x400 的画面区，够看清又不占满屏幕
                applying = true
                width = Math.min(900, Math.round(Screen.desktopAvailableWidth * 0.9))
                height = Math.min(400 + titleBar.height, Math.round(Screen.desktopAvailableHeight * 0.9))
                applying = false
            } else {
                fitToFrame()
            }
            ready = true
            // 立刻把自动定好的尺寸记下来，否则下次启动又要重新猜
            saveTimer.restart()
            return
        }

        if (nowLandscape === landscape) {
            return
        }

        // 换姿势：尺寸和位置都先存到旧的那一边，再取新姿势记住的值
        appSettings.SaveVideoGeometry(geometryKey(), x, y, width, height)
        appSettings.SaveVideoPosition(positionKey(), x, y)
        landscape = nowLandscape

        // 位置按姿势分开恢复，横屏和竖屏各回各的地方，不共用左上角
        const saved_position = appSettings.VideoPosition(positionKey())
        if (saved_position.x >= 0 && saved_position.y >= 0) {
            applying = true
            x = saved_position.x
            y = saved_position.y
            applying = false
        }

        const saved = appSettings.VideoGeometry(geometryKey())
        if (saved.width > 0) {
            applying = true
            width = Math.max(minimumWidth, saved.width)
            height = Math.max(minimumHeight, saved.height)
            applying = false
        } else if (nowLandscape) {
            // 第一次进横屏：用默认 900x400，别按帧比例算（那样会撑得很大，用户得手动点复位）
            applying = true
            width = Math.min(900, Math.round(Screen.desktopAvailableWidth * 0.9))
            height = Math.min(400 + titleBar.height, Math.round(Screen.desktopAvailableHeight * 0.9))
            applying = false
        } else {
            fitToFrame()
        }

        // 切过来的这套尺寸也记下来（用户没调过就是自动算的那套）
        saveTimer.restart()
    }

    // 位置和尺寸都在"停手 400ms 之后"才写盘。
    // 以前每个 onXxxChanged 都写一次，拖动时一秒上百次 QSettings::sync()，
    // 落盘把 UI 卡住好几秒，看起来就是"窗口不跟手"。
    Timer {
        id: saveTimer

        interval: 400
        repeat: false
        onTriggered: {
            appSettings.SaveVideoPosition(videoWindow.positionKey(), videoWindow.x, videoWindow.y)
            if (ready) {
                appSettings.SaveVideoGeometry(videoWindow.geometryKey(), videoWindow.x, videoWindow.y,
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
