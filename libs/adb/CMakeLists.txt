# 只在 Windows 上把 adb.exe 拷到输出目录，省得每次手动配 PATH。
# adb 二进制还没入库：先把 SetAdbPath / QS_ADB_PATH 当兜底，等 P2b 接真机时再把
# 官方 platform-tools 里的 adb.exe 放进 third_party/adb/win/。

add_library(qsc_adb STATIC
    src/AdbArgs.cpp
    src/AdbRunner.cpp
    include/qsc/adb/AdbArgs.h
    include/qsc/adb/AdbRunner.h
)

target_include_directories(qsc_adb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# adb 层只依赖 QtCore：QProcess + QStringList，不碰界面也不碰协议
target_link_libraries(qsc_adb PUBLIC Qt5::Core qs_options)
