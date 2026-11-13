#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>

#include "base/hotkey.h"

namespace qsc
{

// 全局热键：老板键用。注册后即使程序没有焦点也能收到按键。
class BossKey : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    explicit BossKey(QObject *parent = nullptr);
    ~BossKey() override;

    // 注册热键；同一个对象再调一次会先摘掉旧的热键
    bool Bind(const qsc::base::HotkeySpec &spec);

    void Unbind();

    bool IsBound() const;

signals:
    // 用户按下了热键（不管哪个线程装的，都在 GUI 线程发出来）
    void SigTriggered();

protected:
    bool nativeEventFilter(const QByteArray &event_type, void *message, long *result) override;

private:
    bool bound_ = false;                                                                        // 当前有没有注册成功
};

} // namespace qsc
