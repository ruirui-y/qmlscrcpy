#include "playback_settings.h"

#include "app_settings.h"

namespace qsc::app
{

PlaybackSettings::PlaybackSettings(QObject *parent)
    : QObject(parent)
{
}

void PlaybackSettings::SetProfile(const QString &profile)
{
    if (profile.isEmpty() || profile_ == profile)
    {
        return;
    }
    profile_ = profile;
    emit SigVideoChanged();                                                                     // 换档位后码率/帧率跟着变
}

int PlaybackSettings::BitRate() const
{
    return AppSettings().VideoBitRate(profile_);
}

int PlaybackSettings::MaxFps() const
{
    return AppSettings().VideoMaxFps(profile_);
}

// 写进配置：码率和帧率是 server 启动参数，真正生效要重连，由调用方负责
void PlaybackSettings::ApplyVideo(int bit_rate, int max_fps)
{
    AppSettings settings;
    settings.SetVideoBitRate(bit_rate, profile_);
    settings.SetVideoMaxFps(max_fps, profile_);
    emit SigVideoChanged();
}

bool PlaybackSettings::Hardware() const
{
    return AppSettings().DecodeHardware();
}

void PlaybackSettings::SetHardware(bool hardware)
{
    if (Hardware() == hardware)
    {
        return;
    }
    AppSettings().SetDecodeHardware(hardware);
    emit SigDecodeChanged();
}

int PlaybackSettings::Threads() const
{
    return AppSettings().DecodeThreads();
}

void PlaybackSettings::SetThreads(int threads)
{
    const int value = qBound(1, threads, 8);
    if (Threads() == value)
    {
        return;
    }
    AppSettings().SetDecodeThreads(value);
    emit SigDecodeChanged();
}

} // namespace qsc::app
