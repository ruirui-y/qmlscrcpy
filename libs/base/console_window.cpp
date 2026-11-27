#include "base/console_window.h"

#include <QtGlobal>

#ifdef _WIN32
#    include <windows.h>
#endif

namespace qsc::base
{

// 显示或隐藏控制台窗口；没有控制台时什么都不做
void SetConsoleWindowVisible(bool visible)
{
#ifdef _WIN32
    HWND console = ::GetConsoleWindow();
    if (console == nullptr)
    {
        return;
    }
    ::ShowWindow(console, visible ? SW_SHOW : SW_HIDE);
#else
    Q_UNUSED(visible)
#endif
}

} // namespace qsc::base
