#pragma once

// 配置项的唯一事实来源：键名、默认值、类型、所属 ini、所在分节、注释。
// 加一个配置只改这里一行 —— 模板、默认值、注释、范围校验全从这张表来，
// 不会再出现「代码默认 0、模板写 2、用户文件里没有」这种三方漂移。
//
// 不放进表里的东西（故意的）：
//   窗口几何 x/y/w/h：那是程序自己写的状态，成组读写，不是用户可手改的配置

namespace qsc::app
{

// 值类型：决定读的时候用哪个 QVariant 转换、写模板时怎么序列化
enum class SettingKind
{
    Int,                                                                                        // 整数（可带范围）
    Double,                                                                                     // 浮点
    Bool,                                                                                       // 写 true / false
    Text,                                                                                       // 字符串
};

inline constexpr int kNoLimit = 2147483647;                                                     // 上下限哨兵：不做钳制

struct SettingEntry
{
    const char *key;                                                                            // 完整键名，形如 "video_wired/max_size"
    const char *fallback;                                                                       // 默认值，统一用字符串存，读时按 kind 解析
    SettingKind kind;                                                                           // 值类型
    const char *section;                                                                        // 写进 ini 的段名
    const char *comment;                                                                        // 段内注释正文；空串表示不写注释
    bool in_window;                                                                             // true = window.ini（程序状态），false = config.ini（用户可改）
    int min_value;                                                                              // Int/Double 下限；kNoLimit 表示不限
    int max_value;                                                                              // Int/Double 上限；kNoLimit 表示不限
};

// 顺序就是写进 config.ini 的顺序；同一个 section 的条目必须连着放，渲染时按段分组
inline constexpr SettingEntry kSettings[] = {
    {"log/level", "info", SettingKind::Text, "log", "日志级别：debug / info / warning / critical", false, kNoLimit, kNoLimit},
    {"log/to_file", "true", SettingKind::Bool, "log", "", false, kNoLimit, kNoLimit},
    {"log/keep_days", "15", SettingKind::Int, "log", "日志保留天数", false, 1, 365},

    {"input/wheel_mode", "scroll", SettingKind::Text, "input",
     "滚轮模式：scroll = 发滚动事件（微信这类原生列表），drag = 模拟手指滑动（阅读器、地图）", false, kNoLimit, kNoLimit},
    {"input/drag_pixels", "60", SettingKind::Int, "input",
     "滑动模式下一格滚轮手指移动多少像素（太小会被当点击，太大一下翻过头）", false, 1, 2000},
    {"input/drag_scale", "1.0", SettingKind::Double, "input", "左键拖拽倍率：1.0 = 鼠标走 1 像素、手指走 1 像素",
     false, kNoLimit, kNoLimit},
    {"input/click_interval", "0", SettingKind::Int, "input", "连点间隔毫秒，0 = 关闭（按住空格连点）", false, 0, 1000},
    {"input/pinch_modifier", "ctrl", SettingKind::Text, "input",
     "捏合修饰键：ctrl / alt / shift / ctrl_alt / none（填 none 关掉捏合）", false, kNoLimit, kNoLimit},
    {"input/clipboard_autosync", "false", SettingKind::Bool, "input",
     "电脑剪贴板一变就推到设备（会同步敏感内容，默认关）", false, kNoLimit, kNoLimit},

    {"ui/boss_key", "Ctrl+Alt+B", SettingKind::Text, "ui", "老板键：一键隐藏/恢复所有窗口，格式 Ctrl+Alt+B", false,
     kNoLimit, kNoLimit},
    {"ui/show_console", "false", SettingKind::Bool, "ui", "控制台开关：true = 显示命令行窗口", false, kNoLimit, kNoLimit},

    {"keys/back", "Esc", SettingKind::Text, "keys", "窗口快捷键（空值表示禁用这个键）", false, kNoLimit, kNoLimit},
    {"keys/home", "Home", SettingKind::Text, "keys", "", false, kNoLimit, kNoLimit},
    {"keys/screenshot", "Ctrl+Shift+S", SettingKind::Text, "keys", "", false, kNoLimit, kNoLimit},
    {"keys/fullscreen", "F11", SettingKind::Text, "keys", "", false, kNoLimit, kNoLimit},

    {"transfer/upload_dir", "/sdcard/Download/QmlScrcpy", SettingKind::Text, "transfer", "拖文件传到设备上的哪个目录",
     false, kNoLimit, kNoLimit},

    {"video_wired/max_size", "0", SettingKind::Int, "video_wired",
     "长边上限，0 = 设备原生分辨率（更清晰，码率也要跟着涨）", false, 0, 4096},
    {"video_wired/bit_rate", "8000000", SettingKind::Int, "video_wired", "码率，单位 bps", false, 100000, 100000000},
    {"video_wired/max_fps", "0", SettingKind::Int, "video_wired",
     "帧率上限：0 = 不限；应用锁 60Hz 时设 120 也只有 58 帧", false, 0, 240},

    {"video_wireless/bit_rate", "6000000", SettingKind::Int, "video_wireless",
     "无线单独一套（串号里带冒号就用这组）：6M 在局域网下够用且不排队", false, 100000, 100000000},
    {"video_wireless/max_fps", "0", SettingKind::Int, "video_wireless", "同上，无线连接的帧率上限；0 = 不限", false,
     0, 240},

    {"decode/hardware", "true", SettingKind::Bool, "decode", "硬解：true = d3d12va 硬解，false = 软解", false,
     kNoLimit, kNoLimit},
    {"decode/threads", "4", SettingKind::Int, "decode", "软解线程数：1~8，只在软解时生效（少了送包阻塞，多了攒帧）",
     false, 1, 8},

    {"update/url", "", SettingKind::Text, "update",
     "版本检查地址：留空走内置源列表（Gitee → jsDelivr → GitHub），一般不用改", false, kNoLimit, kNoLimit},

    {"device/last_serial", "", SettingKind::Text, "device", "", true, kNoLimit, kNoLimit},
};

} // namespace qsc::app
