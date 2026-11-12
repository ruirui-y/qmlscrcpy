find_package(Qt5 5.15 REQUIRED COMPONENTS Test)

# 测试程序直接双击或在普通终端里跑时，Qt 的 DLL 会从 PATH 里捞 —— 本机 PATH 第一个是
# Qt 5.14.2，捞到它和 5.15.2 编出来的二进制混用就是堆损坏（0xC0000374）。
# 把用到的几个 DLL 拷到和 exe 同目录：同目录优先于 PATH。
function(qs_deploy_qt_runtime target)
    foreach(dll_name Qt5Cored Qt5Guid Qt5Networkd Qt5Testd)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QS_QT_ROOT}/bin/${dll_name}.dll"
                "$<TARGET_FILE_DIR:${target}>"
            COMMENT "拷贝 ${dll_name}.dll（避免捞到 5.14.2）"
            VERBATIM
        )
    endforeach()
endfunction()

# 每个测试文件一个可执行文件，ctest 里按名字单独跑
add_executable(tst_protocol tst_protocol.cpp)
target_link_libraries(tst_protocol PRIVATE qsc_protocol Qt5::Test)
add_test(NAME protocol COMMAND tst_protocol)
qs_deploy_qt_runtime(tst_protocol)

add_executable(tst_adb tst_adb.cpp)
target_link_libraries(tst_adb PRIVATE qsc_adb Qt5::Test)
add_test(NAME adb COMMAND tst_adb)
qs_deploy_qt_runtime(tst_adb)

add_executable(tst_server_args tst_server_args.cpp)
target_link_libraries(tst_server_args PRIVATE qsc_session Qt5::Test)
add_test(NAME server_args COMMAND tst_server_args)
qs_deploy_qt_runtime(tst_server_args)

# 真机用例：没设 QS_DEVICE_SERIAL 时自己跳过
add_executable(tst_server_session tst_server_session.cpp)
target_link_libraries(tst_server_session PRIVATE qsc_session Qt5::Test)
target_compile_definitions(tst_server_session PRIVATE
    QS_DEFAULT_SERVER_PATH="${PROJECT_SOURCE_DIR}/third_party/scrcpy-server")
add_test(NAME server_session COMMAND tst_server_session)
qs_deploy_qt_runtime(tst_server_session)

# 预编译 FFmpeg 的护栏：版本、H.264 解码器、上下文能不能开，外加一次合成帧的 sws 转换
add_executable(tst_ffmpeg tst_ffmpeg.cpp)
target_link_libraries(tst_ffmpeg PRIVATE qsc_stream Qt5::Test)
add_test(NAME ffmpeg COMMAND tst_ffmpeg)
qs_deploy_qt_runtime(tst_ffmpeg)
qs_deploy_ffmpeg(tst_ffmpeg)

# 真机解码用例：拿真实视频流喂 H264Decoder，确认能解出 YUV420P 帧
add_executable(tst_stream tst_stream.cpp)
target_link_libraries(tst_stream PRIVATE qsc_stream qsc_session Qt5::Test)
target_compile_definitions(tst_stream PRIVATE
    QS_DEFAULT_SERVER_PATH="${PROJECT_SOURCE_DIR}/third_party/scrcpy-server")
add_test(NAME stream COMMAND tst_stream)
qs_deploy_qt_runtime(tst_stream)
qs_deploy_ffmpeg(tst_stream)

# 注入逻辑：坐标换算和触摸消息布局，纯逻辑不需要设备
add_executable(tst_input tst_input.cpp)
target_link_libraries(tst_input PRIVATE qsc_input Qt5::Test)
add_test(NAME input COMMAND tst_input)
qs_deploy_qt_runtime(tst_input)

# 键盘映射：Qt 键 → 安卓键码、metaState、自动重复计数
add_executable(tst_key tst_key.cpp)
target_link_libraries(tst_key PRIVATE qsc_input Qt5::Test)
add_test(NAME key COMMAND tst_key)
qs_deploy_qt_runtime(tst_key)

# 老板键配置解析：纯字符串处理
add_executable(tst_version tst_version.cpp)
target_link_libraries(tst_version PRIVATE qsc_update Qt5::Test)
add_test(NAME version COMMAND tst_version)
qs_deploy_qt_runtime(tst_version)

add_executable(tst_hotkey tst_hotkey.cpp)
target_link_libraries(tst_hotkey PRIVATE qsc_base Qt5::Test)
add_test(NAME hotkey COMMAND tst_hotkey)
qs_deploy_qt_runtime(tst_hotkey)

# 无线地址解析与排序：纯字符串处理
add_executable(tst_net_address tst_net_address.cpp)
target_link_libraries(tst_net_address PRIVATE qsc_adb Qt5::Test)
add_test(NAME net_address COMMAND tst_net_address)
qs_deploy_qt_runtime(tst_net_address)

set_tests_properties(protocol adb server_args server_session ffmpeg stream input key net_address version hotkey PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QS_QT_ROOT}/bin")
