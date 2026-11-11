# 视频链路：读包、解码、帧所有权。
# 只依赖 QtCore + FFmpeg，不碰界面；上屏是 ui/ 那边的事。
add_library(qsc_stream STATIC
    h264_decoder.cpp
    h264_decoder.h
    frame.h
)

target_include_directories(qsc_stream PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(qsc_stream PUBLIC qs_ffmpeg Qt5::Core)
