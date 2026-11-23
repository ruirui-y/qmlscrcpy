# FFmpeg 预编译包（8.1，avcodec 62）放在 third_party/ffmpeg：
#   include/      头文件
#   lib/x64/      导入库
#   bin/x64/      运行时 DLL
#
# 4.4 那份没有编 d3d11va（avutil 只导入 bcrypt/KERNEL32，没有 d3d11/dxgi），
# 硬解根本建不出设备，所以换成 BtbN 的 lgpl-shared 构建，它带 d3d11va/d3d12va/dxva2/qsv/cuda/vulkan。
# 8.1 为 D3D12 改造升级（d3d12va）；6.1.3 备份在 .refs/ffmpeg-6.1.3-backup（不入库）。
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
#
# 整目录扫一遍再拷：6.x 的 avcodec 还依赖 swresample，写死文件名漏一个就启动报
# "找不到 xxx.dll"，而且换版本时编号又会变，干脆不写死。
file(GLOB QS_FFMPEG_DLLS "${QS_FFMPEG_ROOT}/bin/x64/*.dll")

function(qs_deploy_ffmpeg target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${QS_FFMPEG_DLLS}
            "$<TARGET_FILE_DIR:${target}>"
        COMMENT "拷贝 FFmpeg 运行库（${QS_FFMPEG_DLLS} 个）"
        VERBATIM
    )
endfunction()
