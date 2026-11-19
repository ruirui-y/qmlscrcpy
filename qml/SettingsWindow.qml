import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

// 设置窗：画质 / 快捷键 / 其他。独立小窗口，主窗只留一个「设置」入口。
// 用独立窗口而不是 Popup，是因为 Popup 画在父母口内部，主窗 250 高塞不下这些内容。
Window
{
    id: settingsWindow

    width: 620
    height: 580
    minimumWidth: 620
    minimumHeight: 520
    visible: false
    color: "#14161a"
    flags: Qt.Window | Qt.FramelessWindowHint
    // 非模态：模态窗会挡住其它窗口的拖放事件，开着设置就没法往投屏窗拖文件了
    modality: Qt.NonModal
    transientParent: null

    // 登记给老板键：设置窗开着时按老板键也要一起藏起来
    Component.onCompleted: deviceController.RegisterWindow(settingsWindow)

    // 首帧预热：窗口第一次真正显示时，DWM 会先给一帧白底，等 GL 出第一帧才变深色，
    // 看起来像"标题栏白闪一下"。启动后先用透明窗口渲染一次，把这一帧提前消化掉。
    property bool warmingUp: false

    function warmUp()
    {
        if (visible)
        {
            return
        }
        warmingUp = true
        flags = Qt.Window | Qt.FramelessWindowHint | Qt.WindowDoesNotAcceptFocus
        opacity = 0
        visible = true
        warmTimer.start()
    }

    Timer
    {
        interval: 700
        running: true
        repeat: false
        onTriggered: settingsWindow.warmUp()
    }

    Timer
    {
        id: warmTimer
        interval: 150
        repeat: false
        onTriggered:
        {
            settingsWindow.visible = false
            settingsWindow.opacity = 1
            settingsWindow.flags = Qt.Window | Qt.FramelessWindowHint
            settingsWindow.warmingUp = false
        }
    }

    // 码率：appliedMbps 是"正在用的"，selectedMbps 是"下拉里选的"，两者不同说明还没应用
    property int appliedMbps: Math.round(deviceController.videoBitRate / 1000000)
    property int selectedMbps: appliedMbps
    property bool bitrateDirty: selectedMbps !== appliedMbps

    property var bitrateValues: {
        const presets = [4, 8, 12, 16, 20, 24]
        // 正在用的值不是预设（比如 6M）时，把它放到第一项
        return presets.indexOf(appliedMbps) < 0 ? [appliedMbps].concat(presets) : presets
    }
    property var bitrateOptions: {
        const names = [qsTr("流畅"), qsTr("标准"), qsTr("清晰"), qsTr("高清"), qsTr("超清"), qsTr("至臻")]
        const presetLabels = [4, 8, 12, 16, 20, 24].map(function (mbps, i)
        {
            return names[i] + "（" + mbps + " Mbps）"
        })
        return bitrateValues.length > presetLabels.length
            ? [qsTr("当前（%1 Mbps）").arg(appliedMbps)].concat(presetLabels)
            : presetLabels
    }

    // 按 Mbps 找下拉里的位置
    function bitrateIndex(mbps)
    {
        const index = bitrateValues.indexOf(mbps)
        return index >= 0 ? index : 0
    }

    // 每次打开设置窗，按当前实际码率重新选一次（应用并重连之后值会变）
    onVisibleChanged:
    {
        if (visible)
        {
            appliedMbps = Math.round(deviceController.videoBitRate / 1000000)
            selectedMbps = appliedMbps
            bitrateBox.currentIndex = bitrateIndex(selectedMbps)
        }
    }

    // Esc 直接关掉设置窗（模态窗被挡住时也能退出来）
    Shortcut
    {
        sequence: "Esc"
        onActivated: settingsWindow.visible = false
    }

    // 一个分组标题
    component SectionTitle: Text
    {
        color: "#7c8698"
        font.pixelSize: 11
        font.bold: true
    }

    // 一行：左边名字，右边输入框；外部用 fieldText 读写
    component ShortcutRow: RowLayout
    {
        id: shortcutRow

        property string label: ""
        property string value: ""
        property alias fieldText: field.text

        Layout.fillWidth: true
        spacing: 8

        Text
        {
            Layout.preferredWidth: 80
            text: parent.label
            color: "#cdd4de"
            font.pixelSize: 12
        }

        TextField
        {
            id: field

            Layout.fillWidth: true
            Layout.preferredHeight: 28
            text: shortcutRow.value
            color: "#d8dee9"
            font.pixelSize: 12
            leftPadding: 10
            selectByMouse: true
            placeholderText: qsTr("留空表示禁用")

            background: Rectangle
            {
                radius: 6
                color: "#151920"
                border.width: 1
                border.color: field.activeFocus ? "#3f8fdd" : "#2c323c"
            }
        }
    }

    // 标题栏
    Rectangle
    {
        id: titleBar

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 32
        color: "#1c1c1c"

        // 拖动区必须最先声明：QML 里后声明的兄弟节点在上层，写在后头会盖住关闭按钮
        MouseArea
        {
            anchors.fill: parent
            property point pressGlobal
            property point pressWindow

            onPressed:
            {
                pressGlobal = mapToGlobal(mouse.x, mouse.y)
                pressWindow = Qt.point(settingsWindow.x, settingsWindow.y)
            }
            onPositionChanged:
            {
                if (!pressed)
                {
                    return
                }
                const current = mapToGlobal(mouse.x, mouse.y)
                settingsWindow.x = pressWindow.x + (current.x - pressGlobal.x)
                settingsWindow.y = pressWindow.y + (current.y - pressGlobal.y)
            }
        }

        Text
        {
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("设置")
            color: "#e6ebf2"
            font.pixelSize: 14
            font.bold: true
        }

        Rectangle
        {
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            width: 24
            height: 22
            color: closeArea.containsMouse ? "#8c2b2b" : "transparent"

            Text
            {
                anchors.centerIn: parent
                text: "✕"
                color: "#c8c8c8"
                font.pixelSize: 12
            }

            MouseArea
            {
                id: closeArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: settingsWindow.visible = false
            }
        }

    }

    Flickable
    {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: titleBar.bottom
        anchors.bottom: parent.bottom
        contentHeight: body.implicitHeight + 32
        clip: true

        ColumnLayout
        {
            id: body

            x: 18
            y: 16
            width: settingsWindow.width - 36
            spacing: 12

            // ── 画质 ──
            SectionTitle
            {
                text: qsTr("画质（改完自动重连）")
            }

            RowLayout
            {
                Layout.fillWidth: true
                spacing: 8

                Text
                {
                    text: qsTr("帧率")
                    color: "#cdd4de"
                    font.pixelSize: 12
                }
                TextField
                {
                    id: fpsField
                    Layout.preferredWidth: 70
                    Layout.preferredHeight: 28
                    text: String(deviceController.videoMaxFps)
                    color: "#d8dee9"
                    font.pixelSize: 12
                    leftPadding: 8
                    validator: IntValidator
                    {
                        bottom: 0
                        top: 240
                    }
                    background: Rectangle
                    {
                        radius: 6
                        color: "#151920"
                        border.width: 1
                        border.color: "#2c323c"
                    }
                }

                Text
                {
                    text: qsTr("码率")
                    color: "#cdd4de"
                    font.pixelSize: 12
                }
                ComboBox
                {
                    id: bitrateBox
                    Layout.preferredWidth: 190
                    Layout.preferredHeight: 28
                    font.pixelSize: 12
                    model: bitrateOptions

                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("码率越高越清晰；无线下太高会受带宽限制")

                    // 自绘：默认样式是系统原生白底，和整套深色主题不搭
                    background: Rectangle
                    {
                        radius: 6
                        color: "#151920"
                        border.width: 1
                        border.color: bitrateBox.activeFocus ? "#3f8fdd" : "#2c323c"
                    }

                    contentItem: Text
                    {
                        leftPadding: 8
                        text: bitrateBox.displayText
                        color: "#d8dee9"
                        font: bitrateBox.font
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }

                    indicator: Text
                    {
                        x: bitrateBox.width - width - 8
                        y: (bitrateBox.height - height) / 2
                        text: "▾"
                        color: "#7c8698"
                        font.pixelSize: 12
                    }

                    delegate: ItemDelegate
                    {
                        width: bitrateBox.width
                        height: 28
                        highlighted: bitrateBox.highlightedIndex === index

                        contentItem: Text
                        {
                            leftPadding: 8
                            text: modelData
                            color: highlighted ? "#ffffff" : "#cdd4de"
                            font: bitrateBox.font
                            verticalAlignment: Text.AlignVCenter
                        }

                        background: Rectangle
                        {
                            color: highlighted ? "#2a3442" : "transparent"
                        }
                    }

                    popup: Popup
                    {
                        width: bitrateBox.width
                        y: bitrateBox.height
                        implicitHeight: contentItem.implicitHeight + 2
                        padding: 1

                        background: Rectangle
                        {
                            radius: 6
                            color: "#1a1f27"
                            border.width: 1
                            border.color: "#2c323c"
                        }

                        contentItem: ListView
                        {
                            clip: true
                            implicitHeight: contentHeight
                            model: bitrateBox.popup.visible ? bitrateBox.delegateModel : null
                            currentIndex: bitrateBox.highlightedIndex
                        }
                    }

                    Component.onCompleted:
                    {
                        currentIndex = bitrateIndex(settingsWindow.selectedMbps)
                    }

                    onActivated:
                    {
                        settingsWindow.selectedMbps = settingsWindow.bitrateValues[index]
                    }
                }

                Text
                {
                    text: qsTr("每帧约 %1 KB @60fps").arg((settingsWindow.selectedMbps * 1000 / 8 / 60).toFixed(0))
                    color: "#7c8698"
                    font.pixelSize: 11
                }
                Item
                {
                    Layout.fillWidth: true
                }

                Rectangle
                {
                    Layout.preferredWidth: 96
                    Layout.preferredHeight: 28
                    radius: 6
                    color: applyArea.containsMouse ? "#5aa9ff" : "#3f8fdd"

                    Text
                    {
                        anchors.centerIn: parent
                        text: settingsWindow.bitrateDirty ? qsTr("应用并重连 *") : qsTr("应用并重连")
                        color: "#ffffff"
                        font.pixelSize: 12
                    }

                    MouseArea
                    {
                        id: applyArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked:
                        {
                            const fps = parseInt(fpsField.text) || 0
                            deviceController.ApplyVideoSettings(settingsWindow.selectedMbps * 1000000, fps)
                            settingsWindow.appliedMbps = settingsWindow.selectedMbps
                        }
                    }
                }
            }

            // 硬解读回方式：同步更跟手，异步不用等 GPU 但画面晚一帧
            RowLayout
            {
                Layout.fillWidth: true
                spacing: 8

                Text
                {
                    text: qsTr("读回方式")
                    color: "#cdd4de"
                    font.pixelSize: 12
                }

                ComboBox
                {
                    id: readbackBox
                    Layout.preferredWidth: 190
                    Layout.preferredHeight: 28
                    font.pixelSize: 12
                    model: [qsTr("同步（更跟手）"), qsTr("异步（快，+1 帧）")]
                    currentIndex: deviceController.asyncReadback ? 1 : 0

                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("同步：每次等 GPU 约 2ms，画面不加延迟；异步：CPU 不等，但画面晚一帧")

                    background: Rectangle
                    {
                        radius: 6
                        color: "#151920"
                        border.width: 1
                        border.color: readbackBox.activeFocus ? "#3f8fdd" : "#2c323c"
                    }

                    contentItem: Text
                    {
                        leftPadding: 8
                        text: readbackBox.displayText
                        color: "#d8dee9"
                        font: readbackBox.font
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }

                    indicator: Text
                    {
                        x: readbackBox.width - width - 8
                        y: (readbackBox.height - height) / 2
                        text: "▾"
                        color: "#7c8698"
                        font.pixelSize: 12
                    }


                    delegate: ItemDelegate
                    {
                        width: readbackBox.width
                        height: 28
                        highlighted: readbackBox.highlightedIndex === index

                        contentItem: Text
                        {
                            leftPadding: 8
                            text: modelData
                            color: highlighted ? "#ffffff" : "#cdd4de"
                            font: readbackBox.font
                            verticalAlignment: Text.AlignVCenter
                        }

                        background: Rectangle
                        {
                            color: highlighted ? "#2a3442" : "transparent"
                        }
                    }

                    popup: Popup
                    {
                        width: readbackBox.width
                        y: readbackBox.height
                        implicitHeight: contentItem.implicitHeight + 2
                        padding: 1

                        background: Rectangle
                        {
                            radius: 6
                            color: "#1a1f27"
                            border.width: 1
                            border.color: "#2c323c"
                        }

                        contentItem: ListView
                        {
                            clip: true
                            implicitHeight: contentHeight
                            model: readbackBox.popup.visible ? readbackBox.delegateModel : null
                            currentIndex: readbackBox.highlightedIndex
                        }
                    }

                    onActivated: deviceController.AsyncReadback = (index === 1)
                }

                Text
                {
                    text: qsTr("改完自动重连")
                    color: "#7c8698"
                    font.pixelSize: 11
                }

                Item
                {
                    Layout.fillWidth: true
                }
            }

            Rectangle
            {
                Layout.fillWidth: true
                height: 1
                color: "#262c36"
            }

            // 解码：硬解（GPU）还是软解（CPU），软解时线程数可调
            RowLayout
            {
                Layout.fillWidth: true
                spacing: 8

                Text
                {
                    text: qsTr("解码方式")
                    color: "#cdd4de"
                    font.pixelSize: 12
                }

                ComboBox
                {
                    id: decodeBox
                    Layout.preferredWidth: 120
                    Layout.preferredHeight: 28
                    font.pixelSize: 12
                    model: [qsTr("硬解（GPU）"), qsTr("软解（CPU）")]
                    currentIndex: deviceController.decodeHardware ? 0 : 1

                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("硬解：GPU 解码后拷回内存，跟手；软解：CPU 解码，占用低但画面滞后 2~3 帧")

                    background: Rectangle
                    {
                        radius: 6
                        color: "#151920"
                        border.width: 1
                        border.color: decodeBox.activeFocus ? "#3f8fdd" : "#2c323c"
                    }

                    contentItem: Text
                    {
                        leftPadding: 8
                        text: decodeBox.displayText
                        color: "#d8dee9"
                        font: decodeBox.font
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }

                    indicator: Text
                    {
                        x: decodeBox.width - width - 8
                        y: (decodeBox.height - height) / 2
                        text: "▾"
                        color: "#7c8698"
                        font.pixelSize: 12
                    }

                    delegate: ItemDelegate
                    {
                        width: decodeBox.width
                        height: 28
                        highlighted: decodeBox.highlightedIndex === index

                        contentItem: Text
                        {
                            leftPadding: 8
                            text: modelData
                            color: highlighted ? "#ffffff" : "#cdd4de"
                            font: decodeBox.font
                            verticalAlignment: Text.AlignVCenter
                        }

                        background: Rectangle
                        {
                            color: highlighted ? "#2a3442" : "transparent"
                        }
                    }

                    popup: Popup
                    {
                        width: decodeBox.width
                        y: decodeBox.height
                        implicitHeight: contentItem.implicitHeight + 2
                        padding: 1

                        background: Rectangle
                        {
                            radius: 6
                            color: "#1a1f27"
                            border.width: 1
                            border.color: "#2c323c"
                        }

                        contentItem: ListView
                        {
                            clip: true
                            implicitHeight: contentHeight
                            model: decodeBox.popup.visible ? decodeBox.delegateModel : null
                            currentIndex: decodeBox.highlightedIndex
                        }
                    }

                    onActivated: deviceController.decodeHardware = (index === 0)
                }

                Text
                {
                    text: qsTr("软解线程")
                    color: deviceController.decodeHardware ? "#5c6675" : "#cdd4de"
                    font.pixelSize: 12
                }

                TextField
                {
                    id: threadField
                    Layout.preferredWidth: 60
                    Layout.preferredHeight: 28
                    enabled: !deviceController.decodeHardware
                    text: String(deviceController.decodeThreads)
                    color: enabled ? "#d8dee9" : "#5c6675"
                    font.pixelSize: 12
                    leftPadding: 8
                    horizontalAlignment: TextInput.AlignHCenter
                    validator: IntValidator
                    {
                        bottom: 1
                        top: 8
                    }
                    background: Rectangle
                    {
                        radius: 6
                        color: "#151920"
                        border.width: 1
                        border.color: threadField.activeFocus ? "#3f8fdd" : "#2c323c"
                    }

                    onEditingFinished:
                    {
                        const value = parseInt(text) || deviceController.decodeThreads
                        deviceController.decodeThreads = value
                        text = String(deviceController.decodeThreads)
                    }
                }

                Text
                {
                    text: qsTr("1~8，4 是默认值；少了解码慢，多了攒帧")
                    color: "#7c8698"
                    font.pixelSize: 11
                }

                Item
                {
                    Layout.fillWidth: true
                }
            }


            // ── 快捷键 ──
            SectionTitle
            {
                text: qsTr("快捷键（留空表示禁用；老板键是全局热键）")
            }

            ShortcutRow
            {
                id: backRow
                label: qsTr("返回")
                value: deviceController.shortcutBack
            }
            ShortcutRow
            {
                id: homeRow
                label: qsTr("回桌面")
                value: deviceController.shortcutHome
            }
            ShortcutRow
            {
                id: screenshotRow
                label: qsTr("截图")
                value: deviceController.shortcutScreenshot
            }
            ShortcutRow
            {
                id: fullscreenRow
                label: qsTr("全屏")
                value: deviceController.shortcutFullscreen
            }
            ShortcutRow
            {
                id: bossKeyRow
                label: qsTr("老板键")
                value: deviceController.bossKey
            }

            RowLayout
            {
                Layout.fillWidth: true
                spacing: 8

                Item
                {
                    Layout.fillWidth: true
                }

                Rectangle
                {
                    Layout.preferredWidth: 96
                    Layout.preferredHeight: 28
                    radius: 6
                    color: saveKeysArea.containsMouse ? "#5aa9ff" : "#3f8fdd"

                    Text
                    {
                        anchors.centerIn: parent
                        text: qsTr("保存快捷键")
                        color: "#ffffff"
                        font.pixelSize: 12
                    }

                    MouseArea
                    {
                        id: saveKeysArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked:
                        {
                            deviceController.SetShortcut("back", backRow.fieldText)
                            deviceController.SetShortcut("home", homeRow.fieldText)
                            deviceController.SetShortcut("screenshot", screenshotRow.fieldText)
                            deviceController.SetShortcut("fullscreen", fullscreenRow.fieldText)
                            deviceController.SetBossKey(bossKeyRow.fieldText)
                        }
                    }
                }
            }

            Rectangle
            {
                Layout.fillWidth: true
                height: 1
                color: "#262c36"
            }

            // ── 其他 ──
            SectionTitle
            {
                text: qsTr("文件上传目录（拖文件进投屏窗时用）")
            }

            RowLayout
            {
                Layout.fillWidth: true
                spacing: 8

                TextField
                {
                    id: uploadDirField
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    text: deviceController.uploadDir
                    color: "#d8dee9"
                    font.pixelSize: 12
                    leftPadding: 10
                    selectByMouse: true
                    placeholderText: "/sdcard/Download/QmlScrcpy"

                    background: Rectangle
                    {
                        radius: 6
                        color: "#151920"
                        border.width: 1
                        border.color: uploadDirField.activeFocus ? "#3f8fdd" : "#2c323c"
                    }
                }

                Rectangle
                {
                    Layout.preferredWidth: 72
                    Layout.preferredHeight: 28
                    radius: 6
                    color: saveDirArea.containsMouse ? "#5aa9ff" : "#3f8fdd"

                    Text
                    {
                        anchors.centerIn: parent
                        text: qsTr("保存目录")
                        color: "#ffffff"
                        font.pixelSize: 12
                    }

                    MouseArea
                    {
                        id: saveDirArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: deviceController.SetUploadDir(uploadDirField.text)
                    }
                }

                Rectangle
                {
                    Layout.preferredWidth: 72
                    Layout.preferredHeight: 28
                    radius: 6
                    color: openDirArea.containsMouse ? "#2e343e" : "#242932"
                    border.width: 1
                    border.color: "#333945"

                    Text
                    {
                        anchors.centerIn: parent
                        text: qsTr("打开首页")
                        color: "#cdd4de"
                        font.pixelSize: 12
                    }

                    MouseArea
                    {
                        id: openDirArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: videoController.OpenUploadDir()
                    }
                }
            }

            Rectangle
            {
                Layout.fillWidth: true
                height: 1
                color: "#262c36"
            }

            SectionTitle
            {
                text: qsTr("其他")
            }

            RowLayout
            {
                Layout.fillWidth: true
                spacing: 8

                Rectangle
                {
                    Layout.preferredWidth: 110
                    Layout.preferredHeight: 28
                    radius: 6
                    color: consoleArea.containsMouse ? "#2e343e" : "#242932"
                    border.width: 1
                    border.color: "#333945"

                    Text
                    {
                        anchors.centerIn: parent
                        text: deviceController.showConsole ? qsTr("命令行：开") : qsTr("命令行：关")
                        color: "#cdd4de"
                        font.pixelSize: 12
                    }

                    MouseArea
                    {
                        id: consoleArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: deviceController.SetShowConsole(!deviceController.showConsole)
                    }
                }

                Rectangle
                {
                    Layout.preferredWidth: 120
                    Layout.preferredHeight: 28
                    radius: 6
                    color: resetArea.containsMouse ? "#3a2b2b" : "#242932"
                    border.width: 1
                    border.color: "#4a3434"

                    Text
                    {
                        anchors.centerIn: parent
                        text: qsTr("恢复默认配置")
                        color: "#e0b4b4"
                        font.pixelSize: 12
                    }

                    MouseArea
                    {
                        id: resetArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: deviceController.ResetAllConfig()
                    }
                }

                Item
                {
                    Layout.fillWidth: true
                }
            }

            Text
            {
                Layout.fillWidth: true
                text: qsTr("恢复默认配置会清空窗口位置、上次设备、画质、快捷键，重启后生效。")
                color: "#6b7280"
                font.pixelSize: 10
                wrapMode: Text.WordWrap
            }
        }
    }
}
