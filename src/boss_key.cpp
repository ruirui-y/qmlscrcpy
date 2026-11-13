#include "boss_key.h"

#include <QDebug>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    include <windows.h>
#endif

namespace qsc
{

namespace
{

constexpr int kHotkeyId = 0x0B05;                                                               // 随便挑一个不冲突的 id，B0SS 的谐音

} // namespace

BossKey::BossKey(QObject *parent) : QObject(parent)
{
}

BossKey::~BossKey()
{
    Unbind();
}

// 注册全局热键；重复调用会先摘掉旧的
bool BossKey::Bind(const qsc::base::HotkeySpec &spec)
{
    Unbind();

#ifdef _WIN32
    // MOD_NOREPEAT：按住不放只触发一次，避免来回抖动
    const bool ok = ::RegisterHotKey(nullptr, kHotkeyId, spec.modifiers_ | MOD_NOREPEAT,
                                     spec.virtual_key_) != FALSE;
    if (!ok)
    {
        qWarning() << "[boss] 注册热键失败，可能被别的程序占用了";
        return false;
    }
    bound_ = true;
    return true;
#else
    Q_UNUSED(spec)
    return false;
#endif
}

// 摘掉热键，程序退出前必须调用，否则这个组合键会被系统认为仍被占用
void BossKey::Unbind()
{
#ifdef _WIN32
    if (bound_)
    {
        ::UnregisterHotKey(nullptr, kHotkeyId);
    }
#endif
    bound_ = false;
}

bool BossKey::IsBound() const
{
    return bound_;
}

// WM_HOTKEY 到了就发信号；其余消息不管
bool BossKey::nativeEventFilter(const QByteArray &event_type, void *message, long *result)
{
    Q_UNUSED(result)
#ifdef _WIN32
    if (event_type == QByteArrayLiteral("windows_generic_MSG")
        || event_type == QByteArrayLiteral("windows_dispatcher_MSG"))
    {
        MSG *msg = static_cast<MSG *>(message);
        if (msg != nullptr && msg->message == WM_HOTKEY && static_cast<int>(msg->wParam) == kHotkeyId)
        {
            emit SigTriggered();
            return true;                                                                        // 吃掉，别再往窗口发
        }
    }
#else
    Q_UNUSED(event_type)
    Q_UNUSED(message)
#endif
    return false;
}

} // namespace qsc
