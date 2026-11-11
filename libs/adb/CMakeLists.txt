# adb 命令层：只依赖 QtCore（QProcess + QStringList），不碰界面也不碰协议。
#
# adb.exe 本身不入库（5.8 MB，Google platform-tools 里随时下），运行时按这个顺序找：
#   1. SetAdbPath() 显式设置
#   2. 环境变量 QS_ADB_PATH
#   3. 程序目录下的 adb.exe
#   4. PATH 里的 adb
add_library(qsc_adb STATIC
    adb_args.cpp
    adb_runner.cpp
    adb_args.h
    adb_runner.h
)

target_include_directories(qsc_adb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(qsc_adb PUBLIC Qt5::Core qs_options)
