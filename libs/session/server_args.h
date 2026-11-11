#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace qsc::session {

// 拉起 android 端 scrcpy-server 需要的参数。
// 字段名尽量和服务器那边的 key=value 参数对齐，方便对着它的文档核对。
struct ServerParams
{
    QString serial_;                     // 设备序列号；只有一台设备时可以为空
    QString server_local_path_;          // 本机 scrcpy-server 文件路径
    QString server_remote_path_ = QStringLiteral("/data/local/tmp/scrcpy-server.jar");
    QString server_version_ = QStringLiteral("3.3.3");
    QString log_level_ = QStringLiteral("debug");  // verbose/debug/info/warn/error
    quint32 bit_rate_ = 8000000;
    quint16 max_size_ = 720;             // 长边压到这个像素；0 表示原始分辨率
    quint32 max_fps_ = 0;                // 0 不限制（仅 Android 10 以上生效）
    int capture_orientation_lock_ = 0;   // 0 不锁，1 锁指定方向，2 锁原始方向
    int capture_orientation_ = 0;        // 0 / 90 / 180 / 270
    QString crop_;                       // "" 或 "宽:高:x:y"
    bool control_ = true;                // 设备端是否接收键鼠控制
    bool stay_awake_ = false;
    QString codec_options_;              // 例如 "profile=1,level=2"
    QString encoder_name_;               // 例如 "OMX.qcom.video.encoder.avc"
    quint32 scid_ = 0xFFFFFFFFu;         // socket 名后缀；0xFFFFFFFF 表示用服务端默认
    bool use_reverse_ = true;            // true：先 reverse，失败再降级 forward
    bool tunnel_forward_ = false;        // 由连接流程决定，不在界面上配
    quint16 local_port_ = 27183;         // 本机监听/转发端口
};

// 拼 adb shell 后面那一串参数（不含 adb 本身，也不含 -s）：
//
//   CLASSPATH=/data/local/tmp/scrcpy-server.jar
//   app_process / com.genymobile.scrcpy.Server 3.3.3
//   log_level=debug video_bit_rate=8000000 max_size=720 capture_orientation=0 audio=false
//
// 原则：能用服务端默认值的一律不传。三星某些机型传参过长会直接
// "stack corruption detected"，减少参数是最省事的规避方式。
class ServerArgs
{
public:
    static QStringList Build(const ServerParams &params);
};

} // namespace qsc::session
