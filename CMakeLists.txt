cmake_minimum_required(VERSION 3.22 FATAL_ERROR)

# 版本号只在这里维护一处，main.cpp 通过编译宏拿
project(QmlScrcpy VERSION 0.4.3 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(Ffmpeg)

# 6.8 起：VideoItem 是 QQuickRhiItem，渲染后端由 RHI 决定（当前 D3D12），
# 而"指定后端"这件事要有 RHI（6.0 才有），6.5 之后 API 才稳定
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network)
message(STATUS "[QmlScrcpy] Qt ${Qt6_VERSION} @ ${Qt6_DIR}")

# 当前这套 Qt 的根目录，部署时要用（Qt6_DIR 是 <root>/lib/cmake/Qt6，退三级回到 root）
get_filename_component(QS_QT_ROOT "${Qt6_DIR}/../../.." ABSOLUTE)

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
