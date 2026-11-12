# 日志门面：给各模块提供分类和落地文件，实现换掉不影响调用点。
# 只依赖 QtCore。
add_library(qsc_base STATIC
    console_window.cpp
    console_window.h
    hotkey.cpp
    task_runner.cpp
    hotkey.h
    task_runner.h
    log.cpp
    log.h
)

target_include_directories(qsc_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(qsc_base PUBLIC qs_options Qt5::Core)