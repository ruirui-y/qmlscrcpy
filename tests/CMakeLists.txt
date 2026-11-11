find_package(Qt5 5.15 REQUIRED COMPONENTS Test)

# 每个测试文件一个可执行文件，ctest 里按名字单独跑
add_executable(tst_protocol tst_protocol.cpp)
target_link_libraries(tst_protocol PRIVATE qsc_protocol Qt5::Test)
add_test(NAME protocol COMMAND tst_protocol)

add_executable(tst_adb tst_adb.cpp)
target_link_libraries(tst_adb PRIVATE qsc_adb Qt5::Test)
add_test(NAME adb COMMAND tst_adb)

add_executable(tst_server_args tst_server_args.cpp)
target_link_libraries(tst_server_args PRIVATE qsc_session Qt5::Test)
add_test(NAME server_args COMMAND tst_server_args)

# 真机用例：没设 QS_DEVICE_SERIAL 时自己跳过
add_executable(tst_server_session tst_server_session.cpp)
target_link_libraries(tst_server_session PRIVATE qsc_session Qt5::Test)
target_compile_definitions(tst_server_session PRIVATE
    QS_DEFAULT_SERVER_PATH="${PROJECT_SOURCE_DIR}/third_party/scrcpy-server")
add_test(NAME server_session COMMAND tst_server_session)

# 预编译 FFmpeg 的护栏：版本、H.264 解码器、上下文能不能开
add_executable(tst_ffmpeg tst_ffmpeg.cpp)
target_link_libraries(tst_ffmpeg PRIVATE qs_ffmpeg Qt5::Test)
add_test(NAME ffmpeg COMMAND tst_ffmpeg)
qs_deploy_ffmpeg(tst_ffmpeg)

# 真机解码用例：拿真实视频流喂 H264Decoder，确认能解出 YUV420P 帧
add_executable(tst_stream tst_stream.cpp)
target_link_libraries(tst_stream PRIVATE qsc_stream qsc_session Qt5::Test)
target_compile_definitions(tst_stream PRIVATE
    QS_DEFAULT_SERVER_PATH="${PROJECT_SOURCE_DIR}/third_party/scrcpy-server")
add_test(NAME stream COMMAND tst_stream)
qs_deploy_ffmpeg(tst_stream)

# 测试程序运行时也要能找到 Qt5Core.dll；path_list_prepend 是 CMake 3.22 的写法，
# 不用手动转义 PATH 里的分号
set_tests_properties(protocol adb server_args server_session ffmpeg stream PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QS_QT_ROOT}/bin")
