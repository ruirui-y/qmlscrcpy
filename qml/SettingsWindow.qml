import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15

// 设置窗：画质 / 快捷键 / 其他。独立小窗口，主窗只留一个「设置」入口。
// 用独立窗口而不是 Popup，是因为 Popup 画在父母口内部，主窗 250 高塞不下这些内容。
Window
{
    id: settingsWindow

    width: 620
    height: 560
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

    // 码率下拉：预设档位对应"流畅 / 标准 / 清晰 / 高清 / 超清 / 至臻"
    property int currentBitrateMbps: Math.round(deviceController.videoBitRate / 1000000)
    property var bitrateValues: {
        const presets = [4, 8, 12, 16, 20, 24]
        // 配置文件里写的是非预设值（比如 6M）时，把它放到第一项
        return presets.indexOf(currentBitrateMbps) < 0 ? [currentBitrateMbps].concat(presets) : presets
    }
    property var bitrateOptions: {
        const names = [qsTr("流畅"), qsTr("标准"), qsTr("清晰"), qsTr("高清"), qsTr("超清"), qsTr("至臻")]
        const presetLabels = [4, 8, 12, 16, 20, 24].map(function (mbps, i)
        {
            return names[i] + "（" + mbps + " Mbps）"
        })
        return bitrateValues.length > presetLabels.length
            ? [qsTr("当前（%1 Mbps）").arg(currentBitrateMbps)].concat(presetLabels)
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
            currentBitrateMbps = Math.round(deviceController.videoBitRate / 1000000)
            bitrateBox.currentIndex = bitrateIndex(currentBitrateMbps)
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
                        currentIndex = bitrateIndex(settingsWindow.currentBitrateMbps)
                    }

                    onActivated:
                    {
                        settingsWindow.currentBitrateMbps = settingsWindow.bitrateValues[index]
                    }
                }

                Text
                {
                    text: qsTr("每帧约 %1 KB @60fps").arg((settingsWindow.currentBitrateMbps * 1000 / 8 / 60).toFixed(0))
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
                        text: qsTr("应用并重连")
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
                            const mbps = bitrateOptions[bitrateBox.currentIndex].mbps
                            deviceController.ApplyVideoSettings(mbps * 1000000, fps)
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
