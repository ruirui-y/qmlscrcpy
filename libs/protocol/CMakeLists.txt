# 协议层：只依赖 QtCore，不碰窗口、网络、FFmpeg。
# 头文件和源文件平铺在同一目录，不分子目录。
add_library(qsc_protocol STATIC
    control_message.cpp
    device_message.cpp
    video_packet.cpp
    control_message.h
    device_message.h
    video_packet.h
    wire.h
)

# include 根设为 libs/：调用方统一写 #include "protocol/control_message.h"，
# 一眼看出来自哪个模块，以后加 session/ 也不会撞名
target_include_directories(qsc_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# PUBLIC：链接本库的人自动拿到 Qt5::Core 和我们的编译选项，不用每个 target 重复写
target_link_libraries(qsc_protocol PUBLIC Qt5::Core qs_options)
