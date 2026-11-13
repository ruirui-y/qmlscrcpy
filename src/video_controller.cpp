#include "video_controller.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QMetaObject>
#include <QQuickWindow>

#include "adb/adb_runner.h"
#include "app_settings.h"
#include "base/log.h"
#include "base/task_runner.h"
#include "control_writer.h"
#include "protocol/control_message.h"
#include "ui/video_item.h"

VideoController::VideoController(ControlWriter *writer, QObject *parent)
    : QObject(parent)
    , writer_(writer)
{
    // 滚轮设置从这里读一次，之后界面改了直接写回配置
    AppSettings settings;
    wheel_mode_ = settings.WheelMode();
    drag_pixels_ = settings.DragPixels();
}

QString VideoController::WheelMode() const
{
    return wheel_mode_;
}

int VideoController::DragPixels() const
{
    return drag_pixels_;
}

VideoItem *VideoController::Surface() const
{
    return surface_;
}

// QML 把画布交进来：输入链路从这里接上
void VideoController::SetSurface(QObject *surface)
{
    surface_ = qobject_cast<VideoItem *>(surface);
    if (surface_ == nullptr)
    {
        qWarning() << "[video] 传进来的不是 VideoSurface";
        return;
    }

    // 画布把鼠标键盘翻成控制消息，这里负责写进控制 socket
    QObject::connect(surface_, &VideoItem::SigControlMessageReady, this,
                     &VideoController::OnSurfaceControlMessage);
    surface_->SetWheelMode(wheel_mode_);
    surface_->SetDragPixels(drag_pixels_);
}

// 画布翻好的输入消息，转手交给会话层写出去
void VideoController::OnSurfaceControlMessage(const QByteArray &message)
{
    if (writer_ != nullptr)
    {
        writer_->WriteControl(message);
    }
}

// 发一次按键：按下和抬起各一条，安卓才会真正触发
void VideoController::SendKeycode(quint32 keycode)
{
    if (writer_ == nullptr || !writer_->IsConnected())
    {
        return;
    }

    qsc::protocol::KeycodeEvent event;
    event.keycode_ = keycode;
    event.action_ = 0;
    writer_->WriteControl(qsc::protocol::ControlMessage::InjectKeycode(event));
    event.action_ = 1;
    writer_->WriteControl(qsc::protocol::ControlMessage::InjectKeycode(event));
}

// 音量加：走安卓的 KEYCODE_VOLUME_UP（24）
void VideoController::SendVolumeUp()
{
    SendKeycode(24);
}

// 音量减：KEYCODE_VOLUME_DOWN（25）
void VideoController::SendVolumeDown()
{
    SendKeycode(25);
}

// 返回上一级：走专门的消息，它在设备息屏时还能顺手点亮屏幕
void VideoController::SendBack()
{
    if (writer_ == nullptr || !writer_->IsConnected())
    {
        return;
    }
    writer_->WriteControl(qsc::protocol::ControlMessage::BackOrScreenOn(true));
    writer_->WriteControl(qsc::protocol::ControlMessage::BackOrScreenOn(false));
}

// 回桌面：安卓 KEYCODE_HOME（3）
void VideoController::SendHome()
{
    SendKeycode(3);
}

// 按一下电源键：KEYCODE_POWER（26），锁屏或点亮屏幕
void VideoController::SendPower()
{
    SendKeycode(26);
}

// 多任务切换：KEYCODE_APP_SWITCH（187）
void VideoController::SendRecents()
{
    SendKeycode(187);
}

// 让设备转屏幕：一条无负载消息
void VideoController::RotateDevice()
{
    if (writer_ == nullptr || !writer_->IsConnected())
    {
        return;
    }
    writer_->WriteControl(qsc::protocol::ControlMessage::RotateDevice());
}

// 息屏或亮屏：只关显示，不动锁屏状态
void VideoController::SetDisplayPower(bool on)
{
    if (writer_ == nullptr || !writer_->IsConnected())
    {
        return;
    }
    writer_->WriteControl(qsc::protocol::ControlMessage::SetDisplayPower(on));
    writer_->ReportStatus(on ? QStringLiteral("已亮屏") : QStringLiteral("已息屏"));
}

// 切换滚轮发法：立即推给画布，同时记住
void VideoController::SetWheelMode(const QString &mode)
{
    const QString normalized = mode == QStringLiteral("drag") ? QStringLiteral("drag") : QStringLiteral("scroll");
    if (wheel_mode_ == normalized)
    {
        return;
    }

    wheel_mode_ = normalized;
    if (surface_ != nullptr)
    {
        surface_->SetWheelMode(wheel_mode_);
        surface_->SetDragPixels(drag_pixels_);
    }
    AppSettings().SetWheelMode(wheel_mode_);
    emit SigWheelModeChanged();
    if (writer_ != nullptr)
    {
        writer_->ReportStatus(QStringLiteral("滚轮模式：%1")
                                  .arg(wheel_mode_ == QStringLiteral("drag") ? QStringLiteral("滑动")
                                                                             : QStringLiteral("滚动")));
    }
}

// 滑动像素加减：范围夹在 10 到 300，太小会被当点击，太大一下翻过头
void VideoController::AdjustDragPixels(int delta)
{
    const int adjusted = qBound(10, drag_pixels_ + delta, 300);
    if (adjusted == drag_pixels_)
    {
        return;
    }

    drag_pixels_ = adjusted;
    if (surface_ != nullptr)
    {
        surface_->SetDragPixels(drag_pixels_);
    }
    AppSettings().SetDragPixels(drag_pixels_);
    emit SigDragPixelsChanged();
    if (writer_ != nullptr)
    {
        writer_->ReportStatus(QStringLiteral("滑动像素：%1").arg(drag_pixels_));
    }
}

// 电脑剪贴板发给设备：写入 + 让设备直接粘贴
void VideoController::SendClipboardToDevice()
{
    if (writer_ == nullptr || !writer_->IsConnected())
    {
        writer_->ReportStatus(QStringLiteral("还没连接设备"));
        return;
    }

    const QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty())
    {
        writer_->ReportStatus(QStringLiteral("电脑剪贴板没有文字"));
        return;
    }

    ++clipboard_sequence_;
    writer_->WriteControl(qsc::protocol::ControlMessage::SetClipboard(clipboard_sequence_, true, text));
    writer_->ReportStatus(QStringLiteral("剪贴板已发给手机（%1 字），已自动粘贴").arg(text.size()));
}

// 把设备的剪贴板取到电脑
void VideoController::PullClipboardFromDevice()
{
    if (writer_ == nullptr || !writer_->IsConnected())
    {
        return;
    }

    ++clipboard_sequence_;
    writer_->WriteControl(qsc::protocol::ControlMessage::GetClipboard(qsc::protocol::ClipboardAction::Copy));
    writer_->ReportStatus(QStringLiteral("正在读取手机剪贴板 …"));
}

// 拖进投屏窗的文件：逐个 adb push 到配置的目录，进度实时刷到状态栏
void VideoController::PushFiles(const QList<QUrl> &urls)
{
    qCInfo(qsc::base::lcInput) << "[drop] 收到" << urls.size() << "个 url";

    // 先把路径抓出来并打日志：拖放传参有问题时能直接看出是哪一步空的
    QStringList paths;
    for (const QUrl &url : urls)
    {
        const QString path = url.toLocalFile();
        const bool is_file = !path.isEmpty() && QFileInfo(path).isFile();
        qCInfo(qsc::base::lcInput) << "[drop] url" << url.toString() << "本地路径" << path << "是文件" << is_file;
        if (is_file)
        {
            paths.append(path);
        }
    }

    if (paths.isEmpty())
    {
        if (writer_ != nullptr)
        {
            writer_->ReportStatus(urls.isEmpty() ? QStringLiteral("拖进来的不是文件")
                                                 : QStringLiteral("拖进来的都是文件夹，暂不支持"));
        }
        return;
    }

    if (writer_ == nullptr || !writer_->IsConnected())
    {
        if (writer_ != nullptr)
        {
            writer_->ReportStatus(QStringLiteral("还没连接设备"));
        }
        return;
    }

    const QString serial = writer_->DeviceSerial();
    const QString upload_dir = AppSettings().UploadDir();

    // 状态栏给人看，把路径换成手机上看到的叫法：/sdcard/Download/x → 下载/x
    QString display_dir = upload_dir;
    display_dir.remove(QStringLiteral("/sdcard/"));
    display_dir.replace(QStringLiteral("Download"), QStringLiteral("下载"));

    writer_->ReportStatus(QStringLiteral("正在传 %1 个文件到「%2」…").arg(paths.size()).arg(display_dir));

    // adb push 是阻塞的，整个循环丢到工作线程；进度用 Queued 调用回主线程改状态
    qsc::base::TaskRunner::Instance().Post([this, serial, paths, upload_dir, display_dir]()
    {
        int done = 0;
        for (const QString &path : paths)
        {
            const QString name = QFileInfo(path).fileName();
            QString error;
            qCInfo(qsc::base::lcInput) << "[drop] 开始 push" << path << "->" << upload_dir;
            const bool ok = qsc::adb::PushFile(serial, path, upload_dir,
                                               [this, name](int percent)
                                               {
                                                   QMetaObject::invokeMethod(this, [this, name, percent]()
                                                   {
                                                       if (writer_ != nullptr)
                                                       {
                                                           writer_->ReportStatus(
                                                               QStringLiteral("传 %1：%2%").arg(name).arg(percent));
                                                       }
                                                   }, Qt::QueuedConnection);
                                               },
                                               &error);
            if (!ok)
            {
                qCInfo(qsc::base::lcInput) << "[drop] push 失败" << name << error;
                QMetaObject::invokeMethod(this, [this, name, error]()
                {
                    if (writer_ != nullptr)
                    {
                        writer_->ReportStatus(QStringLiteral("传 %1 失败：%2").arg(name, error));
                    }
                }, Qt::QueuedConnection);
                return;
            }
            qCInfo(qsc::base::lcInput) << "[drop] push 完成" << name;
            ++done;
        }

        QMetaObject::invokeMethod(this, [this, done, display_dir]()
        {
            if (writer_ != nullptr)
            {
                writer_->ReportStatus(done == 1 ? QStringLiteral("文件已传到「%1」").arg(display_dir)
                                                : QStringLiteral("%1 个文件已传到「%2」").arg(done).arg(display_dir));
            }
        }, Qt::QueuedConnection);
    });
}

// 在手机上打开上传目录：优先厂商文件管理，退路是 AOSP 的下载界面
void VideoController::OpenUploadDir()
{
    if (writer_ == nullptr || !writer_->IsConnected())
    {
        if (writer_ != nullptr)
        {
            writer_->ReportStatus(QStringLiteral("还没连接设备"));
        }
        return;
    }

    const QString upload_dir = AppSettings().UploadDir();
    const QString serial = writer_->DeviceSerial();

    QString display_dir = upload_dir;
    display_dir.remove(QStringLiteral("/sdcard/"));
    display_dir.replace(QStringLiteral("Download"), QStringLiteral("下载"));

    // 目录要先存在，否则打开后看不到东西
    qsc::adb::Run(QStringList() << QStringLiteral("-s") << serial << QStringLiteral("shell") << QStringLiteral("mkdir")
                                << QStringLiteral("-p") << upload_dir, 5000);

    // 各家文件管理的主界面，按机型依次试；第一个能拉起来的就用它
    static const QStringList kFileManagers{
        QStringLiteral("com.coloros.filemanager/com.oplus.filemanager.main.ui.MainActivity"),    // OPPO / realme / OnePlus
        QStringLiteral("com.android.fileexplorer/.FileExplorerTabActivity"),                     // 小米
        QStringLiteral("com.huawei.hidisk/.filemanager.FileManager"),                            // 华为 / 荣耀
        QStringLiteral("com.sec.android.app.myfiles/.common.MainActivity"),                      // 三星
    };
    for (const QString &component : kFileManagers)
    {
        const qsc::adb::AdbResult opened =
            qsc::adb::Run(QStringList() << QStringLiteral("-s") << serial << QStringLiteral("shell")
                                        << QStringLiteral("am") << QStringLiteral("start") << QStringLiteral("-n")
                                        << component, 8000);
        if (opened.Ok())
        {
            writer_->ReportStatus(QStringLiteral("已打开文件管理首页，文件在「%1」").arg(display_dir));
            return;
        }
    }

    // 没有厂商文件管理就退到 AOSP 的「下载」界面
    const qsc::adb::AdbResult fallback = qsc::adb::Run(
        QStringList() << QStringLiteral("-s") << serial << QStringLiteral("shell") << QStringLiteral("am")
                      << QStringLiteral("start") << QStringLiteral("-a")
                      << QStringLiteral("android.intent.action.VIEW_DOWNLOADS"), 8000);
    writer_->ReportStatus(fallback.Ok() ? QStringLiteral("已打开「下载」界面，文件在「%1」").arg(display_dir)
                                        : QStringLiteral("打不开文件管理器：%1").arg(fallback.ErrorText()));
}

// 截图：抓投屏窗当前内容，存到 exe 旁边的 screenshots 目录，同时进剪贴板
void VideoController::SaveScreenshot()
{
    if (surface_ == nullptr || surface_->window() == nullptr)
    {
        if (writer_ != nullptr)
        {
            writer_->ReportStatus(QStringLiteral("没有画面，截不了图"));
        }
        return;
    }

    const QImage image = surface_->window()->grabWindow();
    const QString name =
        QStringLiteral("shot_%1.png").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QCoreApplication::applicationDirPath() + QStringLiteral("/screenshots/") + name;
    QDir().mkpath(QFileInfo(path).absolutePath());
    const bool saved = image.save(path);

    // 顺手放进系统剪贴板，截图就是为了贴给别人看
    QGuiApplication::clipboard()->setImage(image);

    if (writer_ != nullptr)
    {
        writer_->ReportStatus(saved ? QStringLiteral("截图已保存并复制：%1").arg(name)
                                    : QStringLiteral("截图只复制到剪贴板，保存失败"));
    }
    qCInfo(qsc::base::lcRender) << "截图" << path << saved;
}
