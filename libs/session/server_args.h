#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace qsc::session {

// 拉起安卓端 scrcpy-server 需要的参数。
// 字段名尽量和服务器那边的 key=value 参数名对齐，方便对着它的文档核对。
struct ServerParams
{
    QString serial_;                                                                            // 设备序列号；只有一台设备时可以为空
    QString server_local_path_;                                                                 // 本机 scrcpy-server 文件路径；空表示用程序目录下的
    QString server_remote_path_ = QStringLiteral("/data/local/tmp/scrcpy-server.jar");          // 推送到设备上的路径
    QString server_version_ = QStringLiteral("3.3.3");                                          // 服务端版本号，必须和推上去的 jar 一致
    QString log_level_ = QStringLiteral("debug");                                               // 服务端日志级别
    quint32 bit_rate_ = 8000000;                                                                // 视频比特率，bit/s
    quint16 max_size_ = 720;                                                                    // 长边压到这个像素；0 表示原始分辨率
    quint32 max_fps_ = 0;                                                                       // 最大帧率；0 不限制（仅 Android 10 以上生效）
    int capture_orientation_lock_ = 0;                                                          // 采集方向锁定：0 不锁，1 锁指定方向，2 锁原始方向
    int capture_orientation_ = 0;                                                               // 采集方向，单位度：0 / 90 / 180 / 270
    QString crop_;                                                                              // 视频裁剪，"宽:高:x:y"，空表示不裁剪
    bool control_ = true;                                                                       // 设备端是否接收键鼠控制
    bool stay_awake_ = false;                                                                   // 是否让设备保持常亮
    QString codec_options_;                                                                     // 编码器选项，例如 "profile=1,level=2"
    QString encoder_name_;                                                                      // 指定编码器名称，例如 "OMX.qcom.video.encoder.avc"
    quint32 scid_ = 0xFFFFFFFFu;                                                                // socket 名后缀；0xFFFFFFFF 表示由连接层自己抽一个
    bool use_reverse_ = true;                                                                   // true：先 reverse，失败再降级 forward
    bool tunnel_forward_ = false;                                                               // 由连接流程决定，不在界面上配
    quint16 local_port_ = 27183;                                                                // 本机监听/转发端口
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
    static QStringList Build(const ServerParams &params);                                       // 拼出完整参数列表
};

} // namespace qsc::session
