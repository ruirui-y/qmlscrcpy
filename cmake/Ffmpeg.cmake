# FFmpeg 预编译包（4.4，avcodec 58）放在 third_party/ffmpeg：
#   include/      头文件
#   lib/x64/      导入库
#   bin/x64/      运行时 DLL
#
# 用 INTERFACE 目标包起来，谁链接谁继承头文件路径和导入库，不用每个 target 重复写。
set(QS_FFMPEG_ROOT "${PROJECT_SOURCE_DIR}/third_party/ffmpeg")

if(NOT EXISTS "${QS_FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "[QmlScrcpy] 缺 third_party/ffmpeg，P3 起的视频链路需要它")
endif()

add_library(qs_ffmpeg INTERFACE)
target_include_directories(qs_ffmpeg INTERFACE "${QS_FFMPEG_ROOT}/include")
target_link_directories(qs_ffmpeg INTERFACE "${QS_FFMPEG_ROOT}/lib/x64")

# qs_options 一定要带上：里面有 /utf-8。少了它 MSVC 会按本地代码页（936）读源文件，
# 中文注释能把后面的代码吃掉，报一堆莫名其妙的"缺少函数标题"。
target_link_libraries(qs_ffmpeg INTERFACE avcodec avutil swscale qs_options)

# 把运行时 DLL 拷到目标的输出目录。
#
# 注意：这份 DLL 是 release（/MD）编的，我们的 Debug 构建是 /MDd。
# 跨 DLL 分配的内存只能由分配方释放 —— AVFrame / AVPacket 必须用
# av_frame_free / av_packet_free，绝不能 delete[]，否则在两套 CRT 上换堆。
function(qs_deploy_ffmpeg target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QS_FFMPEG_ROOT}/bin/x64/avcodec-58.dll"
            "${QS_FFMPEG_ROOT}/bin/x64/avutil-56.dll"
            "${QS_FFMPEG_ROOT}/bin/x64/swscale-5.dll"
            "$<TARGET_FILE_DIR:${target}>"
        COMMENT "拷贝 FFmpeg 运行库"
        VERBATIM
    )
endfunction()
