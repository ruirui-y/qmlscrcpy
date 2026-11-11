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

# 测试程序运行时也要能找到 Qt5Core.dll；path_list_prepend 是 CMake 3.22 的写法，
# 不用手动转义 PATH 里的分号
set_tests_properties(protocol adb server_args server_session PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QS_QT_ROOT}/bin")
