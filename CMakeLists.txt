cmake_minimum_required(VERSION 3.22 FATAL_ERROR)

# 版本号只在这里维护一处，main.cpp 通过编译宏拿
project(QmlScrcpy VERSION 0.1.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(Ffmpeg)

# 5.15 是硬底线：QWindow::startSystemMove() 从 5.15 才有，无边框窗口拖动靠它
find_package(Qt5 5.15 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network)
message(STATUS "[QmlScrcpy] Qt ${Qt5_VERSION} @ ${Qt5_DIR}")

# 当前这套 Qt 的根目录，部署时要用
get_filename_component(QS_QT_ROOT "${Qt5_DIR}/../../.." ABSOLUTE)

# 全 QML，不做 .ui；AUTOMOC 先开着，P1 加 QObject 类型时才能自动生成 moc
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)

# 与界面无关、能单独单测的库
add_subdirectory(libs)

option(QS_BUILD_TESTS "构建单元测试（ctest）" ON)
if(QS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

add_subdirectory(src)
