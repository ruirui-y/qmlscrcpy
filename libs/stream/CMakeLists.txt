# 视频链路：读包、解码、转 RGB、帧所有权。
# 只依赖 QtCore + Qt5::Network（读 socket）+ 协议层（包头）+ FFmpeg，不碰界面。
add_library(qsc_stream STATIC
    h264_decoder.cpp
    video_reader.cpp
    yuv_convert.cpp
    h264_decoder.h
    video_reader.h
    yuv_convert.h
    frame.h
)

target_include_directories(qsc_stream PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(qsc_stream PUBLIC qsc_protocol qs_ffmpeg Qt5::Core Qt5::Gui Qt5::Network)
