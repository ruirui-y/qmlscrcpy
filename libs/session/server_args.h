#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace qsc::session {

// 拉起安卓端 scrcpy-server 需要的参数。
// 字段名尽量和服务器那边的 key=value 参数名对齐，方便对着它的文档核对。
struct ServerParams
{
    // 设备序列号；只有一台设备时可以为空
    QString serial_;

    // 本机 scrcpy-server 文件路径；空表示用程序目录下的 scrcpy-server
    QString server_local_path_;

    // 推送到设备上的 server 路径
    QString server_remote_path_ = QStringLiteral("/data/local/tmp/scrcpy-server.jar");

    // 服务端版本号，必须和推上去的那个 jar 一致
    QString server_version_ = QStringLiteral("3.3.3");

    // 服务端日志级别：verbose / debug / info / warn / error
    QString log_level_ = QStringLiteral("debug");

    // 视频比特率，单位 bit/s
    quint32 bit_rate_ = 8000000;

    // 长边压到这个像素；0 表示原始分辨率
    quint16 max_size_ = 720;

    // 最大帧率；0 不限制（仅 Android 10 以上生效）
    quint32 max_fps_ = 0;

    // 采集方向锁定：0 不锁，1 锁指定方向，2 锁原始方向
    int capture_orientation_lock_ = 0;

    // 采集方向，单位度：0 / 90 / 180 / 270
    int capture_orientation_ = 0;

    // 视频裁剪，"宽:高:x:y"，空表示不裁剪
    QString crop_;

    // 设备端是否接收键鼠控制
    bool control_ = true;

    // 是否让设备保持常亮
    bool stay_awake_ = false;

    // 编码器选项，例如 "profile=1,level=2"
    QString codec_options_;

    // 指定编码器名称，例如 "OMX.qcom.video.encoder.avc"
    QString encoder_name_;

    // socket 名后缀；0xFFFFFFFF 表示用服务端默认，连接层会自己抽一个
    quint32 scid_ = 0xFFFFFFFFu;

    // true：先 reverse，失败再降级 forward
    bool use_reverse_ = true;

    // 由连接流程决定，不在界面上配
    bool tunnel_forward_ = false;

    // 本机监听/转发端口
    quint16 local_port_ = 27183;
};

// 拼 adb shell 后面那一串参数（不含 adb 本身，也不含 -s）：
//
//   CLASSPATH=/data/local/tmp/scrcpy-server.jar
//   app_process / com.genymobile.scrcpy.Server 3.3.3
//   log_level=debug video_bit_rate=8000000 max_size=720 capture_orientation=0 audio=false
//
// 原则：能用服务端默认值的一律不传。三星某些机型传参过长会直接
// "stack corruption detected"，少传参数是最省事的规避方式。
class ServerArgs
{
public:
    static QStringList Build(const ServerParams &params);
};

} // namespace qsc::session
