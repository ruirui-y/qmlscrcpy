#pragma once

namespace qsc::base
{

// 控制台窗口开关。exe 是控制台子系统（QS_HIDE_CONSOLE=OFF）时才有这扇窗；
// 改成 GUI 子系统后这两个函数都会变成空操作。
bool ConsoleWindowAvailable();

// 显示或隐藏控制台窗口；没有控制台时什么都不做
void SetConsoleWindowVisible(bool visible);

} // namespace qsc::base
