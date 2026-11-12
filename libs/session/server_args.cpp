#include "session/server_args.h"

namespace qsc::session
{

// 把参数结构体拼成 scrcpy-server 的启动参数
QStringList ServerArgs::Build(const ServerParams &params)
{
    QStringList args;

    // 前三段是 app_process 的固定套路：classpath、占位目录、主类名
    args << QStringLiteral("CLASSPATH=%1").arg(params.server_remote_path_);
    args << QStringLiteral("app_process");
    args << QStringLiteral("/");
    args << QStringLiteral("com.genymobile.scrcpy.Server");
    args << params.server_version_;

    if (!params.log_level_.isEmpty())
    {
        args << QStringLiteral("log_level=%1").arg(params.log_level_);
    }

    args << QStringLiteral("video_bit_rate=%1").arg(params.bit_rate_);

    // 0 表示不缩放，交给服务端默认，别传
    if (params.max_size_ > 0)
    {
        args << QStringLiteral("max_size=%1").arg(params.max_size_);
    }
    if (params.max_fps_ > 0)
    {
        args << QStringLiteral("max_fps=%1").arg(params.max_fps_);
    }

    // 采集方向：带 @ 表示锁定，@ 后面没数字表示锁原始方向
    if (params.capture_orientation_lock_ == 1)
    {
        args << QStringLiteral("capture_orientation=@%1").arg(params.capture_orientation_);
    }
    else if (params.capture_orientation_lock_ == 2)
    {
        args << QStringLiteral("capture_orientation=@");
    }
    else
    {
        args << QStringLiteral("capture_orientation=%1").arg(params.capture_orientation_);
    }

    // 只有 forward 模式才需要：让服务端连本机，而不是本机连服务端
    if (params.tunnel_forward_)
    {
        args << QStringLiteral("tunnel_forward=true");
    }
    if (!params.crop_.isEmpty())
    {
        args << QStringLiteral("crop=%1").arg(params.crop_);
    }
    if (!params.control_)
    {
        args << QStringLiteral("control=false");
    }
    if (params.stay_awake_)
    {
        args << QStringLiteral("stay_awake=true");
    }
    if (!params.codec_options_.isEmpty())
    {
        args << QStringLiteral("codec_options=%1").arg(params.codec_options_);
    }
    if (!params.encoder_name_.isEmpty())
    {
        args << QStringLiteral("encoder_name=%1").arg(params.encoder_name_);
    }

    // 这一版不做音频转发，固定关掉
    args << QStringLiteral("audio=false");

    // 不传 scid 时服务端用默认值；传了就定宽 8 位十六进制，和 socket 名一致
    if (params.scid_ != 0xFFFFFFFFu)
    {
        args << QStringLiteral("scid=%1").arg(params.scid_, 8, 16, QLatin1Char('0'));
    }

    return args;
}

} // namespace qsc::session
