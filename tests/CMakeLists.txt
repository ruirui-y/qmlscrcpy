find_package(Qt5 5.15 REQUIRED COMPONENTS Test)

# 每个测试文件一个可执行文件，ctest 里按名字跑
add_executable(tst_protocol tst_protocol.cpp)
target_link_libraries(tst_protocol PRIVATE qsc_protocol Qt5::Test)

add_test(NAME protocol COMMAND tst_protocol)

# 测试程序运行时也要能找到 Qt5Core.dll；path_list_prepend 是 CMake 3.22 的写法，
# 不用手动转义 PATH 里的分号
set_tests_properties(protocol PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QS_QT_ROOT}/bin")
