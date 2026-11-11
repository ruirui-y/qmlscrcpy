# 可独立编译、可单测的库都放这里。每个子目录一个静态库，只依赖它真正需要的 Qt 模块。
add_subdirectory(protocol)
add_subdirectory(adb)
add_subdirectory(session)
