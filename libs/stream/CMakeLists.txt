# 视频链路：读包、解码、转 RGB、帧所有权。
# 只依赖 QtCore + Qt6::Network（读 socket）+ 协议层（包头）+ FFmpeg，不碰界面。
add_library(qsc_stream STATIC
    decode_probe.cpp
    decode_probe.h
    h264_decoder.cpp
    video_reader.cpp
    video_traffic.cpp
    h264_decoder.h
    video_reader.h
    video_traffic.h
    frame.h
)

target_include_directories(qsc_stream PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(qsc_stream PUBLIC qsc_base qsc_protocol qs_ffmpeg Qt6::Core Qt6::Gui Qt6::Network)

# 硬解要自己建 D3D11 设备（避开虚拟适配器），这两个库只有 Windows 上有
if(WIN32)
    target_link_libraries(qsc_stream PUBLIC d3d11 dxgi)
endif()
