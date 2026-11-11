# 协议层：只依赖 QtCore，不碰窗口、网络、FFmpeg。
# 单独成库是为了让 tests/ 能直接跑，不用起整个程序，也不用连设备。
add_library(qsc_protocol STATIC
    src/ControlMessage.cpp
    src/DeviceMessage.cpp
    src/VideoPacket.cpp
    include/qsc/protocol/ControlMessage.h
    include/qsc/protocol/DeviceMessage.h
    include/qsc/protocol/VideoPacket.h
    include/qsc/protocol/Wire.h
)

target_include_directories(qsc_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC：链接本库的人自动拿到 Qt5::Core 和我们的编译选项，不用每个 target 重复写
target_link_libraries(qsc_protocol PUBLIC Qt5::Core qs_options)
