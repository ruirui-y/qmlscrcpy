#include "window_keeper.h"

#include "app_settings.h"
#include "base/hotkey.h"
#include "boss_key.h"

#include <QKeySequence>
#include <QTimer>
#include <QWindow>

namespace qsc::app
{

WindowKeeper::WindowKeeper(QObject *parent)
    : QObject(parent)
{
}

QString WindowKeeper::BossKey() const
{
    return AppSettings().BossKeyText();
}

void WindowKeeper::SetBossKeyHost(QObject *host)
{
    boss_key_host_ = host;
}

// 改老板键：解析成功就重新注册并写进 window.ini，失败时把旧键装回去
bool WindowKeeper::SetBossKey(const QString &text)
{
    qsc::base::HotkeySpec spec;
    if (!qsc::base::ParseHotkey(text, &spec))
    {
        emit SigStatus(QStringLiteral("老板键格式不对：%1（示例 Ctrl+Alt+B）").arg(text));
        return false;
    }

    auto *host = qobject_cast<qsc::BossKey *>(boss_key_host_.data());
    if (host == nullptr)
    {
        emit SigStatus(QStringLiteral("老板键组件没就绪，重启程序再试"));
        return false;
    }

    // Bind 先摘旧热键，新键失败时要把旧键装回去，否则用户会失去老板键
    const QString previous = AppSettings().BossKeyText();
    qsc::base::HotkeySpec previous_spec;
    if (!host->Bind(spec))
    {
        if (qsc::base::ParseHotkey(previous, &previous_spec))
        {
            host->Bind(previous_spec);
        }
        emit SigStatus(QStringLiteral("%1 可能被别的程序占用，已保留 %2").arg(qsc::base::HotkeyText(spec), previous));
        return false;
    }

    AppSettings().SetBossKeyText(qsc::base::HotkeyText(spec));
    emit SigBossKeyChanged();
    emit SigStatus(QStringLiteral("老板键已改为 %1").arg(qsc::base::HotkeyText(spec)));
    return true;
}

QString WindowKeeper::Shortcut(const QString &name) const
{
    return AppSettings().Shortcut(name);
}

// 空串表示禁用，非空必须是合法的 QKeySequence
bool WindowKeeper::SetShortcut(const QString &name, const QString &sequence)
{
    const QString trimmed = sequence.trimmed();
    if (!trimmed.isEmpty() && QKeySequence(trimmed).isEmpty())
    {
        emit SigStatus(QStringLiteral("快捷键格式不对：%1").arg(sequence));
        return false;
    }

    AppSettings().SetShortcut(name, trimmed);
    emit SigShortcutsChanged();
    emit SigStatus(trimmed.isEmpty() ? QStringLiteral("已禁用该快捷键")
                                     : QStringLiteral("快捷键已改为 %1").arg(trimmed));
    return true;
}

// QML 把窗口登记进来：投屏窗在主窗内部，QWindow::parent 非空，不在 topLevelWindows() 里
void WindowKeeper::Register(QObject *window)
{
    QWindow *raw = qobject_cast<QWindow *>(window);
    if (raw == nullptr)
    {
        return;
    }
    for (const QPointer<QWindow> &existing : app_windows_)
    {
        if (existing == raw)
        {
            return;
        }
    }
    app_windows_.append(raw);
}

// 老板键：第一次按藏起所有登记过的窗口，再按恢复
void WindowKeeper::ToggleBossKey()
{
    if (!boss_hidden_)
    {
        hidden_windows_.clear();
        for (const QPointer<QWindow> &window : app_windows_)
        {
            if (!window.isNull() && window->isVisible())
            {
                hidden_windows_.append(window);
                window->hide();
            }
        }
        boss_hidden_ = true;
        emit SigStatus(QStringLiteral("已隐藏 %1 个窗口，再按一次老板键恢复").arg(hidden_windows_.size()));

        // Qt 会因为激活/曝光事件把窗口重新显示一次，等下一轮事件循环再补一刀
        QTimer::singleShot(0, this, [this]()
        {
            if (!boss_hidden_)
            {
                return;
            }
            for (const QPointer<QWindow> &window : hidden_windows_)
            {
                if (!window.isNull() && window->isVisible())
                {
                    window->hide();
                }
            }
        });
        return;
    }

    for (const QPointer<QWindow> &window : hidden_windows_)
    {
        if (!window.isNull())
        {
            window->show();
        }
    }
    hidden_windows_.clear();
    boss_hidden_ = false;
    emit SigStatus(QStringLiteral("窗口已恢复"));
}

// 断线时兜底：老板键藏起来的窗口要放回去，否则用户找不到界面
void WindowKeeper::RestoreIfHidden()
{
    if (!boss_hidden_)
    {
        return;
    }
    for (const QPointer<QWindow> &window : hidden_windows_)
    {
        if (!window.isNull())
        {
            window->show();
        }
    }
    hidden_windows_.clear();
    boss_hidden_ = false;
}

} // namespace qsc::app
